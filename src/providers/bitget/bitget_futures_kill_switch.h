#pragma once
#ifdef HAS_BITGET

// Bitget UTA v3 kill-switch (Phase 3 safety — gold path).
// Two REST calls, no retry loop, deadline-bounded:
//   1) POST /api/v3/trade/cancel-symbol-order {category, symbol}
//   2) POST /api/v3/trade/close-positions     {category, symbol}
// Venue-native close-positions is the Bitget advantage over Binance's
// positionRisk + reduceOnly MARKET dance.
// Routes through post_fn so unit tests inject a fake without sockets.
// Production: make_bitget_futures_kill_switch(rest, category, symbol).

#include "execution/live_safety.h"
#include "providers/bitget/bitget_rest_client.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

class BitgetFuturesKillSwitch : public IKillSwitch
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using post_fn = std::function<response(std::string_view endpoint,
                                           std::string_view json_body)>;
    // Optional: set REST per-call I/O timeout before each step (rest client).
    using set_timeout_fn = std::function<void(std::chrono::milliseconds)>;
    // Optional: read current timeout so we can restore after kill (shared REST).
    using get_timeout_fn = std::function<std::chrono::milliseconds()>;

    BitgetFuturesKillSwitch(post_fn post,
                            std::string category,
                            std::string symbol,
                            set_timeout_fn set_timeout = nullptr,
                            get_timeout_fn get_timeout = nullptr)
        : post_(std::move(post))
        , category_(std::move(category))
        , symbol_(std::move(symbol))
        , set_timeout_(std::move(set_timeout))
        , get_timeout_(std::move(get_timeout))
    {}

    bool cancel_all_and_flatten(std::chrono::milliseconds deadline) override
    {
        if (!post_)
        {
            std::cerr << "BitgetFuturesKillSwitch: no post_fn, cannot act\n";
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto expires_at = start + deadline;

        // Bound each REST call so a still-down LAN can't wedge shutdown.
        // Two calls fit inside deadline; min(1500ms, deadline/3) leaves
        // slack for TLS handshake reuse and the wall-clock check between.
        // Always restore the previous timeout — kill shares the live REST
        // client with DMS heartbeats and subsequent place/cancel.
        const long long per_call_ms =
            std::min<long long>(1500, deadline.count() / 3);
        const auto prev_timeout = get_timeout_
            ? get_timeout_()
            : std::chrono::milliseconds{0};
        const bool tighten = set_timeout_ && per_call_ms > 0;
        if (tighten)
            set_timeout_(std::chrono::milliseconds(per_call_ms));

        struct restore_timeout
        {
            set_timeout_fn* set = nullptr;
            std::chrono::milliseconds prev{0};
            bool active = false;
            ~restore_timeout()
            {
                if (active && set && *set)
                    (*set)(prev);
            }
        } restorer{&set_timeout_, prev_timeout, tighten};

        // 1) Cancel open orders for this symbol (scoped).
        {
            const std::string body = make_body(/*include_symbol=*/true);
            auto resp = post_("/api/v3/trade/cancel-symbol-order", body);
            if (!ok_cancel(resp))
            {
                std::cerr << "BitgetFuturesKillSwitch: cancel-symbol-order "
                             "HTTP " << resp.status << " - "
                          << bitget::truncate_for_log(resp.body) << "\n";
                return false;
            }
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BitgetFuturesKillSwitch: deadline expired after "
                         "cancel-symbol-order\n";
            return false;
        }

        // 2) Venue-native flatten — empty position is OK.
        {
            const std::string body = make_body(/*include_symbol=*/true);
            auto resp = post_("/api/v3/trade/close-positions", body);
            if (!ok_close(resp))
            {
                std::cerr << "BitgetFuturesKillSwitch: close-positions HTTP "
                          << resp.status << " - "
                          << bitget::truncate_for_log(resp.body) << "\n";
                return false;
            }
        }

        return true;
    }

    // "No orders" / "nothing to cancel" business codes treated as success.
    // Mapped for tests; Bitget often returns top-level 00000 with empty list
    // or per-item 24056, but some paths surface these at the top level.
    static bool is_cancel_noop_code(std::string_view code)
    {
        return code == "25204"   // order does not exist
            || code == "24056"   // notExisted
            || code == "22001";  // no order to cancel (legacy/ws-adjacent)
    }

    // Empty / no position codes treated as success for close-positions.
    static bool is_close_noop_code(std::string_view code)
    {
        return code == "25227"   // no position available to close
            || code == "24056"   // notExisted
            || code == "22002"   // no position to close (legacy)
            || code == "25601";  // position does not exist
    }

private:
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

    static bool ok_cancel(const response& r)
    {
        if (!http_ok(r.status)) return false;
        if (bitget::is_business_success(r.status, r.body)) return true;
        auto code = bitget::extract_business_code(r.body);
        return is_cancel_noop_code(code);
    }

    static bool ok_close(const response& r)
    {
        if (!http_ok(r.status)) return false;
        if (bitget::is_business_success(r.status, r.body)) return true;
        auto code = bitget::extract_business_code(r.body);
        return is_close_noop_code(code);
    }

    post_fn post_;
    std::string category_;
    std::string symbol_;
    set_timeout_fn set_timeout_;
    get_timeout_fn get_timeout_;
};

inline std::shared_ptr<BitgetFuturesKillSwitch>
make_bitget_futures_kill_switch(std::shared_ptr<BitgetRestClient> rest,
                                std::string category,
                                std::string symbol)
{
    BitgetFuturesKillSwitch::post_fn post;
    BitgetFuturesKillSwitch::set_timeout_fn set_to;
    BitgetFuturesKillSwitch::get_timeout_fn get_to;
    if (rest)
    {
        post = [rest](std::string_view ep, std::string_view body)
            -> BitgetFuturesKillSwitch::response
        {
            auto r = rest->post_json(std::string(ep), std::string(body));
            return {r.status, std::move(r.body)};
        };
        set_to = [rest](std::chrono::milliseconds ms) {
            rest->set_per_call_timeout(ms);
        };
        get_to = [rest]() {
            return rest->per_call_timeout();
        };
    }
    return std::make_shared<BitgetFuturesKillSwitch>(
        std::move(post), std::move(category), std::move(symbol),
        std::move(set_to), std::move(get_to));
}

#endif // HAS_BITGET
