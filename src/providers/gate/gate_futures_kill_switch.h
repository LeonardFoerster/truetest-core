#pragma once
#ifdef HAS_GATE

// Gate.io USDT-M futures kill-switch (Phase 3 safety).
// Three REST calls, no retry loop, deadline-bounded:
//   1) DELETE /api/v4/futures/{settle}/orders?contract=...
//   2) GET    /api/v4/futures/{settle}/positions/{contract} → size
//   3) POST   /api/v4/futures/{settle}/orders  reduce-only market
//              (size = -position_size, price="0", tif=ioc)
// Routes through injectable del/get/post so unit tests inject fakes
// without sockets. Production: make_gate_futures_kill_switch(...).
// Loud, non-retrying, fail-closed — deadline miss or HTTP/label fail
// returns false; operator must intervene (S3/S4).

#include "execution/client_order_id.h"
#include "execution/live_safety.h"
#include "providers/gate/gate_auth.h"
#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_parser.h"
#include "providers/gate/gate_rest_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

class GateFuturesKillSwitch : public IKillSwitch
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    // DELETE path + query (no leading '?').
    using del_fn = std::function<response(std::string_view path,
                                          std::string_view query)>;
    // GET path + query (query usually empty for /positions/{contract}).
    using get_fn = std::function<response(std::string_view path,
                                          std::string_view query)>;
    // POST path + exact JSON body (signed as-is by rest client).
    using post_fn = std::function<response(std::string_view path,
                                           std::string_view json_body)>;
    // Optional: set REST per-call I/O timeout before each step.
    using set_timeout_fn = std::function<void(std::chrono::milliseconds)>;
    // Optional: read current timeout so we can restore after kill.
    using get_timeout_fn = std::function<std::chrono::milliseconds()>;

    GateFuturesKillSwitch(del_fn del,
                          get_fn get,
                          post_fn post,
                          gate::endpoints ep,
                          std::string symbol,
                          std::shared_ptr<ClientOrderIdMinter> minter = nullptr,
                          set_timeout_fn set_timeout = nullptr,
                          get_timeout_fn get_timeout = nullptr)
        : del_(std::move(del))
        , get_(std::move(get))
        , post_(std::move(post))
        , ep_(std::move(ep))
        , symbol_(gate::normalize_contract_symbol(symbol))
        , minter_(std::move(minter))
        , set_timeout_(std::move(set_timeout))
        , get_timeout_(std::move(get_timeout))
    {}

    bool cancel_all_and_flatten(std::chrono::milliseconds deadline) override
    {
        if (!del_ || !get_ || !post_)
        {
            std::cerr << "GateFuturesKillSwitch: missing REST fn, cannot act\n";
            return false;
        }
        if (symbol_.empty())
        {
            std::cerr << "GateFuturesKillSwitch: empty symbol, cannot act\n";
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto expires_at = start + deadline;

        // Bound each REST call so a still-down LAN can't wedge shutdown.
        // Three calls fit inside deadline; min(1500ms, deadline/3) leaves
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

        // 1) Cancel open orders for this contract (scoped).
        {
            const std::string path = gate::futures_path(ep_, "/orders");
            const std::string query = "contract=" + symbol_;
            auto resp = del_(path, query);
            if (!ok_cancel(resp))
            {
                std::cerr << "GateFuturesKillSwitch: cancel-all HTTP "
                          << resp.status << " - "
                          << gate::redact_for_log(
                                 gate::truncate_for_log(resp.body))
                          << "\n";
                return false;
            }
        }

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "GateFuturesKillSwitch: deadline expired after "
                         "cancel-all\n";
            return false;
        }

        // 2) Read signed position size.
        double position_size = 0.0;
        {
            std::string tail;
            tail.reserve(11 + symbol_.size());
            tail.append("/positions/");
            tail.append(symbol_);
            const std::string path = gate::futures_path(ep_, tail);
            auto resp = get_(path, /*query=*/"");
            if (!ok_position(resp, position_size))
            {
                std::cerr << "GateFuturesKillSwitch: positions HTTP "
                          << resp.status << " - "
                          << gate::redact_for_log(
                                 gate::truncate_for_log(resp.body))
                          << "\n";
                return false;
            }
        }

        if (std::abs(position_size) < 1e-12)
            return true; // already flat

        if (std::chrono::steady_clock::now() >= expires_at)
        {
            std::cerr << "GateFuturesKillSwitch: deadline expired before "
                         "flatten\n";
            return false;
        }

        // 3) Reduce-only market opposite size (G3: signed size, no side).
        {
            const double close_size = -position_size;
            const std::string path = gate::futures_path(ep_, "/orders");
            const std::string body = make_flatten_body(close_size);
            auto resp = post_(path, body);
            if (!ok_flatten(resp))
            {
                std::cerr << "GateFuturesKillSwitch: flatten order HTTP "
                          << resp.status << " - "
                          << gate::redact_for_log(
                                 gate::truncate_for_log(resp.body))
                          << "\n";
                return false;
            }
        }

        return true;
    }

    // "No orders" / "nothing to cancel" labels treated as success (Binance -2011).
    static bool is_cancel_noop_label(std::string_view label)
    {
        return label == "ORDER_NOT_FOUND"
            || label == "NO_OPEN_ORDERS"
            || label == "NO_CHANGE"
            || label == "EMPTY_ORDERS";
    }

    // Empty / no position labels treated as flat (size=0).
    static bool is_position_flat_label(std::string_view label)
    {
        return label == "POSITION_NOT_FOUND"
            || label == "POSITION_EMPTY"
            || label == "NO_POSITION"
            || label == "EMPTY_POSITION";
    }

    // Extract signed size from GET .../positions/{contract} body.
    // Single object (normal), array of rows, or flat-error label → size 0.
    // Returns false on unparseable non-flat payloads (fail-closed).
    //
    // IMPORTANT: array bodies must be handled BEFORE try_parse_size_field on
    // the whole haystack — extract_sv_number finds the first "size" key and
    // would otherwise return the first row (often 0) and skip non-zero rows.
    static bool extract_position_size(std::string_view body, double& out)
    {
        out = 0.0;
        if (body.empty())
            return true; // treat empty as flat

        // Skip leading whitespace so array detection is robust.
        std::size_t i = 0;
        while (i < body.size()
               && (body[i] == ' ' || body[i] == '\t'
                   || body[i] == '\n' || body[i] == '\r'))
            ++i;
        if (i >= body.size())
            return true;
        const std::string_view trimmed = body.substr(i);

        // Array of position rows — take first non-zero, else first parseable.
        // Gate single-contract path is an object; list path is an array.
        if (trimmed.front() == '[')
        {
            bool any = false;
            double first = 0.0;
            double nonzero = 0.0;
            bool have_nonzero = false;
            for_each_array_object(trimmed, [&](std::string_view obj) {
                double s = 0.0;
                if (!try_parse_size_field(obj, s))
                    return;
                if (!any)
                {
                    first = s;
                    any = true;
                }
                if (!have_nonzero && std::abs(s) >= 1e-12)
                {
                    nonzero = s;
                    have_nonzero = true;
                }
            });
            if (!any)
                return true; // empty array → flat
            out = have_nonzero ? nonzero : first;
            return true;
        }

        // Error envelope: {"label":"...","message":"..."}
        auto label = gate::extract_sv_string(trimmed, "label");
        if (!label.empty()
            && gate::extract_sv_number(trimmed, "size").empty()
            && gate::extract_sv_string(trimmed, "size").empty())
        {
            if (is_position_flat_label(label))
                return true;
            return false;
        }

        // Prefer top-level size (single position object).
        if (try_parse_size_field(trimmed, out))
            return true;

        // Object without size but looks like a position shell → refuse.
        // Bare {} is flat.
        auto contract = gate::extract_sv_string(trimmed, "contract");
        if (contract.empty() && trimmed.find("size") == std::string_view::npos)
            return true;
        return false;
    }

private:
    static bool http_ok(int status)
    {
        return status >= 200 && status < 300;
    }

    static bool message_looks_like_no_orders(std::string_view body)
    {
        // Case-insensitive substring scan for common "nothing to cancel" text.
        auto lower_has = [](std::string_view hay, std::string_view needle) {
            if (needle.empty() || hay.size() < needle.size()) return false;
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
            {
                bool match = true;
                for (std::size_t j = 0; j < needle.size(); ++j)
                {
                    const auto a = static_cast<unsigned char>(hay[i + j]);
                    const auto b = static_cast<unsigned char>(needle[j]);
                    if (std::tolower(a) != std::tolower(b))
                    {
                        match = false;
                        break;
                    }
                }
                if (match) return true;
            }
            return false;
        };
        return lower_has(body, "no open order")
            || lower_has(body, "order not found")
            || lower_has(body, "no order to cancel");
    }

    static bool ok_cancel(const response& r)
    {
        if (http_ok(r.status))
            return true;
        auto label = gate::extract_error_label(r.body);
        if (is_cancel_noop_label(label))
            return true;
        if (message_looks_like_no_orders(r.body))
            return true;
        return false;
    }

    static bool ok_position(const response& r, double& size_out)
    {
        if (http_ok(r.status))
            return extract_position_size(r.body, size_out);
        // Non-2xx: only flat labels are OK.
        auto label = gate::extract_error_label(r.body);
        if (is_position_flat_label(label))
        {
            size_out = 0.0;
            return true;
        }
        return false;
    }

    static bool ok_flatten(const response& r)
    {
        if (!http_ok(r.status))
            return false;
        // Gate place success returns the order object (has id / contract).
        // Fail-closed if body is an error envelope on 2xx (defensive).
        auto label = gate::extract_sv_string(r.body, "label");
        if (!label.empty()
            && gate::extract_sv_string(r.body, "contract").empty()
            && gate::extract_sv_number(r.body, "id").empty()
            && gate::extract_sv_string(r.body, "id").empty())
            return false;
        return true;
    }

    static bool try_parse_size_field(std::string_view obj, double& out)
    {
        auto sv = gate::extract_sv_number(obj, "size");
        if (sv.empty())
            sv = gate::extract_sv_string(obj, "size");
        if (sv.empty())
            return false;
        double v = 0.0;
        if (!gate::parse_double_sv(sv, v))
            return false;
        out = v;
        return true;
    }

    // Minimal array-of-objects walker (cold path). Invokes cb for each `{...}`.
    template <typename Fn>
    static void for_each_array_object(std::string_view arr, Fn&& cb)
    {
        std::size_t i = 0;
        while (i < arr.size() && arr[i] != '[')
            ++i;
        if (i >= arr.size())
            return;
        ++i;
        while (i < arr.size())
        {
            while (i < arr.size()
                   && (arr[i] == ',' || arr[i] == ' ' || arr[i] == '\n'
                       || arr[i] == '\r' || arr[i] == '\t'))
                ++i;
            if (i >= arr.size() || arr[i] == ']')
                return;
            if (arr[i] != '{')
            {
                ++i;
                continue;
            }
            const std::size_t start = i;
            int depth = 0;
            for (; i < arr.size(); ++i)
            {
                if (arr[i] == '{')
                    ++depth;
                else if (arr[i] == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        ++i;
                        cb(arr.substr(start, i - start));
                        break;
                    }
                }
            }
        }
    }

    static std::string format_size(double size)
    {
        char buf[64];
        // Prefer integer JSON number when size is whole contracts.
        const double nearest = std::round(size);
        if (std::abs(size - nearest) < 1e-9 && std::abs(nearest) < 1e15)
        {
            std::snprintf(buf, sizeof(buf), "%.0f", nearest);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%.8f", size);
        }
        return std::string(buf);
    }

    // Gate text: t- prefix, [0-9A-Za-z_.-], conservative ≤32 total.
    static std::string make_client_text(
        const std::shared_ptr<ClientOrderIdMinter>& minter)
    {
        std::string raw = minter ? minter->next() : "tt-kill";
        std::string id;
        id.reserve(raw.size() + 2);
        if (raw.size() < 2 || raw[0] != 't' || raw[1] != '-')
            id.append("t-");
        for (char c : raw)
        {
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
                || (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == '-')
                id.push_back(c);
        }
        if (id == "t-" || id.empty())
            id = "t-tt-kill";
        if (id.size() > 32)
            id.resize(32);
        return id;
    }

    std::string make_flatten_body(double close_size) const
    {
        const std::string text = make_client_text(minter_);
        const std::string size_s = format_size(close_size);
        std::string b;
        b.reserve(96 + symbol_.size() + size_s.size() + text.size());
        b.append("{\"contract\":\"");
        b.append(symbol_);
        b.append("\",\"size\":");
        b.append(size_s);
        b.append(",\"price\":\"0\",\"tif\":\"ioc\",\"reduce_only\":true");
        b.append(",\"text\":\"");
        b.append(text);
        b.append("\"}");
        return b;
    }

    del_fn del_;
    get_fn get_;
    post_fn post_;
    gate::endpoints ep_;
    std::string symbol_;
    std::shared_ptr<ClientOrderIdMinter> minter_;
    set_timeout_fn set_timeout_;
    get_timeout_fn get_timeout_;
};

inline std::shared_ptr<GateFuturesKillSwitch>
make_gate_futures_kill_switch(std::shared_ptr<GateRestClient> rest,
                              gate::endpoints ep,
                              std::string symbol,
                              std::shared_ptr<ClientOrderIdMinter> minter
                              = nullptr)
{
    GateFuturesKillSwitch::del_fn del;
    GateFuturesKillSwitch::get_fn get;
    GateFuturesKillSwitch::post_fn post;
    GateFuturesKillSwitch::set_timeout_fn set_to;
    GateFuturesKillSwitch::get_timeout_fn get_to;
    if (rest)
    {
        del = [rest](std::string_view path, std::string_view query)
            -> GateFuturesKillSwitch::response
        {
            auto r = rest->del(std::string(path), std::string(query));
            return {r.status, std::move(r.body)};
        };
        get = [rest](std::string_view path, std::string_view query)
            -> GateFuturesKillSwitch::response
        {
            auto r = rest->get(std::string(path), std::string(query));
            return {r.status, std::move(r.body)};
        };
        post = [rest](std::string_view path, std::string_view body)
            -> GateFuturesKillSwitch::response
        {
            auto r = rest->post_json(std::string(path), std::string(body));
            return {r.status, std::move(r.body)};
        };
        set_to = [rest](std::chrono::milliseconds ms) {
            rest->set_per_call_timeout(ms);
        };
        get_to = [rest]() {
            return rest->per_call_timeout();
        };
    }
    return std::make_shared<GateFuturesKillSwitch>(
        std::move(del), std::move(get), std::move(post), std::move(ep),
        std::move(symbol), std::move(minter), std::move(set_to),
        std::move(get_to));
}

#endif // HAS_GATE
