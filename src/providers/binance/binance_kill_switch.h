#pragma once
#ifdef HAS_BINANCE

#include "execution/client_order_id.h"
#include "execution/live_safety.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_reconciler.h"
#include "providers/binance/binance_rest_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

// Cancel all open orders, then market-sell base-asset balance queried
// from the exchange (not local state - local may be exactly what's wrong).
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

        // Bound each REST call so a still-down LAN can't wedge shutdown.
        // See BinanceFuturesKillSwitch for the same rationale; min(1500ms,
        // deadline/3) splits the budget across cancel_all / account /
        // MARKET SELL with the wall-clock check between calls catching
        // anything that overruns.
        {
            const long long per_call_ms =
                std::min<long long>(1500, deadline.count() / 3);
            if (per_call_ms > 0)
                rest_->set_per_call_timeout(
                    std::chrono::milliseconds(per_call_ms));
        }

        {
            const std::string params = "symbol=" + symbol_;
            auto resp = rest_->del("/api/v3/openOrders", params);
            if (resp.status < 200 || resp.status >= 300)
            {
                // -2011 = "no open orders", treat as success.
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
                return true;
        }

        if (ex_base_free < 1e-12)
        {
            // Locked inventory after cancel_all means a stuck order -
            // operator must intervene. Report failure so the warning fires.
            if (ex_base_locked > 1e-12)
            {
                std::cerr << "BinanceKillSwitch: "
                          << ex_base_locked << " " << base_asset_
                          << " still locked after cancel_all - manual "
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

        // clientOrderId -> idempotent against transport retries.
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
