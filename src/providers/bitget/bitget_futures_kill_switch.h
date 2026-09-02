#pragma once
#ifdef HAS_BITGET

// Bitget UTA v3 kill-switch (Phase 3 safety — gold path).
// One bounded, non-retrying shutdown sequence:
//   1) POST /api/v3/trade/cancel-symbol-order {category, symbol}
//   2) GET unfilled-orders to prove regular orders are gone
//   3) list/cancel/read back both tpsl and trigger strategy orders
//   4) POST /api/v3/trade/close-positions     {category, symbol}
//   5) GET current-position to prove flat after a close mutation
// Venue-native close-positions is the Bitget advantage over Binance's
// positionRisk + reduceOnly MARKET dance.
// Routes through post_fn so unit tests inject a fake without sockets.
// Production: make_bitget_futures_kill_switch(rest, category, symbol).

#include "execution/live_safety.h"
#include "providers/bitget/bitget_futures_reconciler.h"
#include "providers/bitget/bitget_rest_client.h"
#include "providers/recovery_payload.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

class BitgetFuturesKillSwitch : public IKillSwitch
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using post_fn = std::function<response(
        std::string_view endpoint,
        std::string_view json_body,
        std::chrono::milliseconds deadline)>;
    using get_fn = std::function<response(
        std::string_view endpoint,
        std::string_view query,
        std::chrono::milliseconds deadline)>;

    BitgetFuturesKillSwitch(post_fn post,
                            std::string category,
                            std::string symbol,
                            get_fn get = {})
        : post_(std::move(post))
        , category_(std::move(category))
        , symbol_(std::move(symbol))
        , get_(std::move(get))
    {}

    bool is_operational() const noexcept override
    {
        return post_ && get_ && !category_.empty() && !symbol_.empty();
    }

    bool cancel_all_and_flatten(std::chrono::milliseconds deadline) override
    {
        if (!post_)
        {
            std::cerr << "BitgetFuturesKillSwitch: no post_fn, cannot act\n";
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

        // 1) Cancel open orders for this symbol (scoped).
        {
            const std::string body = make_body(/*include_symbol=*/true);
            try
            {
                auto resp = post_(
                    "/api/v3/trade/cancel-symbol-order", body, remaining());
                if (!ok_cancel(resp))
                {
                    std::cerr << "BitgetFuturesKillSwitch: cancel-symbol-order "
                                 "HTTP " << resp.status << " - "
                              << bitget::truncate_for_log(resp.body) << "\n";
                    all_steps_succeeded = false;
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "BitgetFuturesKillSwitch: cancel-symbol-order threw: "
                          << e.what() << "\n";
                all_steps_succeeded = false;
            }
            catch (...)
            {
                std::cerr << "BitgetFuturesKillSwitch: cancel-symbol-order threw\n";
                all_steps_succeeded = false;
            }
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BitgetFuturesKillSwitch: deadline expired after "
                         "cancel-symbol-order\n";
            return false;
        }

        // A cancel ACK proves request receipt, not final venue state. The
        // authoritative synchronous proof is the scoped open-order surface.
        try
        {
            if (!regular_orders_empty(expires_at))
                all_steps_succeeded = false;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BitgetFuturesKillSwitch: regular-order readback threw: "
                      << e.what() << "\n";
            all_steps_succeeded = false;
        }
        catch (...)
        {
            std::cerr << "BitgetFuturesKillSwitch: regular-order readback threw\n";
            all_steps_succeeded = false;
        }

        // Strategy/TPSL orders live outside cancel-symbol-order. Sweep both
        // supported strategy surfaces, then prove the scoped readbacks empty.
        for (const std::string_view type : {std::string_view{"tpsl"},
                                            std::string_view{"trigger"}})
        {
            try
            {
                if (!sweep_strategy_orders(type, expires_at))
                    all_steps_succeeded = false;
            }
            catch (const std::exception& e)
            {
                std::cerr << "BitgetFuturesKillSwitch: " << type
                          << " strategy sweep threw: " << e.what() << "\n";
                all_steps_succeeded = false;
            }
            catch (...)
            {
                std::cerr << "BitgetFuturesKillSwitch: " << type
                          << " strategy sweep threw\n";
                all_steps_succeeded = false;
            }
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BitgetFuturesKillSwitch: deadline expired during "
                         "strategy-order sweep\n";
            return false;
        }

        // 2) Venue-native flatten — empty position is OK.
        {
            const std::string body = make_body(/*include_symbol=*/true);
            try
            {
                auto resp = post_(
                    "/api/v3/trade/close-positions", body, remaining());
                const auto close_result = classify_close(resp);
                if (close_result == close_outcome::invalid)
                {
                    std::cerr << "BitgetFuturesKillSwitch: close-positions HTTP "
                              << resp.status << " - "
                              << bitget::truncate_for_log(resp.body) << "\n";
                    return false;
                }
                if (close_result == close_outcome::mutation_accepted)
                {
                    if (!get_ || std::chrono::steady_clock::now() >= expires_at)
                    {
                        std::cerr << "BitgetFuturesKillSwitch: close ACK cannot "
                                     "be verified flat within the deadline\n";
                        return false;
                    }
                    const std::string query = "category=" + category_
                        + "&symbol=" + symbol_;
                    const auto verify = get_(
                        "/api/v3/position/current-position", query, remaining());
                    double verified_position = 0.0;
                    if (!http_ok(verify.status)
                        || !bitget::is_business_success(
                            verify.status, verify.body)
                        || !BitgetFuturesReconciler::extract_position_amt(
                            verify.body, verified_position, symbol_)
                        || std::abs(verified_position) >= 1e-12)
                    {
                        std::cerr << "BitgetFuturesKillSwitch: close ACK did not "
                                     "produce an authoritative flat position\n";
                        return false;
                    }
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "BitgetFuturesKillSwitch: close/flat verification "
                             "threw: " << e.what() << "\n";
                return false;
            }
            catch (...)
            {
                std::cerr << "BitgetFuturesKillSwitch: close/flat verification "
                             "threw\n";
                return false;
            }
        }

        return all_steps_succeeded;
    }

    // "No orders" / "nothing to cancel" business codes treated as success.
    // Mapped for tests; Bitget often returns top-level 00000 with empty list
    // or per-item 24056, but some paths surface these at the top level.
    static bool is_cancel_noop_code(std::string_view code)
    {
        return code == "25204";  // REST: order does not exist
    }

    // Empty / no position codes treated as success for close-positions.
    static bool is_close_noop_code(std::string_view code)
    {
        return code == "25227";  // REST: no position available to close
    }

private:
    std::chrono::milliseconds remaining_until(
        std::chrono::steady_clock::time_point expires_at) const
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= expires_at) return std::chrono::milliseconds{0};
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            expires_at - now);
    }

    bool regular_orders_empty(
        std::chrono::steady_clock::time_point expires_at) const
    {
        if (!get_ || std::chrono::steady_clock::now() >= expires_at)
            return false;
        const std::string query = "category=" + category_
            + "&symbol=" + symbol_;
        const auto resp = get_(
            "/api/v3/trade/unfilled-orders", query,
            remaining_until(expires_at));
        if (!http_ok(resp.status)
            || !provider_recovery::is_authoritative_object(resp.body)
            || !provider_recovery::top_level_exact_string(
                resp.body, "code", "00000")
            || !provider_recovery::top_level_exact_string(
                resp.body, "msg", "success"))
            return false;

        std::string_view data;
        std::string_view list;
        if (!provider_recovery::top_level_member(resp.body, "data", data)
            || !provider_recovery::is_authoritative_object(data)
            || !provider_recovery::top_level_member(data, "list", list)
            || !provider_recovery::is_authoritative_object_array(list))
            return false;

        std::size_t rows = 0;
        const bool valid = provider_recovery::every_top_level_object(
            list, [&](std::string_view) {
                ++rows;
                return true;
            });
        return valid && rows == 0;
    }

    bool list_strategy_orders(std::string_view type,
                              std::chrono::steady_clock::time_point expires_at,
                              std::vector<std::string>& ids)
    {
        ids.clear();
        if (!get_) return false;
        const std::string query = "category=" + category_
            + "&type=" + std::string(type);
        const auto resp = get_(
            "/api/v3/trade/unfilled-strategy-orders", query,
            remaining_until(expires_at));
        if (resp.status < 200 || resp.status >= 300
            || !provider_recovery::is_authoritative_object(resp.body)
            || !provider_recovery::top_level_exact_string(
                resp.body, "code", "00000")
            || !provider_recovery::top_level_exact_string(
                resp.body, "msg", "success"))
            return false;
        std::string_view data;
        if (!provider_recovery::top_level_member(resp.body, "data", data)
            || !provider_recovery::is_authoritative_object_array(data))
            return false;
        std::unordered_set<std::string> unique;
        return provider_recovery::every_top_level_object(
            data, [&](std::string_view row) {
                std::string_view category;
                std::string_view symbol;
                std::string_view status;
                std::string_view client_oid;
                std::uint64_t order_id = 0;
                if (!provider_recovery::top_level_plain_string(
                        row, "category", category)
                    || category != category_
                    || !provider_recovery::top_level_plain_string(
                        row, "symbol", symbol)
                    || !provider_recovery::top_level_plain_string(
                        row, "status", status)
                    || !provider_recovery::top_level_plain_string(
                        row, "clientOid", client_oid)
                    || !provider_recovery::top_level_positive_u64(
                        row, "orderId", order_id)
                    || (status != "pending" && status != "submitting"))
                    return false;
                if (symbol != symbol_) return true;
                const std::string id = std::to_string(order_id);
                if (!unique.insert(id).second) return false;
                ids.push_back(id);
                return true;
            });
    }

    bool sweep_strategy_orders(
        std::string_view type,
        std::chrono::steady_clock::time_point expires_at)
    {
        std::vector<std::string> ids;
        if (!list_strategy_orders(type, expires_at, ids)) return false;
        bool all_cancelled = true;
        for (const auto& id : ids)
        {
            if (std::chrono::steady_clock::now() >= expires_at)
                return false;
            const std::string body = "{\"orderId\":\"" + id + "\"}";
            try
            {
                const auto resp = post_(
                    "/api/v3/trade/cancel-strategy-order", body,
                    remaining_until(expires_at));
                std::string_view data;
                const bool cancelled = resp.status >= 200 && resp.status < 300
                    && provider_recovery::top_level_exact_string(
                        resp.body, "code", "00000")
                    && provider_recovery::top_level_exact_string(
                        resp.body, "msg", "success")
                    && provider_recovery::top_level_member(
                        resp.body, "data", data)
                    && provider_recovery::is_exact_null(data);
                const bool already_gone = resp.status >= 200
                    && resp.status < 300
                    && bitget::extract_business_code(resp.body) == "25204";
                if (!cancelled && !already_gone) all_cancelled = false;
            }
            catch (...) { all_cancelled = false; }
        }
        std::vector<std::string> remaining;
        return list_strategy_orders(type, expires_at, remaining)
            && remaining.empty() && all_cancelled;
    }

    std::string make_body(bool include_symbol) const
    {
        // Minimal exact JSON — rest client signs the body as-is.
        std::string b;
        b.reserve(64 + category_.size() + symbol_.size());
        b.append("{\"category\":\"");
        b.append(category_);
        b.push_back('"');
        if (include_symbol && !symbol_.empty())
        {
            b.append(",\"symbol\":\"");
            b.append(symbol_);
            b.push_back('"');
        }
        b.push_back('}');
        return b;
    }

    static bool http_ok(int status)
    {
        return status >= 200 && status < 300;
    }

    enum class close_outcome { invalid, already_flat, mutation_accepted };

    bool authoritative_batch_success(const response& r,
                                     std::size_t* row_count_out = nullptr) const
    {
        if (!http_ok(r.status)) return false;
        if (!provider_recovery::is_authoritative_object(r.body))
            return false;
        if (!provider_recovery::top_level_exact_string(
                r.body, "code", "00000")
            || !provider_recovery::top_level_exact_string(
                r.body, "msg", "success"))
            return false;

        std::string_view data;
        if (!provider_recovery::top_level_member(r.body, "data", data)
            || !provider_recovery::is_authoritative_object(data))
            return false;
        std::string_view results;
        if (!provider_recovery::top_level_member(data, "list", results)
            || !provider_recovery::is_authoritative_object_array(results))
            return false;

        std::size_t row_count = 0;
        const bool valid = provider_recovery::every_top_level_object(
            results, [&](std::string_view row) {
                ++row_count;
                std::string_view order_id;
                std::string_view client_oid;
                std::string_view code;
                std::string_view msg;
                if (!provider_recovery::top_level_plain_string(
                        row, "orderId", order_id)
                    || !provider_recovery::top_level_plain_string(
                        row, "clientOid", client_oid)
                    || !provider_recovery::top_level_plain_string(
                        row, "code", code)
                    || !provider_recovery::top_level_plain_string(
                        row, "msg", msg)
                    || (order_id.empty() && client_oid.empty()))
                    return false;

                if (!order_id.empty())
                {
                    std::uint64_t parsed = 0;
                    if (!provider_recovery::parse_positive_u64(
                            order_id, parsed))
                        return false;
                }

                if (code == "00000") return msg == "success";
                if (msg.empty()) return false;
                if (code == "24056") return msg == "notExisted";
                return false;
            });
        if (row_count_out) *row_count_out = row_count;
        return valid;
    }

    bool ok_cancel(const response& r) const
    {
        if (!http_ok(r.status)) return false;
        const auto code = bitget::extract_business_code(r.body);
        return is_cancel_noop_code(code)
            || authoritative_batch_success(r);
    }

    close_outcome classify_close(const response& r) const
    {
        if (!http_ok(r.status)) return close_outcome::invalid;
        const auto code = bitget::extract_business_code(r.body);
        if (is_close_noop_code(code)) return close_outcome::already_flat;
        std::size_t row_count = 0;
        if (!authoritative_batch_success(r, &row_count))
            return close_outcome::invalid;
        (void)row_count;
        // `data.list` is a mutation result list, not an authoritative
        // position snapshot. Even an empty successful list must be followed
        // by current-position before the DMS may be disarmed.
        return close_outcome::mutation_accepted;
    }

    post_fn post_;
    std::string category_;
    std::string symbol_;
    get_fn get_;
};

inline std::shared_ptr<BitgetFuturesKillSwitch>
make_bitget_futures_kill_switch(std::shared_ptr<BitgetRestClient> rest,
                                std::string category,
                                std::string symbol)
{
    BitgetFuturesKillSwitch::post_fn post;
    BitgetFuturesKillSwitch::get_fn get;
    if (rest)
    {
        post = [rest](std::string_view ep, std::string_view body,
                      std::chrono::milliseconds deadline)
            -> BitgetFuturesKillSwitch::response
        {
            auto r = rest->safety_post_json(
                std::string(ep), std::string(body), deadline);
            return {r.status, std::move(r.body)};
        };
        get = [rest](std::string_view ep, std::string_view query,
                     std::chrono::milliseconds deadline)
            -> BitgetFuturesKillSwitch::response
        {
            auto r = rest->safety_get(
                std::string(ep), std::string(query), deadline);
            return {r.status, std::move(r.body)};
        };
    }
    return std::make_shared<BitgetFuturesKillSwitch>(
        std::move(post), std::move(category), std::move(symbol),
        std::move(get));
}

#endif // HAS_BITGET
