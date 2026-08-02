#pragma once
#ifdef HAS_BITGET

// Bitget UTA futures bracket adapter (Phase 4).
// Places SL+TP as one strategy order (type=tpsl, tpslMode=full) via
// POST /api/v3/trade/place-strategy-order. Cancel via
// POST /api/v3/trade/cancel-strategy-order. Restart recovery via
// GET /api/v3/trade/unfilled-strategy-orders.
//
// Single full-position brackets only (qty_fraction ~ 1). Partial scale-outs
// decline → engine-side ExitManager remains enforcer.

#include "exits/bracket_adapter.h"
#include "exits/exit_intent.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/bitget/bitget_rest_client.h"

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

class BitgetFuturesBracketAdapter : public truetest::exits::IBracketAdapter
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

    BitgetFuturesBracketAdapter(post_fn post,
                                get_fn get,
                                std::string category = "USDT-FUTURES")
        : post_(std::move(post))
        , get_(std::move(get))
        , category_(std::move(category))
    {}

    truetest::exits::bracket_caps capabilities() const override
    {
        truetest::exits::bracket_caps c;
        c.stop_market = true;
        c.oco = true; // single strategy order binds TP+SL
        c.stop_limit = false;
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
            std::cerr << "BitgetFuturesBracketAdapter: intent missing SL or TP "
                         "(opener=" << opener_order_id << ") — declining\n";
            return handles;
        }
        if (intent.qty_fraction < 0.999999 || intent.qty_fraction > 1.000001)
        {
            std::cerr << "BitgetFuturesBracketAdapter: partial-fraction intent "
                         "(qty_fraction=" << intent.qty_fraction
                      << ") not supported with tpslMode=full; declining "
                         "(opener=" << opener_order_id << ")\n";
            return handles;
        }
        if (!post_ || category_.empty())
            return handles;

        const std::string symbol = upper(intent.symbol);
        const char* side =
            (intent.close_side == order_side::sell) ? "sell" : "buy";
        // clientOid: tt-fb-{opener} fits charset and ≤32 chars.
        const std::string cli = "tt-fb-" + std::to_string(opener_order_id);

        std::string body;
        body.reserve(256);
        body.push_back('{');
        append_kv(body, "category", category_, true);
        append_kv(body, "symbol", symbol);
        append_kv(body, "type", "tpsl");
        append_kv(body, "tpslMode", "full");
        append_kv(body, "side", side);
        append_kv(body, "stopLoss", fmt_double(*intent.stop_loss));
        append_kv(body, "takeProfit", fmt_double(*intent.take_profit));
        append_kv(body, "slOrderType", "market");
        append_kv(body, "tpOrderType", "market");
        append_kv(body, "slTriggerBy", "mark");
        append_kv(body, "tpTriggerBy", "mark");
        append_kv(body, "clientOid", cli);
        body.push_back('}');

        auto resp = post_("/api/v3/trade/place-strategy-order", body);
        if (!bitget::is_business_success(resp.status, resp.body))
        {
            std::cerr << "BitgetFuturesBracketAdapter: place-strategy-order "
                         "failed opener=" << opener_order_id
                      << " HTTP " << resp.status << " "
                      << bitget::truncate_for_log(resp.body) << "\n";
            return handles;
        }

        auto id = bitget::extract_sv_string(resp.body, "orderId");
        if (id.empty())
            id = bitget::extract_sv_number(resp.body, "orderId");
        if (id.empty())
        {
            std::cerr << "BitgetFuturesBracketAdapter: place OK but no "
                         "orderId for opener=" << opener_order_id << "\n";
            return handles;
        }

        // One strategy order owns both legs — store id on both handles so
        // cancel() is idempotent whether engine cancels SL or TP path.
        handles.sl_exchange_id = std::string(id);
        handles.tp_exchange_id = std::string(id);
        handles.oco_list_id = std::string(id);
        handles.symbol = symbol;
        return handles;
    }

    void cancel(std::uint64_t opener_order_id,
                const truetest::exits::bracket_handles& handles) override
    {
        if (!post_ || handles.empty())
            return;

        std::string id;
        if (handles.oco_list_id)
            id = *handles.oco_list_id;
        else if (handles.sl_exchange_id)
            id = *handles.sl_exchange_id;
        else if (handles.tp_exchange_id)
            id = *handles.tp_exchange_id;
        if (id.empty())
            return;

        std::string body = "{\"orderId\":\"";
        body.append(id);
        body.append("\"}");
        auto resp = post_("/api/v3/trade/cancel-strategy-order", body);
        if (!bitget::is_business_success(resp.status, resp.body))
        {
            // Already gone / filled is common — log but do not escalate.
            auto code = bitget::extract_business_code(resp.body);
            if (code != "25204" && code != "24056" && code != "22001")
            {
                std::cerr << "BitgetFuturesBracketAdapter: cancel strategy "
                             "orderId=" << id << " opener=" << opener_order_id
                          << " HTTP " << resp.status << " "
                          << bitget::truncate_for_log(resp.body) << "\n";
            }
        }
    }

    std::vector<truetest::exits::IBracketAdapter::recovered_bracket>
    list_open() override
    {
        std::vector<truetest::exits::IBracketAdapter::recovered_bracket> out;
        if (!get_)
            return out;

        const std::string q =
            "category=" + category_ + "&type=tpsl";
        auto resp = get_("/api/v3/trade/unfilled-strategy-orders", q);
        if (resp.status < 200 || resp.status >= 300
            || !bitget::is_business_success(resp.status, resp.body))
        {
            std::cerr << "BitgetFuturesBracketAdapter: unfilled-strategy-orders "
                         "HTTP " << resp.status
                      << " — restart recovery skipped\n";
            return out;
        }

        auto arr = bitget::detail::extract_array(resp.body, "data");
        if (arr.empty())
            return out;

        bitget::detail::for_each_array_object(arr, [&](std::string_view obj) {
            auto cli = bitget::extract_sv_string(obj, "clientOid");
            constexpr std::string_view pref = "tt-fb-";
            if (cli.size() <= pref.size()
                || cli.substr(0, pref.size()) != pref)
                return;
            std::uint64_t opener = 0;
            try
            {
                opener = static_cast<std::uint64_t>(
                    std::stoull(std::string(cli.substr(pref.size()))));
            }
            catch (...)
            {
                return;
            }

            truetest::exits::IBracketAdapter::recovered_bracket rb;
            rb.opener_order_id = opener;
            rb.symbol = std::string(bitget::extract_sv_string(obj, "symbol"));
            rb.handles.symbol = rb.symbol;

            auto id = bitget::extract_sv_string(obj, "orderId");
            if (id.empty())
                id = bitget::extract_sv_number(obj, "orderId");
            if (!id.empty())
            {
                rb.handles.sl_exchange_id = std::string(id);
                rb.handles.tp_exchange_id = std::string(id);
                rb.handles.oco_list_id = std::string(id);
            }

            auto sl = bitget::extract_sv_string(obj, "stopLoss");
            if (sl.empty())
                sl = bitget::extract_sv_number(obj, "stopLoss");
            auto tp = bitget::extract_sv_string(obj, "takeProfit");
            if (tp.empty())
                tp = bitget::extract_sv_number(obj, "takeProfit");
            double sl_d = 0, tp_d = 0;
            if (!sl.empty() && bitget::parse_double_sv(sl, sl_d))
                rb.stop_loss = sl_d;
            if (!tp.empty() && bitget::parse_double_sv(tp, tp_d))
                rb.take_profit = tp_d;
            if (rb.stop_loss && rb.take_profit)
                rb.entry_price = (*rb.stop_loss + *rb.take_profit) * 0.5;
            else if (rb.stop_loss)
                rb.entry_price = *rb.stop_loss;
            else if (rb.take_profit)
                rb.entry_price = *rb.take_profit;

            auto side = bitget::extract_sv_string(obj, "side");
            rb.close_side = (side == "sell" || side == "SELL")
                ? order_side::sell
                : order_side::buy;

            out.push_back(std::move(rb));
        });
        return out;
    }

private:
    post_fn post_;
    get_fn get_;
    std::string category_;

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

    static void append_kv(std::string& out, std::string_view key,
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
};

inline std::shared_ptr<BitgetFuturesBracketAdapter>
make_bitget_futures_bracket_adapter(std::shared_ptr<BitgetRestClient> rest,
                                    std::string category = "USDT-FUTURES")
{
    BitgetFuturesBracketAdapter::post_fn post;
    BitgetFuturesBracketAdapter::get_fn get;
    if (rest)
    {
        post = [rest](std::string_view ep, std::string_view body)
            -> BitgetFuturesBracketAdapter::response {
            auto r = rest->post_json(std::string(ep), std::string(body));
            return {r.status, std::move(r.body)};
        };
        get = [rest](std::string_view ep, std::string_view q)
            -> BitgetFuturesBracketAdapter::response {
            auto r = rest->get(std::string(ep), std::string(q));
            return {r.status, std::move(r.body)};
        };
    }
    return std::make_shared<BitgetFuturesBracketAdapter>(
        std::move(post), std::move(get), std::move(category));
}

#endif // HAS_BITGET
