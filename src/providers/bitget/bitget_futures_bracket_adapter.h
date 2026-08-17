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
#include "providers/recovery_payload.h"

#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
                                std::string category = "USDT-FUTURES",
                                std::string configured_symbol = {})
        : post_(std::move(post))
        , get_(std::move(get))
        , category_(std::move(category))
        , configured_symbol_(upper(std::move(configured_symbol)))
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
        append_kv(body, "reduceOnly", "yes");
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

        if (!provider_recovery::is_authoritative_object(resp.body))
            throw std::runtime_error(
                "Bitget bracket placement returned malformed 2xx payload");
        std::string_view data;
        if (!provider_recovery::top_level_member(resp.body, "data", data)
            || !provider_recovery::is_authoritative_object(data))
            throw std::runtime_error(
                "Bitget bracket placement returned incomplete identity");

        std::uint64_t parsed_id = 0;
        std::string_view returned_cli;
        if (!provider_recovery::top_level_positive_u64(
                data, "orderId", parsed_id)
            || !provider_recovery::top_level_plain_string(
                data, "clientOid", returned_cli)
            || returned_cli != cli)
            throw std::runtime_error(
                "Bitget bracket placement returned ambiguous order identity");
        const auto id = std::to_string(parsed_id);

        // One strategy order owns both legs — store id on both handles so
        // cancel() is idempotent whether engine cancels SL or TP path.
        handles.sl_exchange_id = id;
        handles.tp_exchange_id = id;
        handles.oco_list_id = id;
        handles.symbol = symbol;
        return handles;
    }

    void cancel(std::uint64_t opener_order_id,
                const truetest::exits::bracket_handles& handles) override
    {
        (void)opener_order_id;
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
        if (resp.status >= 200 && resp.status < 300
            && bitget::extract_business_code(resp.body) == "00000")
        {
            std::string_view msg;
            std::string_view data;
            if (!provider_recovery::top_level_plain_string(
                    resp.body, "msg", msg)
                || msg != "success"
                || !provider_recovery::top_level_member(
                    resp.body, "data", data)
                || !provider_recovery::is_exact_null(data))
                throw std::runtime_error(
                    "Bitget bracket cancel returned ambiguous 2xx payload");
            return;
        }
        // Exact authoritative already-gone codes prove no resting strategy
        // order remains.  Every other response is a terminal uncertainty.
        const auto code = bitget::extract_business_code(resp.body);
        if (code == "25204" || code == "24056" || code == "22001") return;
        throw std::runtime_error(
            "Bitget bracket cancel did not prove venue cancellation");
    }

    std::vector<truetest::exits::IBracketAdapter::recovered_bracket>
    list_open() override
    {
        std::vector<truetest::exits::IBracketAdapter::recovered_bracket> out;
        if (!get_)
            throw std::runtime_error(
                "Bitget futures bracket recovery unavailable: missing GET transport");

        const std::string q =
            "category=" + category_ + "&type=tpsl";
        auto resp = get_("/api/v3/trade/unfilled-strategy-orders", q);
        if (resp.status < 200 || resp.status >= 300
            || !bitget::is_business_success(resp.status, resp.body))
        {
            throw std::runtime_error(
                "Bitget futures bracket recovery failed: unfilled-strategy-orders HTTP "
                + std::to_string(resp.status));
        }

        if (!provider_recovery::is_valid_document(resp.body))
            throw std::runtime_error(
                "Bitget futures bracket recovery failed: malformed response payload");

        std::string_view arr;
        if (!provider_recovery::top_level_member(resp.body, "data", arr)
            || !provider_recovery::is_authoritative_object_array(arr))
            throw std::runtime_error(
                "Bitget futures bracket recovery failed: missing or malformed data array");
        if (!provider_recovery::every_top_level_object(
                arr, [](std::string_view obj) {
                    std::string_view client;
                    return provider_recovery::top_level_plain_string(
                               obj, "clientOid", client)
                        && !client.empty();
                }))
            throw std::runtime_error(
                "Bitget futures bracket recovery failed: strategy identity missing");

        std::unordered_set<std::uint64_t> recovered_openers;
        std::unordered_set<std::string> recovered_order_ids;
        bitget::detail::for_each_array_object(arr, [&](std::string_view obj) {
            std::string_view cli;
            if (!provider_recovery::top_level_plain_string(
                    obj, "clientOid", cli))
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: clientOid missing");
            constexpr std::string_view pref = "tt-fb-";
            if (cli.size() <= pref.size()
                || cli.substr(0, pref.size()) != pref)
                return;
            std::string_view symbol;
            if (!provider_recovery::top_level_plain_string(
                    obj, "symbol", symbol))
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: symbol missing");
            if (!configured_symbol_.empty() && symbol != configured_symbol_)
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: TrueTest strategy order belongs to an unexpected symbol");
            std::uint64_t opener = 0;
            if (!provider_recovery::parse_positive_u64(
                    cli.substr(pref.size()), opener))
            {
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: invalid TrueTest clientOid");
            }
            if (!recovered_openers.insert(opener).second)
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: duplicate TrueTest opener");

            std::string_view category;
            std::string_view status;
            if (!provider_recovery::top_level_plain_string(
                    obj, "category", category)
                || category != category_
                || !provider_recovery::top_level_plain_string(
                    obj, "status", status)
                || (status != "pending" && status != "submitting"))
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: strategy row is not authoritatively open for this category");

            truetest::exits::IBracketAdapter::recovered_bracket rb;
            rb.opener_order_id = opener;
            rb.symbol = std::string(symbol);
            rb.handles.symbol = rb.symbol;

            std::uint64_t parsed_id = 0;
            if (rb.symbol.empty()
                || !provider_recovery::top_level_positive_u64(
                    obj, "orderId", parsed_id))
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: incomplete TrueTest strategy order");
            const std::string id = std::to_string(parsed_id);
            if (!recovered_order_ids.insert(std::string(id)).second)
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: duplicate venue orderId");
            rb.handles.sl_exchange_id = id;
            rb.handles.tp_exchange_id = id;
            rb.handles.oco_list_id = id;

            std::string_view sl;
            std::string_view tp;
            if (!provider_recovery::top_level_scalar_text(
                    obj, "stopLoss", sl)
                || !provider_recovery::top_level_scalar_text(
                    obj, "takeProfit", tp))
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: protection prices missing");
            double sl_d = 0, tp_d = 0;
            if (!sl.empty() && bitget::parse_double_sv(sl, sl_d))
                rb.stop_loss = sl_d;
            if (!tp.empty() && bitget::parse_double_sv(tp, tp_d))
                rb.take_profit = tp_d;
            if (!rb.stop_loss || !rb.take_profit || *rb.stop_loss <= 0.0
                || *rb.take_profit <= 0.0)
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: TrueTest protection legs incomplete");
            if (rb.stop_loss && rb.take_profit)
                rb.entry_price = (*rb.stop_loss + *rb.take_profit) * 0.5;
            else if (rb.stop_loss)
                rb.entry_price = *rb.stop_loss;
            else if (rb.take_profit)
                rb.entry_price = *rb.take_profit;

            std::string_view side;
            if (!provider_recovery::top_level_plain_string(
                    obj, "side", side))
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: close side missing");
            if (side == "sell" || side == "SELL")
                rb.close_side = order_side::sell;
            else if (side == "buy" || side == "BUY")
                rb.close_side = order_side::buy;
            else
                throw std::runtime_error(
                    "Bitget futures bracket recovery failed: invalid close side");

            out.push_back(std::move(rb));
        });
        return out;
    }

private:
    post_fn post_;
    get_fn get_;
    std::string category_;
    std::string configured_symbol_;

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
                                    std::string category = "USDT-FUTURES",
                                    std::shared_ptr<std::atomic<bool>> cancelled =
                                        std::make_shared<std::atomic<bool>>(false),
                                    std::string configured_symbol = {})
{
    BitgetFuturesBracketAdapter::post_fn post;
    BitgetFuturesBracketAdapter::get_fn get;
    if (rest)
    {
        constexpr auto deadline = std::chrono::milliseconds{1500};
        post = [rest, cancelled](std::string_view ep, std::string_view body)
            -> BitgetFuturesBracketAdapter::response {
            if (!rest->ensure_clock_fresh_for_order(std::chrono::milliseconds{500}))
                throw std::runtime_error("clock refresh failed before Bitget bracket mutation");
            auto r = rest->safety_post_json(
                std::string(ep), std::string(body), deadline, cancelled.get());
            if (r.request_written && (r.status == 0 || r.status >= 500))
                throw std::runtime_error("ambiguous post-write Bitget bracket mutation");
            return {r.status, std::move(r.body)};
        };
        get = [rest, cancelled](std::string_view ep, std::string_view q)
            -> BitgetFuturesBracketAdapter::response {
            if (!rest->ensure_clock_fresh_for_order(std::chrono::milliseconds{500}))
                throw std::runtime_error("clock refresh failed before Bitget bracket query");
            auto r = rest->safety_get(
                std::string(ep), std::string(q), deadline, cancelled.get());
            return {r.status, std::move(r.body)};
        };
    }
    return std::make_shared<BitgetFuturesBracketAdapter>(
        std::move(post), std::move(get), std::move(category),
        std::move(configured_symbol));
}

#endif // HAS_BITGET
