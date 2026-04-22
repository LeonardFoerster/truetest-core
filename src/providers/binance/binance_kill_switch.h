#pragma once
#ifdef HAS_BINANCE

#include "execution/client_order_id.h"
#include "execution/live_safety.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_reconciler.h"
#include "providers/binance/binance_rest_client.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

// Live-mode shutdown path. Asks the exchange to cancel every open order on
// the configured symbol, then queries current holdings and submits a market
// SELL for any remaining base-asset balance. Returns true iff both steps
// completed before the supplied deadline.
//
// Flatten logic queries the exchange for the current base-asset balance
// rather than trusting the local portfolio — the whole point of this path
// is to be correct when something has gone wrong with local state. If the
// exchange says we hold 0 of base, flatten is a no-op.
class BinanceKillSwitch : public IKillSwitch
{
public:
    BinanceKillSwitch(std::shared_ptr<BinanceRestClient> rest,
                      std::string symbol,
                      std::string base_asset,
                      std::shared_ptr<ClientOrderIdMinter> minter)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , base_asset_(std::move(base_asset))
        , minter_(std::move(minter))
    {}

    bool cancel_all_and_flatten(std::chrono::milliseconds deadline) override
    {
        if (!rest_)
        {
            std::cerr << "BinanceKillSwitch: no REST client, cannot act\n";
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto expires_at = start + deadline;

        // Phase 1: cancel all open orders for the symbol.
        {
            const std::string params = "symbol=" + symbol_;
            auto resp = rest_->del("/api/v3/openOrders", params);
            if (resp.status < 200 || resp.status >= 300)
            {
                // -2011 ("Unknown order sent") fires when there are no open
                // orders; treat that as success. Other HTTP / exchange errors
                // are failures because residual orders can still fill.
                if (resp.body.find("-2011") == std::string::npos)
                {
                    std::cerr << "BinanceKillSwitch: cancel_all HTTP "
                              << resp.status << " - " << resp.body << "\n";
                    return false;
                }
            }
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceKillSwitch: deadline expired after cancel_all\n";
            return false;
        }

        // Phase 2: query current base-asset balance, market-sell what's there.
        double ex_base_free = 0.0, ex_base_locked = 0.0;
        {
            auto acct = rest_->get("/api/v3/account", "");
            if (acct.status < 200 || acct.status >= 300)
            {
                std::cerr << "BinanceKillSwitch: /api/v3/account HTTP "
                          << acct.status << " - " << acct.body << "\n";
                return false;
            }
            if (!BinanceReconciler::extract_balance(
                    acct.body, base_asset_, ex_base_free, ex_base_locked))
            {
                // Asset absent from account — nothing to flatten.
                return true;
            }
        }

        if (ex_base_free < 1e-12)
        {
            // Locked inventory exists (e.g. still on a stuck order) but no
            // free qty. cancel_all should have freed it; if not, the operator
            // needs to intervene. Report partial success so the warning fires.
            if (ex_base_locked > 1e-12)
            {
                std::cerr << "BinanceKillSwitch: "
                          << ex_base_locked << " " << base_asset_
                          << " still locked after cancel_all — manual "
                             "intervention required\n";
                return false;
            }
            return true;
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceKillSwitch: deadline expired before flatten\n";
            return false;
        }

        // Submit market SELL. clientOrderId makes the request idempotent
        // against transport retries.
        std::string params = "symbol=" + symbol_
            + "&side=SELL&type=MARKET&quantity="
            + format_qty(ex_base_free);
        if (minter_)
            params += "&newClientOrderId=" + minter_->next();

        auto sell = rest_->post("/api/v3/order", params);
        if (sell.status < 200 || sell.status >= 300)
        {
            std::cerr << "BinanceKillSwitch: flatten order HTTP "
                      << sell.status << " - " << sell.body << "\n";
            return false;
        }

        return true;
    }

private:
    static std::string format_qty(double q)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.8f", q);
        return buf;
    }

    std::shared_ptr<BinanceRestClient> rest_;
    std::string symbol_;
    std::string base_asset_;
    std::shared_ptr<ClientOrderIdMinter> minter_;
};

#endif // HAS_BINANCE
