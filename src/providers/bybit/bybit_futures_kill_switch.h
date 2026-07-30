#pragma once
#ifdef HAS_BYBIT

// Bybit V5 linear kill-switch (Phase 3 safety).
// Deadline-bounded, no retry loop:
//   1) POST /v5/order/cancel-all  {category, symbol}
//   2) GET  /v5/position/list     category=linear&symbol=X
//   3) if |signed_qty| > eps:
//        POST /v5/order/create reduceOnly MARKET opposite side
//
// Routes through post_fn / get_fn so unit tests inject fakes without sockets.
// Production: make_bybit_futures_kill_switch(rest, symbol, minter).

#include "execution/live_safety.h"
#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_futures_reconciler.h"
#include "providers/bybit/bybit_rest_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

class BybitFuturesKillSwitch : public IKillSwitch
{
public:
    struct response
    {
        int status = 0;
        std::string body;
        int ret_code = -1;
        bool business_ok = false;
    };

    using post_fn = std::function<response(std::string_view endpoint,
                                           std::string_view json_body)>;
    using get_fn = std::function<response(std::string_view endpoint,
                                          std::string_view query)>;
    using set_timeout_fn = std::function<void(std::chrono::milliseconds)>;
    using get_timeout_fn = std::function<std::chrono::milliseconds()>;
    using mint_fn = std::function<std::string()>;

    BybitFuturesKillSwitch(post_fn post,
                           get_fn get,
                           std::string symbol,
                           std::string category = "linear",
                           mint_fn mint = nullptr,
                           set_timeout_fn set_timeout = nullptr,
                           get_timeout_fn get_timeout = nullptr)
        : post_(std::move(post))
        , get_(std::move(get))
        , symbol_(std::move(symbol))
        , category_(std::move(category))
        , mint_(std::move(mint))
        , set_timeout_(std::move(set_timeout))
        , get_timeout_(std::move(get_timeout))
    {}

    bool cancel_all_and_flatten(std::chrono::milliseconds deadline) override
    {
        if (!post_ || !get_)
        {
            std::cerr << "BybitFuturesKillSwitch: no post/get fn, cannot act\n";
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto expires_at = start + deadline;

        // Bound each REST call so a still-down LAN can't wedge shutdown.
        // Three calls fit inside deadline; min(1500ms, deadline/3) leaves
        // slack for TLS handshake reuse and wall-clock checks between steps.
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
            const std::string body = make_cancel_all_body();
            auto resp = post_(bybit::paths::order_cancel_all, body);
            if (!ok_cancel(resp))
            {
                std::cerr << "BybitFuturesKillSwitch: cancel-all HTTP "
                          << resp.status << " - "
                          << bybit::truncate_for_log(resp.body) << "\n";
                return false;
            }
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BybitFuturesKillSwitch: deadline expired after "
                         "cancel-all\n";
            return false;
        }

        // 2) Read signed position.
        double position_amt = 0.0;
        {
            const std::string q =
                "category=" + category_ + "&symbol=" + symbol_;
            auto pr = get_(bybit::paths::position_list, q);
            if (!http_ok(pr.status)
                || (!pr.business_ok
                    && !bybit::is_business_success(pr.status, pr.body)))
            {
                std::cerr << "BybitFuturesKillSwitch: position/list HTTP "
                          << pr.status << " - "
                          << bybit::truncate_for_log(pr.body) << "\n";
                return false;
            }
            if (!BybitFuturesReconciler::extract_position_amt(
                    pr.body, position_amt, symbol_))
            {
                std::cerr << "BybitFuturesKillSwitch: position size missing "
                             "in /v5/position/list\n";
                return false;
            }
        }

        if (std::abs(position_amt) < 1e-12)
            return true;

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "BybitFuturesKillSwitch: deadline expired before "
                         "flatten\n";
            return false;
        }

        // 3) Long → Sell to close, short → Buy to close; reduceOnly always.
        {
            const char* close_side = position_amt > 0.0 ? "Sell" : "Buy";
            const std::string body =
                make_flatten_body(close_side, std::abs(position_amt));
            auto close = post_(bybit::paths::order_create, body);
            if (!ok_create(close))
            {
                std::cerr << "BybitFuturesKillSwitch: flatten order HTTP "
                          << close.status << " - "
                          << bybit::truncate_for_log(close.body) << "\n";
                return false;
            }
        }

        return true;
    }

    // retCode strings treated as "nothing to cancel" success.
    static bool is_cancel_noop_code(std::string_view code)
    {
        return code == "110001"  // order not exists / already cancelled (common)
            || code == "110010"  // no open orders to cancel (variant)
            || code == "20001";  // order not modified / not found (legacy-ish)
    }

private:
    std::string make_cancel_all_body() const
    {
        std::string b;
        b.reserve(64 + category_.size() + symbol_.size());
        b.append("{\"category\":\"");
        b.append(category_);
        b.append("\",\"symbol\":\"");
        b.append(symbol_);
        b.append("\"}");
        return b;
    }

    std::string make_flatten_body(const char* side, double abs_qty) const
    {
        char qty_buf[64];
        std::snprintf(qty_buf, sizeof(qty_buf), "%.8f", abs_qty);
        // Trim trailing zeros after decimal for cleaner venue payload.
        std::string qty = qty_buf;
        if (qty.find('.') != std::string::npos)
        {
            while (!qty.empty() && qty.back() == '0') qty.pop_back();
            if (!qty.empty() && qty.back() == '.') qty.pop_back();
        }
        if (qty.empty()) qty = "0";

        std::string b;
        b.reserve(160 + category_.size() + symbol_.size());
        b.append("{\"category\":\"");
        b.append(category_);
        b.append("\",\"symbol\":\"");
        b.append(symbol_);
        b.append("\",\"side\":\"");
        b.append(side);
        b.append("\",\"orderType\":\"Market\",\"qty\":\"");
        b.append(qty);
        b.append("\",\"reduceOnly\":true,\"positionIdx\":0");
        if (mint_)
        {
            const auto id = mint_();
            if (!id.empty())
            {
                b.append(",\"orderLinkId\":\"");
                b.append(id);
                b.push_back('"');
            }
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
        if (r.business_ok || bybit::is_business_success(r.status, r.body))
            return true;
        auto code = bybit::extract_ret_code(r.body);
        return is_cancel_noop_code(code);
    }

    static bool ok_create(const response& r)
    {
        if (!http_ok(r.status)) return false;
        return r.business_ok || bybit::is_business_success(r.status, r.body);
    }

    post_fn post_;
    get_fn get_;
    std::string symbol_;
    std::string category_;
    mint_fn mint_;
    set_timeout_fn set_timeout_;
    get_timeout_fn get_timeout_;
};

inline std::shared_ptr<BybitFuturesKillSwitch>
make_bybit_futures_kill_switch(
    std::shared_ptr<BybitRestClient> rest,
    std::string symbol,
    std::shared_ptr<bybit::ShortOrderLinkIdMinter> minter = nullptr,
    std::string category = "linear")
{
    BybitFuturesKillSwitch::post_fn post;
    BybitFuturesKillSwitch::get_fn get;
    BybitFuturesKillSwitch::set_timeout_fn set_to;
    BybitFuturesKillSwitch::get_timeout_fn get_to;
    BybitFuturesKillSwitch::mint_fn mint;
    if (rest)
    {
        post = [rest](std::string_view ep, std::string_view body)
            -> BybitFuturesKillSwitch::response
        {
            auto r = rest->post_json(std::string(ep), std::string(body));
            return {r.status, std::move(r.body), r.ret_code, r.business_ok};
        };
        get = [rest](std::string_view ep, std::string_view q)
            -> BybitFuturesKillSwitch::response
        {
            auto r = rest->get(std::string(ep), std::string(q));
            return {r.status, std::move(r.body), r.ret_code, r.business_ok};
        };
        set_to = [rest](std::chrono::milliseconds ms) {
            rest->set_per_call_timeout(ms);
        };
        get_to = [rest]() {
            return rest->per_call_timeout();
        };
    }
    if (minter)
    {
        mint = [minter]() { return minter->next(); };
    }
    return std::make_shared<BybitFuturesKillSwitch>(
        std::move(post), std::move(get), std::move(symbol),
        std::move(category), std::move(mint),
        std::move(set_to), std::move(get_to));
}

#endif // HAS_BYBIT
