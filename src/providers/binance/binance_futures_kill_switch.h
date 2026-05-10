#pragma once
#ifdef HAS_BINANCE

#include "execution/client_order_id.h"
#include "execution/live_safety.h"
#include "providers/binance/binance_futures_reconciler.h"
#include "providers/binance/binance_rest_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

// Cancel all open orders, then read /fapi/v2/positionRisk and flatten
// any non-zero position with a reduceOnly MARKET on the opposite side.
// Differences from the spot kill switch:
//   - no balance walk: futures has no "free base" to sweep; closing
//     the position releases the margin automatically;
//   - signed positionAmt drives both quantity and side selection
//     (long >0 → SELL to close, short <0 → BUY to close);
//   - reduceOnly=true is mandatory — it stops the close from
//     accidentally opening an oversized opposite position if the venue
//     has already partially liquidated us.
class BinanceFuturesKillSwitch : public IKillSwitch
{
public:
    BinanceFuturesKillSwitch(std::shared_ptr<BinanceRestClient> rest,
                             std::string symbol,
                             std::shared_ptr<ClientOrderIdMinter> minter)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , minter_(std::move(minter))
    {}

    bool cancel_all_and_flatten(std::chrono::milliseconds deadline) override
    {
        if (!rest_)
        {
            std::cerr << "BinanceFuturesKillSwitch: no REST client, cannot act\n";
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto expires_at = start + deadline;

        // Bound each REST call so a still-down LAN can't wedge shutdown.
        // Three calls fit inside `deadline`; min(1500ms, deadline/3) leaves
        // slack for TLS handshake reuse hits and the wall-clock checks
        // between calls. Set once and left in place — the rest client is
        // only used for the remainder of shutdown.
        {
            const long long per_call_ms =
                std::min<long long>(1500, deadline.count() / 3);
            if (per_call_ms > 0)
                rest_->set_per_call_timeout(
                    std::chrono::milliseconds(per_call_ms));
        }

        {
            const std::string params = "symbol=" + symbol_;
            auto resp = rest_->del("/fapi/v1/allOpenOrders", params);
            if (resp.status < 200 || resp.status >= 300)
            {
                // -2011 = "no open orders" — same code, same treatment as spot.
                if (resp.body.find("-2011") == std::string::npos)
                {
                    std::cerr << "BinanceFuturesKillSwitch: cancel_all HTTP "
                              << resp.status << " - " << resp.body << "\n";
                    return false;
                }
            }
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceFuturesKillSwitch: deadline expired after "
                         "cancel_all\n";
            return false;
        }

        double position_amt = 0.0;
        {
            auto pr = rest_->get("/fapi/v2/positionRisk",
                                 "symbol=" + symbol_);
            if (pr.status < 200 || pr.status >= 300)
            {
                std::cerr << "BinanceFuturesKillSwitch: /fapi/v2/positionRisk "
                             "HTTP " << pr.status << " - " << pr.body << "\n";
                return false;
            }
            if (!BinanceFuturesReconciler::extract_position_amt(
                    pr.body, position_amt))
            {
                std::cerr << "BinanceFuturesKillSwitch: positionAmt missing "
                             "in /fapi/v2/positionRisk\n";
                return false;
            }
        }

        if (std::abs(position_amt) < 1e-12)
            return true;

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceFuturesKillSwitch: deadline expired before "
                         "flatten\n";
            return false;
        }

        // Long → SELL to close, short → BUY to close.
        const char* close_side = position_amt > 0.0 ? "SELL" : "BUY";
        std::string params = "symbol=" + symbol_
            + "&side=" + close_side
            + "&type=MARKET&reduceOnly=true&quantity="
            + format_qty(std::abs(position_amt));
        if (minter_)
            params += "&newClientOrderId=" + minter_->next();

        auto close = rest_->post("/fapi/v1/order", params);
        if (close.status < 200 || close.status >= 300)
        {
            std::cerr << "BinanceFuturesKillSwitch: flatten order HTTP "
                      << close.status << " - " << close.body << "\n";
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
    std::shared_ptr<ClientOrderIdMinter> minter_;
};

#endif // HAS_BINANCE
