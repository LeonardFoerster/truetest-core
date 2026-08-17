#pragma once
#ifdef HAS_BINANCE

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Authoritative path list: scripts/check-live-safety-freeze.sh
// ============================================================

#include "execution/client_order_id.h"
#include "execution/live_safety.h"
#include "providers/binance/binance_futures_reconciler.h"
#include "providers/binance/binance_rest_client.h"
#include "providers/recovery_payload.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <functional>
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
    using request_fn = std::function<BinanceRestClient::response(
        const std::string&, const std::string&, std::chrono::milliseconds)>;
    struct injected_requests_t {};
    static constexpr injected_requests_t injected_requests{};

    BinanceFuturesKillSwitch(std::shared_ptr<BinanceRestClient> rest,
                             std::string symbol,
                             std::shared_ptr<ClientOrderIdMinter> minter)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , minter_(std::move(minter))
    {}

    BinanceFuturesKillSwitch(injected_requests_t,
                             request_fn del,
                             request_fn get,
                             request_fn post,
                             std::string symbol,
                             std::shared_ptr<ClientOrderIdMinter> minter = {})
        : del_(std::move(del))
        , get_(std::move(get))
        , post_(std::move(post))
        , symbol_(std::move(symbol))
        , minter_(std::move(minter))
    {}

    bool cancel_all_and_flatten(std::chrono::milliseconds deadline) override
    {
        if (!rest_ && (!del_ || !get_ || !post_))
        {
            std::cerr << "BinanceFuturesKillSwitch: no REST client, cannot act\n";
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto expires_at = start + deadline;

        const auto remaining = [&]() {
            const auto now = std::chrono::steady_clock::now();
            if (now >= expires_at) return std::chrono::milliseconds{0};
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                expires_at - now);
        };
        bool all_steps_succeeded = true;

        {
            const std::string params = "symbol=" + binance::url_encode(symbol_);
            try
            {
                auto resp = request_del(
                    "/fapi/v1/allOpenOrders", params, remaining());
                const bool cancel_ok =
                    (resp.status >= 200 && resp.status < 300
                     && provider_recovery::has_exact_top_level_code(
                         resp.body, 200)
                     && provider_recovery::top_level_exact_string(
                         resp.body, "msg",
                         "The operation of cancel all open order is done."));
                if (!cancel_ok)
                {
                    std::cerr << "BinanceFuturesKillSwitch: cancel_all HTTP "
                              << resp.status << " - "
                              << binance::redact_for_log(resp.body, 240)
                              << "\n";
                    all_steps_succeeded = false;
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "BinanceFuturesKillSwitch: cancel_all threw: "
                          << e.what() << "\n";
                all_steps_succeeded = false;
            }
            catch (...)
            {
                std::cerr << "BinanceFuturesKillSwitch: cancel_all threw\n";
                all_steps_succeeded = false;
            }
        }

        // Conditional protection orders live on the separate Algo surface;
        // regular allOpenOrders does not cancel them.
        {
            const std::string params = "symbol=" + binance::url_encode(symbol_);
            try
            {
                auto resp = request_del(
                    "/fapi/v1/algoOpenOrders", params, remaining());
                const bool cancel_ok = resp.status >= 200 && resp.status < 300
                    && provider_recovery::has_exact_top_level_code(
                        resp.body, 200)
                    && provider_recovery::top_level_exact_string(
                        resp.body, "msg",
                        "The operation of cancel all open order is done.");
                if (!cancel_ok)
                {
                    std::cerr << "BinanceFuturesKillSwitch: cancel algo "
                                 "orders failed HTTP " << resp.status << " - "
                              << binance::redact_for_log(resp.body, 240)
                              << "\n";
                    all_steps_succeeded = false;
                }
            }
            catch (...)
            {
                std::cerr << "BinanceFuturesKillSwitch: cancel algo orders "
                             "threw\n";
                all_steps_succeeded = false;
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
            auto pr = request_get(
                "/fapi/v2/positionRisk",
                "symbol=" + binance::url_encode(symbol_), remaining());
            if (pr.status < 200 || pr.status >= 300)
            {
                std::cerr << "BinanceFuturesKillSwitch: /fapi/v2/positionRisk "
                             "HTTP " << pr.status << " - "
                          << binance::redact_for_log(pr.body, 240) << "\n";
                return false;
            }
            if (!BinanceFuturesReconciler::extract_position_amt(
                    pr.body, position_amt, symbol_))
            {
                std::cerr << "BinanceFuturesKillSwitch: positionAmt missing "
                             "in /fapi/v2/positionRisk\n";
                return false;
            }
        }

        if (std::abs(position_amt) < 1e-12)
            return all_steps_succeeded;

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceFuturesKillSwitch: deadline expired before "
                         "flatten\n";
            return false;
        }

        // Long → SELL to close, short → BUY to close.
        const char* close_side = position_amt > 0.0 ? "SELL" : "BUY";
        std::string params;
        binance::append_param(params, "symbol", symbol_);
        binance::append_param(params, "side", close_side);
        binance::append_param(params, "type", "MARKET");
        binance::append_param(params, "reduceOnly", "true");
        binance::append_param(params, "quantity", format_qty(std::abs(position_amt)));
        std::string flatten_client_id;
        if (minter_)
        {
            flatten_client_id = minter_->next();
            binance::append_param(
                params, "newClientOrderId", flatten_client_id);
        }

        auto close = request_post(
            "/fapi/v1/order", params, remaining());
        if (close.status < 200 || close.status >= 300
            || !authoritative_order_ack(close.body, flatten_client_id))
        {
            std::cerr << "BinanceFuturesKillSwitch: flatten order HTTP "
                      << close.status << " - "
                      << binance::redact_for_log(close.body, 240) << "\n";
            return false;
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceFuturesKillSwitch: deadline expired before "
                         "flat-state verification\n";
            return false;
        }
        double verified_position_amt = 0.0;
        auto verify = request_get(
            "/fapi/v2/positionRisk",
            "symbol=" + binance::url_encode(symbol_), remaining());
        if (verify.status < 200 || verify.status >= 300
            || !BinanceFuturesReconciler::extract_position_amt(
                verify.body, verified_position_amt, symbol_)
            || std::abs(verified_position_amt) >= 1e-12)
        {
            std::cerr << "BinanceFuturesKillSwitch: flatten ACK did not "
                         "produce an authoritative flat position\n";
            return false;
        }

        return all_steps_succeeded;
    }

private:
    static bool authoritative_order_ack(std::string_view body,
                                        std::string_view expected_client_id)
    {
        if (!provider_recovery::is_authoritative_object(body)) return false;
        std::uint64_t parsed = 0;
        if (!provider_recovery::top_level_positive_u64(
                body, "orderId", parsed)) return false;
        if (expected_client_id.empty()) return true;
        std::string_view returned_client;
        return provider_recovery::top_level_plain_string(
                   body, "clientOrderId", returned_client)
            && returned_client == expected_client_id;
    }

    BinanceRestClient::response request_del(
        const std::string& endpoint, const std::string& params,
        std::chrono::milliseconds deadline)
    {
        return rest_ ? rest_->safety_del(endpoint, params, deadline)
                     : del_(endpoint, params, deadline);
    }

    BinanceRestClient::response request_get(
        const std::string& endpoint, const std::string& params,
        std::chrono::milliseconds deadline)
    {
        return rest_ ? rest_->safety_get(endpoint, params, deadline)
                     : get_(endpoint, params, deadline);
    }

    BinanceRestClient::response request_post(
        const std::string& endpoint, const std::string& params,
        std::chrono::milliseconds deadline)
    {
        return rest_ ? rest_->safety_post(endpoint, params, deadline)
                     : post_(endpoint, params, deadline);
    }

    static std::string format_qty(double q)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.8f", q);
        return buf;
    }

    std::shared_ptr<BinanceRestClient> rest_;
    request_fn del_;
    request_fn get_;
    request_fn post_;
    std::string symbol_;
    std::shared_ptr<ClientOrderIdMinter> minter_;
};

#endif // HAS_BINANCE
