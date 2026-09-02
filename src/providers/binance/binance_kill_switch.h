#pragma once
#ifdef HAS_BINANCE

#include "execution/client_order_id.h"
#include "execution/live_safety.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_reconciler.h"
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

// Cancel all open orders, then market-sell base-asset balance queried
// from the exchange (not local state - local may be exactly what's wrong).
// A placement ACK is not success: the final account readback must prove flat.
class BinanceKillSwitch : public IKillSwitch
{
public:
    using request_fn = std::function<BinanceRestClient::response(
        const std::string&, const std::string&, std::chrono::milliseconds)>;
    struct injected_requests_t {};
    static constexpr injected_requests_t injected_requests{};

    BinanceKillSwitch(std::shared_ptr<BinanceRestClient> rest,
                      std::string symbol,
                      std::string base_asset,
                      std::shared_ptr<ClientOrderIdMinter> minter)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , base_asset_(std::move(base_asset))
        , minter_(std::move(minter))
    {}

    BinanceKillSwitch(injected_requests_t,
                      request_fn del,
                      request_fn get,
                      request_fn post,
                      std::string symbol,
                      std::string base_asset,
                      std::shared_ptr<ClientOrderIdMinter> minter = {})
        : del_(std::move(del))
        , get_(std::move(get))
        , post_(std::move(post))
        , symbol_(std::move(symbol))
        , base_asset_(std::move(base_asset))
        , minter_(std::move(minter))
    {}

    bool is_operational() const noexcept override
    {
        return (rest_ || (del_ && get_ && post_))
            && !symbol_.empty() && !base_asset_.empty();
    }

    bool cancel_all_and_flatten(std::chrono::milliseconds deadline) override
    {
        if (!rest_ && (!del_ || !get_ || !post_))
        {
            std::cerr << "BinanceKillSwitch: no REST client, cannot act\n";
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
                    "/api/v3/openOrders", params, remaining());
                const bool cancel_ok =
                    (resp.status >= 200 && resp.status < 300
                     && authoritative_cancel_all(resp.body))
                    || ((resp.status < 200 || resp.status >= 300)
                        && provider_recovery::has_exact_top_level_code(
                            resp.body, -2011));
                if (!cancel_ok)
                {
                    std::cerr << "BinanceKillSwitch: cancel_all HTTP "
                              << resp.status << " - "
                              << binance::redact_for_log(resp.body, 240)
                              << "\n";
                    all_steps_succeeded = false;
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "BinanceKillSwitch: cancel_all threw: "
                          << e.what() << "\n";
                all_steps_succeeded = false;
            }
            catch (...)
            {
                std::cerr << "BinanceKillSwitch: cancel_all threw\n";
                all_steps_succeeded = false;
            }
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceKillSwitch: deadline expired after cancel_all\n";
            return false;
        }

        double ex_base_free = 0.0, ex_base_locked = 0.0;
        {
            auto acct = request_get(
                "/api/v3/account", "", remaining());
            if (acct.status < 200 || acct.status >= 300)
            {
                std::cerr << "BinanceKillSwitch: /api/v3/account HTTP "
                          << acct.status << " - "
                          << binance::redact_for_log(acct.body, 240) << "\n";
                return false;
            }
            if (!BinanceReconciler::extract_balance(
                    acct.body, base_asset_, ex_base_free, ex_base_locked))
            {
                std::cerr << "BinanceKillSwitch: /api/v3/account response "
                             "did not contain base asset " << base_asset_
                          << "; flatten outcome is unknown\n";
                return false;
            }
        }

        if (ex_base_locked > 1e-12)
        {
            std::cerr << "BinanceKillSwitch: " << ex_base_locked << " "
                      << base_asset_ << " remains locked after cancel_all; "
                         "continuing to flatten free inventory but preserving "
                         "failure state\n";
            all_steps_succeeded = false;
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
            return all_steps_succeeded;
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceKillSwitch: deadline expired before flatten\n";
            return false;
        }

        // clientOrderId -> idempotent against transport retries.
        std::string params;
        binance::append_param(params, "symbol", symbol_);
        binance::append_param(params, "side", "SELL");
        binance::append_param(params, "type", "MARKET");
        binance::append_param(params, "quantity", format_qty(ex_base_free));
        std::string flatten_client_id;
        if (minter_)
        {
            flatten_client_id = minter_->next();
            binance::append_param(
                params, "newClientOrderId", flatten_client_id);
        }

        auto sell = request_post(
            "/api/v3/order", params, remaining());
        if (sell.status < 200 || sell.status >= 300
            || !authoritative_order_ack(sell.body, flatten_client_id))
        {
            std::cerr << "BinanceKillSwitch: flatten order HTTP "
                      << sell.status << " - "
                      << binance::redact_for_log(sell.body, 240) << "\n";
            return false;
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BinanceKillSwitch: deadline expired before "
                         "flat-state verification\n";
            return false;
        }
        double verified_free = 0.0;
        double verified_locked = 0.0;
        auto verify = request_get("/api/v3/account", "", remaining());
        if (verify.status < 200 || verify.status >= 300
            || !BinanceReconciler::extract_balance(
                verify.body, base_asset_, verified_free, verified_locked)
            || verified_free >= 1e-12 || verified_locked >= 1e-12)
        {
            std::cerr << "BinanceKillSwitch: flatten ACK did not produce "
                         "an authoritative zero base-asset balance\n";
            return false;
        }

        return all_steps_succeeded;
    }

private:
    bool authoritative_cancel_all(std::string_view body) const
    {
        if (!provider_recovery::is_authoritative_object_array(body))
            return false;
        return provider_recovery::every_top_level_object(
            body, [&](std::string_view row) {
                std::uint64_t parsed = 0;
                std::string_view returned_symbol;
                if (!provider_recovery::top_level_plain_string(
                        row, "symbol", returned_symbol)
                    || returned_symbol != symbol_)
                    return false;

                std::string_view order_id_raw;
                const auto order_id_state = provider_recovery::payload_parser(
                    row).inspect_top_level_member("orderId", order_id_raw);
                if (order_id_state
                    == provider_recovery::payload_parser::member_result::unique)
                {
                    return provider_recovery::top_level_positive_u64(
                               row, "orderId", parsed)
                        && provider_recovery::top_level_exact_string(
                               row, "status", "CANCELED");
                }
                if (order_id_state
                    == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
                    return false;

                if (!provider_recovery::top_level_positive_u64(
                        row, "orderListId", parsed)
                    || !provider_recovery::top_level_exact_string(
                        row, "listStatusType", "ALL_DONE")
                    || !provider_recovery::top_level_exact_string(
                        row, "listOrderStatus", "ALL_DONE"))
                    return false;

                std::string_view reports;
                if (!provider_recovery::top_level_member(
                        row, "orderReports", reports)
                    || !provider_recovery::is_authoritative_object_array(
                        reports))
                    return false;
                std::size_t report_count = 0;
                const bool reports_ok =
                    provider_recovery::every_top_level_object(
                        reports, [&](std::string_view report) {
                            ++report_count;
                            std::uint64_t report_id = 0;
                            std::string_view report_symbol;
                            return provider_recovery::top_level_positive_u64(
                                       report, "orderId", report_id)
                                && provider_recovery::top_level_exact_string(
                                       report, "status", "CANCELED")
                                && provider_recovery::top_level_plain_string(
                                       report, "symbol", report_symbol)
                                && report_symbol == symbol_;
                        });
                return reports_ok && report_count > 0;
            });
    }

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
    std::string base_asset_;
    std::shared_ptr<ClientOrderIdMinter> minter_;
};

#endif // HAS_BINANCE
