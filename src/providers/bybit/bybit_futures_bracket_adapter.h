#pragma once
#ifdef HAS_BYBIT

// Bybit V5 linear futures bracket adapter (Phase 4).
// Places SL+TP as two SEPARATE conditional Market orders via
// POST /v5/order/create (triggerPrice + reduceOnly + closeOnTrigger).
// Cancel via POST /v5/order/cancel. Restart recovery via
// GET /v5/order/realtime filtered by orderLinkId prefix.
//
// Placement is NOT atomic (oco=false) — same contract as Binance futures
// brackets. Partial qty_fraction scale-outs decline → engine ExitManager
// remains the enforcer. Full-position brackets only (qty_fraction ≈ 1).
//
// Note: reduceOnly place orders cannot also attach TP/SL on the same create
// (Bybit quirk); brackets are always separate conditional orders.

#include "exits/bracket_adapter.h"
#include "exits/exit_intent.h"
#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_parser.h"
#include "providers/bybit/bybit_rest_client.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class BybitFuturesBracketAdapter : public truetest::exits::IBracketAdapter
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using post_fn = std::function<response(std::string_view endpoint,
                                           std::string_view json_body)>;
    using get_fn = std::function<response(std::string_view endpoint,
                                          std::string_view query)>;

    BybitFuturesBracketAdapter(post_fn post,
                               get_fn get,
                               std::string category = "linear")
        : post_(std::move(post))
        , get_(std::move(get))
        , category_(std::move(category))
    {}

    truetest::exits::bracket_caps capabilities() const override
    {
        truetest::exits::bracket_caps c;
        c.stop_market = true;
        // Two independent POSTs — not atomic. Engine must not assume OCO
        // placement semantics; cancel-other-when-fires is best-effort via
        // reduceOnly/closeOnTrigger once position is flat.
        c.oco           = false;
        c.stop_limit    = false;
        c.trailing_stop = false;
        return c;
    }

    truetest::exits::bracket_handles place(
        std::uint64_t opener_order_id,
        const truetest::exits::exit_intent& intent,
        double /*opener_fill_price*/) override
    {
        truetest::exits::bracket_handles handles;

        if (!intent.stop_loss || !intent.take_profit)
        {
            std::cerr << "BybitFuturesBracketAdapter: intent missing SL or TP "
                         "(opener=" << opener_order_id << ") — declining; "
                         "engine-side eval remains the only enforcer\n";
            return handles;
        }
        if (intent.qty_fraction < 0.999999 || intent.qty_fraction > 1.000001)
        {
            std::cerr << "BybitFuturesBracketAdapter: partial-fraction intent "
                         "(qty_fraction=" << intent.qty_fraction
                      << ") not supported; declining "
                         "(opener=" << opener_order_id << ")\n";
            return handles;
        }
        if (intent.qty <= 0.0)
        {
            std::cerr << "BybitFuturesBracketAdapter: intent.qty <= 0 "
                         "(opener=" << opener_order_id << ") — declining\n";
            return handles;
        }
        if (!post_ || category_.empty())
            return handles;

        const std::string symbol = upper(intent.symbol);
        const char* side =
            (intent.close_side == order_side::sell) ? "Sell" : "Buy";

        // triggerDirection: 1 = rise to trigger, 2 = fall to trigger.
        // Long close (Sell): SL falls (2), TP rises (1).
        // Short close (Buy):  SL rises (1), TP falls (2).
        const int sl_dir =
            (intent.close_side == order_side::sell) ? 2 : 1;
        const int tp_dir =
            (intent.close_side == order_side::sell) ? 1 : 2;

        // orderLinkId max 36: "tt-fb-sl-" (9) + decimal opener id.
        const std::string sl_cli = "tt-fb-sl-" + std::to_string(opener_order_id);
        const std::string tp_cli = "tt-fb-tp-" + std::to_string(opener_order_id);
        if (sl_cli.size() > 36 || tp_cli.size() > 36)
        {
            std::cerr << "BybitFuturesBracketAdapter: orderLinkId exceeds 36 "
                         "for opener=" << opener_order_id << " — declining\n";
            return handles;
        }

        // Place SL first so a protective stop exists if TP fails.
        auto sl_id = place_leg(symbol, side, *intent.stop_loss, sl_dir,
                               intent.qty, sl_cli, opener_order_id, "SL");
        if (sl_id.empty())
            return handles;
        handles.sl_exchange_id = sl_id;
        handles.symbol = symbol;

        auto tp_id = place_leg(symbol, side, *intent.take_profit, tp_dir,
                               intent.qty, tp_cli, opener_order_id, "TP");
        if (tp_id.empty())
        {
            std::cerr << "BybitFuturesBracketAdapter: TP leg failed for opener="
                      << opener_order_id << " — SL is placed (orderId="
                      << sl_id << "), engine-side TP remains the only "
                         "enforcer for that side\n";
            return handles;
        }
        handles.tp_exchange_id = tp_id;
        return handles;
    }

    void cancel(std::uint64_t opener_order_id,
                const truetest::exits::bracket_handles& handles) override
    {
        if (!post_ || handles.empty())
            return;

        struct leg { const std::optional<std::string>* id; const char* tag; };
        const leg legs[] = {
            {&handles.sl_exchange_id, "SL"},
            {&handles.tp_exchange_id, "TP"},
        };
        for (const auto& l : legs)
        {
            if (!*l.id) continue;
            std::string body;
            body.reserve(80 + handles.symbol.size() + (**l.id).size());
            body.append("{\"category\":\"");
            body.append(category_);
            body.append("\"");
            if (!handles.symbol.empty())
            {
                body.append(",\"symbol\":\"");
                body.append(handles.symbol);
                body.append("\"");
            }
            body.append(",\"orderId\":\"");
            body.append(**l.id);
            body.append("\"}");

            auto resp = post_(bybit::paths::order_cancel, body);
            if (!ok_cancel(resp))
            {
                std::cerr << "BybitFuturesBracketAdapter: cancel "
                          << l.tag << " (orderId=" << **l.id
                          << ") for opener=" << opener_order_id
                          << " HTTP " << resp.status << " body="
                          << bybit::truncate_for_log(resp.body) << "\n";
            }
        }
    }

    std::vector<truetest::exits::IBracketAdapter::recovered_bracket>
    list_open() override
    {
        std::vector<truetest::exits::IBracketAdapter::recovered_bracket> out;
        if (!get_)
            return out;

        // Open + recent conditional orders. Filter by our client id prefix.
        const std::string q =
            "category=" + category_ + "&openOnly=0&limit=50";
        auto resp = get_(bybit::paths::order_realtime, q);
        if (resp.status < 200 || resp.status >= 300
            || !bybit::is_business_success(resp.status, resp.body))
        {
            std::cerr << "BybitFuturesBracketAdapter: order/realtime HTTP "
                      << resp.status
                      << " — restart recovery skipped\n";
            return out;
        }

        auto result = bybit::detail::extract_object(resp.body, "result");
        auto arr = bybit::detail::extract_array(
            result.empty() ? std::string_view(resp.body) : result, "list");
        if (arr.empty())
            return out;

        struct partial
        {
            truetest::exits::IBracketAdapter::recovered_bracket rb;
            bool sl_seen = false;
            bool tp_seen = false;
        };
        std::unordered_map<std::uint64_t, partial> by_opener;

        constexpr std::string_view sl_pref = "tt-fb-sl-";
        constexpr std::string_view tp_pref = "tt-fb-tp-";

        bybit::detail::for_each_array_object(arr, [&](std::string_view obj) {
            auto cli = bybit::extract_sv_string(obj, "orderLinkId");
            const bool is_sl = cli.size() > sl_pref.size()
                && cli.substr(0, sl_pref.size()) == sl_pref;
            const bool is_tp = cli.size() > tp_pref.size()
                && cli.substr(0, tp_pref.size()) == tp_pref;
            if (!is_sl && !is_tp) return;

            std::uint64_t opener = 0;
            try
            {
                const auto pref = is_sl ? sl_pref : tp_pref;
                opener = static_cast<std::uint64_t>(
                    std::stoull(std::string(cli.substr(pref.size()))));
            }
            catch (...)
            {
                return;
            }

            auto& p = by_opener[opener];
            auto& rb = p.rb;
            rb.opener_order_id = opener;

            auto sym = bybit::extract_sv_string(obj, "symbol");
            rb.symbol.assign(sym.data(), sym.size());
            rb.handles.symbol = rb.symbol;

            auto side = bybit::extract_sv_string(obj, "side");
            rb.close_side = (side == "Sell" || side == "sell" || side == "SELL")
                ? order_side::sell
                : order_side::buy;

            auto qty_sv = bybit::extract_sv_string(obj, "qty");
            if (qty_sv.empty())
                qty_sv = bybit::extract_sv_number(obj, "qty");
            double qty_d = 0.0;
            if (!qty_sv.empty() && bybit::parse_double_sv(qty_sv, qty_d)
                && qty_d > 0.0)
                rb.qty = qty_d;

            auto trig = bybit::extract_sv_string(obj, "triggerPrice");
            if (trig.empty())
                trig = bybit::extract_sv_number(obj, "triggerPrice");
            double trig_d = 0.0;
            if (!trig.empty())
                (void)bybit::parse_double_sv(trig, trig_d);

            auto id = bybit::extract_sv_string(obj, "orderId");
            if (id.empty())
                id = bybit::extract_sv_number(obj, "orderId");

            if (is_sl)
            {
                if (trig_d > 0.0) rb.stop_loss = trig_d;
                if (!id.empty()) rb.handles.sl_exchange_id = std::string(id);
                p.sl_seen = true;
            }
            else
            {
                if (trig_d > 0.0) rb.take_profit = trig_d;
                if (!id.empty()) rb.handles.tp_exchange_id = std::string(id);
                p.tp_seen = true;
            }
        });

        for (auto& [opener, p] : by_opener)
        {
            (void)opener;
            if (p.rb.stop_loss && p.rb.take_profit)
                p.rb.entry_price = (*p.rb.stop_loss + *p.rb.take_profit) * 0.5;
            else if (p.rb.stop_loss)
                p.rb.entry_price = *p.rb.stop_loss;
            else if (p.rb.take_profit)
                p.rb.entry_price = *p.rb.take_profit;
            out.push_back(std::move(p.rb));
        }
        return out;
    }

private:
    post_fn post_;
    get_fn get_;
    std::string category_;

    std::string place_leg(const std::string& symbol,
                          const char* side,
                          double trigger_price,
                          int trigger_direction,
                          double qty,
                          const std::string& client_id,
                          std::uint64_t opener_order_id,
                          const char* tag)
    {
        std::string body;
        body.reserve(280 + symbol.size() + client_id.size());
        body.push_back('{');
        append_kv_str(body, "category", category_, /*first=*/true);
        append_kv_str(body, "symbol", symbol);
        append_kv_str(body, "side", side);
        append_kv_str(body, "orderType", "Market");
        append_kv_str(body, "qty", fmt_double(qty));
        append_kv_str(body, "triggerPrice", fmt_double(trigger_price));
        append_kv_str(body, "triggerBy", "MarkPrice");
        append_kv_num(body, "triggerDirection", trigger_direction);
        append_kv_bool(body, "reduceOnly", true);
        append_kv_bool(body, "closeOnTrigger", true);
        append_kv_num(body, "positionIdx", 0);
        append_kv_str(body, "orderLinkId", client_id);
        body.push_back('}');

        auto resp = post_(bybit::paths::order_create, body);
        if (!bybit::is_business_success(resp.status, resp.body))
        {
            std::cerr << "BybitFuturesBracketAdapter: " << tag
                      << " place failed for opener=" << opener_order_id
                      << " HTTP " << resp.status << " body="
                      << bybit::truncate_for_log(resp.body) << "\n";
            return {};
        }

        // result.orderId preferred.
        auto result = bybit::detail::extract_object(resp.body, "result");
        const std::string_view root =
            result.empty() ? std::string_view(resp.body) : result;
        auto id = bybit::extract_sv_string(root, "orderId");
        if (id.empty())
            id = bybit::extract_sv_number(root, "orderId");
        if (id.empty())
        {
            // Fallback: top-level scan.
            id = bybit::extract_sv_string(resp.body, "orderId");
            if (id.empty())
                id = bybit::extract_sv_number(resp.body, "orderId");
        }
        if (id.empty())
        {
            std::cerr << "BybitFuturesBracketAdapter: " << tag
                      << " place OK but no orderId for opener="
                      << opener_order_id << "\n";
            return {};
        }
        return std::string(id);
    }

    static bool ok_cancel(const response& r)
    {
        if (r.status < 200 || r.status >= 300)
            return false;
        if (bybit::is_business_success(r.status, r.body))
            return true;
        // Already gone / filled — common on flatten paths.
        auto code = bybit::extract_ret_code(r.body);
        return code == "110001" || code == "110010" || code == "20001";
    }

    static std::string upper(std::string s)
    {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    static std::string fmt_double(double v)
    {
        char tmp[48];
        std::snprintf(tmp, sizeof(tmp), "%.8f", v);
        std::string s(tmp);
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
        }
        return s;
    }

    static void append_kv_str(std::string& out, std::string_view key,
                              std::string_view value, bool first = false)
    {
        if (!first)
            out.push_back(',');
        out.push_back('"');
        out.append(key);
        out.append("\":\"");
        out.append(value);
        out.push_back('"');
    }

    static void append_kv_num(std::string& out, std::string_view key, int v)
    {
        out.push_back(',');
        out.push_back('"');
        out.append(key);
        out.append("\":");
        out.append(std::to_string(v));
    }

    static void append_kv_bool(std::string& out, std::string_view key, bool v)
    {
        out.push_back(',');
        out.push_back('"');
        out.append(key);
        out.append("\":");
        out.append(v ? "true" : "false");
    }
};

inline std::shared_ptr<BybitFuturesBracketAdapter>
make_bybit_futures_bracket_adapter(std::shared_ptr<BybitRestClient> rest,
                                   std::string category = "linear")
{
    BybitFuturesBracketAdapter::post_fn post;
    BybitFuturesBracketAdapter::get_fn get;
    if (rest)
    {
        post = [rest](std::string_view ep, std::string_view body)
            -> BybitFuturesBracketAdapter::response {
            auto r = rest->post_json(std::string(ep), std::string(body));
            return {r.status, std::move(r.body)};
        };
        get = [rest](std::string_view ep, std::string_view q)
            -> BybitFuturesBracketAdapter::response {
            auto r = rest->get(std::string(ep), std::string(q));
            return {r.status, std::move(r.body)};
        };
    }
    return std::make_shared<BybitFuturesBracketAdapter>(
        std::move(post), std::move(get), std::move(category));
}

#endif // HAS_BYBIT
