#pragma once
#ifdef HAS_BINANCE

#include "exits/bracket_adapter.h"
#include "exits/exit_intent.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_rest_client.h"
#include "providers/recovery_payload.h"

#include <cstdio>
#include <atomic>
#include <chrono>
#include <cmath>
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

// USDT-M futures bracket adapter. Places SL+TP as two SEPARATE conditional
// algo orders (STOP_MARKET + TAKE_PROFIT_MARKET), both with
// closePosition=true. Binance forbids quantity/reduceOnly in that shape.
// There is no futures OCO endpoint, so
// placement is NOT atomic - if the second POST fails after the first
// succeeds, we have a hanging leg that cancel() must clean up.
// `closePosition=true` prevents either leg from intentionally opening reverse
// exposure, but it is not treated as an OCO/sibling-cancel guarantee. Engine
// handling and restart reconciliation must still account for both venue IDs.
// Constraint: closePosition=true requires the order to close the entire
// position. Partial brackets (qty_fraction < 1.0 for TP1/TP2 scale-outs)
// cannot use closePosition=true without splitting into a different
// shape; this adapter declines them and the engine-side ExitManager
// remains the only enforcer for that intent. Single full-position
// SL+TP brackets are the supported case - same scope as spot OCO.
class BinanceFuturesBracketAdapter : public truetest::exits::IBracketAdapter
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using request_fn = std::function<response(std::string_view endpoint,
                                              std::string_view params)>;

    BinanceFuturesBracketAdapter(request_fn post,
                                 request_fn del,
                                 request_fn get = nullptr,
                                 std::string configured_symbol = {})
        : post_(std::move(post))
        , del_(std::move(del))
        , get_(std::move(get))
        , configured_symbol_(upper(std::move(configured_symbol)))
    {}

    truetest::exits::bracket_caps capabilities() const override
    {
        truetest::exits::bracket_caps c;
        c.stop_market = true;
        // oco intentionally false: placement is two separate POSTs and the
        // venue contract does not prove atomic sibling cancellation.
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

        // Single-bracket-only: declines on partial intents (TP1/TP2
        // scale-outs) and on intents missing either leg. Same contract
        // as the spot OCO adapter.
        if (!intent.stop_loss || !intent.take_profit)
        {
            std::cerr << "BinanceFuturesBracketAdapter: intent missing SL or TP "
                         "(opener=" << opener_order_id << ") - declining; "
                         "engine-side eval remains the only enforcer\n";
            return handles;
        }
        if (intent.qty_fraction < 0.999999 || intent.qty_fraction > 1.000001)
        {
            std::cerr << "BinanceFuturesBracketAdapter: partial-fraction "
                         "intent (qty_fraction=" << intent.qty_fraction
                      << ") not supported with closePosition=true; "
                         "declining (opener=" << opener_order_id << ")\n";
            return handles;
        }
        if (!post_) return handles;

        const std::string side  = (intent.close_side == order_side::sell)
                                    ? "SELL" : "BUY";
        const std::string symbol = upper(intent.symbol);

        // Place SL first. If it succeeds and TP fails, we still have a
        // protective stop and cancel() can roll back the SL on teardown.
        const std::string sl_cli = "tt-fb-sl-" + std::to_string(opener_order_id);
        const std::string tp_cli = "tt-fb-tp-" + std::to_string(opener_order_id);

        auto sl_id = place_leg("STOP_MARKET", symbol, side,
                               *intent.stop_loss, sl_cli, opener_order_id);
        if (sl_id.empty()) return handles;
        handles.sl_exchange_id = sl_id;
        handles.symbol = symbol;  // populated even on partial-leg success
                                  // so cancel() can scrub a hanging SL

        std::string tp_id;
        try
        {
            tp_id = place_leg("TAKE_PROFIT_MARKET", symbol, side,
                              *intent.take_profit, tp_cli, opener_order_id);
        }
        catch (...)
        {
            // A 2xx response without an authoritative identity may mean the
            // venue accepted the TP even though we cannot track it. Scrub the
            // already-known SL once, then preserve the placement ambiguity by
            // rethrowing so the live engine enters its terminal halt path.
            try { cancel(opener_order_id, handles); }
            catch (...) {}
            throw;
        }
        if (tp_id.empty())
        {
            // Hanging SL after TP failure: log loudly, return what we have.
            // The engine will see SL-only handles and rely on its own TP.
            // cancel() on teardown will sweep the SL.
            std::cerr << "BinanceFuturesBracketAdapter: TP leg failed for opener="
                      << opener_order_id << " - SL is placed (orderId="
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
        if (!del_ || handles.empty()) return;

        // Per-leg DELETE. We don't try the cancel-orderList endpoint
        // because there's no list - placement is two independent orders.
        // -2011 / -2013 ("unknown order" / "order does not exist") arrive
        // when the venue already closed the leg via closePosition=true
        // auto-cancel; treat as success.
        struct leg { const std::optional<std::string>* id; const char* tag; };
        const leg legs[] = {
            {&handles.sl_exchange_id, "SL"},
            {&handles.tp_exchange_id, "TP"},
        };
        bool cancel_failed = false;
        for (const auto& l : legs)
        {
            if (!*l.id) continue;
            std::string params;
            binance::append_param(params, "algoId", **l.id);
            try
            {
                auto resp = del_("/fapi/v1/algoOrder", params);
                if (resp.status >= 200 && resp.status < 300)
                {
                    std::uint64_t returned_id = 0;
                    std::uint64_t expected_id = 0;
                    const std::string expected_client = std::string(
                        std::string_view{l.tag} == "SL"
                            ? "tt-fb-sl-" : "tt-fb-tp-")
                        + std::to_string(opener_order_id);
                    std::string_view returned_client;
                    if (provider_recovery::parse_positive_u64(
                            **l.id, expected_id)
                        && provider_recovery::top_level_positive_u64(
                            resp.body, "algoId", returned_id)
                        && returned_id == expected_id
                        && provider_recovery::top_level_plain_string(
                            resp.body, "clientAlgoId", returned_client)
                        && returned_client == expected_client
                        && provider_recovery::has_exact_top_level_code(
                            resp.body, 200)
                        && provider_recovery::top_level_exact_string(
                            resp.body, "msg", "success"))
                        continue;
                    cancel_failed = true;
                    continue;
                }
                if (provider_recovery::has_exact_top_level_code(
                        resp.body, -2011)
                    || provider_recovery::has_exact_top_level_code(
                        resp.body, -2013))
                    continue;
                cancel_failed = true;
            }
            catch (...) { cancel_failed = true; }
        }
        if (cancel_failed)
            throw std::runtime_error(
                "Binance futures bracket cancel did not prove every venue cancellation");
    }

    std::vector<truetest::exits::IBracketAdapter::recovered_bracket>
    list_open() override
    {
        std::vector<truetest::exits::IBracketAdapter::recovered_bracket> out;
        if (!get_)
            throw std::runtime_error(
                "Binance futures bracket recovery unavailable: missing GET transport");

        std::string query;
        binance::append_param(query, "algoType", "CONDITIONAL");
        if (!configured_symbol_.empty())
            binance::append_param(query, "symbol", configured_symbol_);
        auto resp = get_("/fapi/v1/openAlgoOrders", query);
        if (resp.status < 200 || resp.status >= 300)
        {
            throw std::runtime_error(
                "Binance futures bracket recovery failed: openAlgoOrders HTTP "
                + std::to_string(resp.status));
        }
        if (!provider_recovery::is_authoritative_object_array(resp.body))
            throw std::runtime_error(
                "Binance futures bracket recovery failed: malformed openAlgoOrders payload");
        // Walk the array, parse one object at a time, group SL/TP per
        // opener_id we extract from the clientOrderId suffix.
        struct partial
        {
            truetest::exits::IBracketAdapter::recovered_bracket rb;
            bool sl_seen = false;
            bool tp_seen = false;
        };
        std::unordered_map<std::uint64_t, partial> by_opener;
        std::unordered_set<std::string> recovered_order_ids;

        const std::string sl_pref = "tt-fb-sl-";
        const std::string tp_pref = "tt-fb-tp-";
        const bool rows_ok = provider_recovery::every_top_level_object(
            resp.body, [&](std::string_view obj) {
            std::string_view cli_sv;
            if (!provider_recovery::top_level_plain_string(
                    obj, "clientAlgoId", cli_sv)) return false;
            std::string cli(cli_sv);
            bool is_sl = cli.size() > sl_pref.size() &&
                         cli.compare(0, sl_pref.size(), sl_pref) == 0;
            bool is_tp = cli.size() > tp_pref.size() &&
                         cli.compare(0, tp_pref.size(), tp_pref) == 0;
            if (!is_sl && !is_tp) return true;

            std::uint64_t opener = 0;
            const auto& pref = is_sl ? sl_pref : tp_pref;
            if (!provider_recovery::parse_positive_u64(
                    std::string_view(cli).substr(pref.size()), opener))
            {
                throw std::runtime_error(
                    "Binance futures bracket recovery failed: invalid TrueTest clientAlgoId");
            }

            auto& p = by_opener[opener];
            if ((is_sl && p.sl_seen) || (is_tp && p.tp_seen))
                throw std::runtime_error(
                    "Binance futures bracket recovery failed: duplicate TrueTest protection leg");
            auto& rb = p.rb;

            std::string_view symbol_view;
            std::string_view side_view;
            std::string_view trigger;
            std::string_view order_type;
            std::uint64_t parsed_id = 0;
            if (!provider_recovery::top_level_plain_string(
                    obj, "symbol", symbol_view)
                || !provider_recovery::top_level_plain_string(
                    obj, "side", side_view)
                || !provider_recovery::top_level_scalar_text(
                    obj, "triggerPrice", trigger)
                || !provider_recovery::top_level_plain_string(
                    obj, "orderType", order_type)
                || !provider_recovery::top_level_positive_u64(
                    obj, "algoId", parsed_id)
                || !provider_recovery::top_level_exact_string(
                    obj, "algoType", "CONDITIONAL")
                || !provider_recovery::top_level_exact_string(
                    obj, "algoStatus", "NEW")
                || !provider_recovery::top_level_exact_string(
                    obj, "positionSide", "BOTH")
                || !top_level_bool(obj, "closePosition", true)
                || !top_level_bool(obj, "reduceOnly", false))
                throw std::runtime_error(
                    "Binance futures bracket recovery failed: incomplete TrueTest algo order");
            const std::string symbol(symbol_view);
            const std::string side(side_view);
            if (!configured_symbol_.empty() && symbol != configured_symbol_)
                throw std::runtime_error(
                    "Binance futures bracket recovery returned an unexpected symbol");
            const std::string id = std::to_string(parsed_id);
            if (side != "BUY" && side != "SELL")
                throw std::runtime_error(
                    "Binance futures bracket recovery failed: invalid close side");
            if (!recovered_order_ids.insert(id).second)
                throw std::runtime_error(
                    "Binance futures bracket recovery failed: duplicate venue algoId");
            if (!rb.symbol.empty()
                && (rb.symbol != symbol
                    || rb.close_side != ((side == "SELL")
                        ? order_side::sell : order_side::buy)))
                throw std::runtime_error(
                    "Binance futures bracket recovery failed: inconsistent TrueTest legs");

            rb.opener_order_id = opener;
            rb.symbol = symbol;
            rb.handles.symbol = symbol;
            rb.close_side = (side == "SELL") ? order_side::sell : order_side::buy;
            double stop_d = 0.0;
            if (!binance::parse_double_sv(trigger, stop_d) || stop_d <= 0.0)
                throw std::runtime_error(
                    "Binance futures bracket recovery failed: invalid trigger price");

            if (is_sl)
            {
                if (order_type != "STOP_MARKET")
                    throw std::runtime_error(
                        "Binance futures bracket recovery failed: invalid stop order type");
                rb.stop_loss = stop_d;
                if (!id.empty()) rb.handles.sl_exchange_id = id;
                p.sl_seen = true;
            }
            else
            {
                if (order_type != "TAKE_PROFIT_MARKET")
                    throw std::runtime_error(
                        "Binance futures bracket recovery failed: invalid take-profit order type");
                rb.take_profit = stop_d;
                if (!id.empty()) rb.handles.tp_exchange_id = id;
                p.tp_seen = true;
            }
            return true;
        });
        if (!rows_ok)
            throw std::runtime_error(
                "Binance futures bracket recovery failed: ambiguous order schema");

        for (auto& [opener, p] : by_opener)
        {
            // Approximate entry as midpoint (matches spot adapter's
            // recovery behavior; portfolio holds true cost basis).
            if (p.rb.stop_loss && p.rb.take_profit)
                p.rb.entry_price = (*p.rb.stop_loss + *p.rb.take_profit) * 0.5;
            else if (p.rb.stop_loss)   p.rb.entry_price = *p.rb.stop_loss;
            else if (p.rb.take_profit) p.rb.entry_price = *p.rb.take_profit;
            out.push_back(std::move(p.rb));
        }
        return out;
    }

private:
    request_fn post_;
    request_fn del_;
    request_fn get_;
    std::string configured_symbol_;

    // Returns the algoId on authoritative success, "" on a known venue
    // decline, and throws when a 2xx response cannot prove the accepted
    // order's identity.
    std::string place_leg(const char* type,
                          const std::string& symbol,
                          const std::string& side,
                          double trigger_price,
                          const std::string& client_id,
                          std::uint64_t opener_order_id)
    {
        // closePosition=true means quantity is omitted (Binance derives
        // it from current position size at trigger time) and already carries
        // close-only semantics. Binance forbids also sending reduceOnly.
        std::string params;
        params.reserve(192);
        binance::append_param(params, "algoType", "CONDITIONAL");
        binance::append_param(params, "symbol", symbol);
        binance::append_param(params, "side", side);
        binance::append_param(params, "type", type);
        binance::append_param(params, "triggerPrice", fmt_double(trigger_price));
        binance::append_param(params, "closePosition", "true");
        binance::append_param(params, "clientAlgoId", client_id);

        auto resp = post_("/fapi/v1/algoOrder", params);
        if (resp.status < 200 || resp.status >= 300)
        {
            std::cerr << "BinanceFuturesBracketAdapter: " << type
                      << " place failed for opener=" << opener_order_id
                      << " HTTP " << resp.status << " body="
                      << binance::redact_for_log(resp.body, 240)
                      << "\n";
            return {};
        }

        if (!provider_recovery::is_authoritative_object(resp.body))
            throw std::runtime_error(
                "Binance futures bracket placement returned malformed 2xx payload");

        std::uint64_t parsed_id = 0;
        std::string_view returned_client_id;
        if (!provider_recovery::top_level_positive_u64(
                resp.body, "algoId", parsed_id)
            || !provider_recovery::top_level_plain_string(
                resp.body, "clientAlgoId", returned_client_id)
            || returned_client_id != client_id
            || !provider_recovery::top_level_exact_string(
                resp.body, "algoType", "CONDITIONAL")
            || !provider_recovery::top_level_exact_string(
                resp.body, "orderType", type)
            || !provider_recovery::top_level_exact_string(
                resp.body, "symbol", symbol)
            || !provider_recovery::top_level_exact_string(
                resp.body, "side", side)
            || !provider_recovery::top_level_exact_string(
                resp.body, "positionSide", "BOTH")
            || !provider_recovery::top_level_exact_string(
                resp.body, "algoStatus", "NEW")
            || !top_level_bool(resp.body, "closePosition", true)
            || !top_level_bool(resp.body, "reduceOnly", false))
            throw std::runtime_error(
                "Binance futures bracket placement returned ambiguous order identity");
        std::string_view returned_trigger;
        double parsed_trigger = 0.0;
        if (!provider_recovery::top_level_scalar_text(
                resp.body, "triggerPrice", returned_trigger)
            || !binance::parse_double_sv(returned_trigger, parsed_trigger)
            || std::abs(parsed_trigger - trigger_price) > 1e-9)
            throw std::runtime_error(
                "Binance futures bracket placement returned ambiguous trigger price");
        return std::to_string(parsed_id);
    }

    static bool top_level_bool(std::string_view object,
                               std::string_view key,
                               bool expected)
    {
        std::string_view raw;
        if (!provider_recovery::top_level_member(object, key, raw))
            return false;
        raw = provider_recovery::trim_json_ws(raw);
        return raw == (expected ? "true" : "false");
    }

    static std::string upper(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(::toupper(c));
        return s;
    }

    static std::string fmt_double(double v)
    {
        char tmp[48];
        std::snprintf(tmp, sizeof(tmp), "%.8f", v);
        std::string s(tmp);
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.')   s.pop_back();
        }
        return s;
    }
};

inline std::shared_ptr<BinanceFuturesBracketAdapter>
make_binance_futures_bracket_adapter(
    std::shared_ptr<BinanceRestClient> client,
    std::shared_ptr<std::atomic<bool>> cancelled =
        std::make_shared<std::atomic<bool>>(false),
    std::string configured_symbol = {})
{
    constexpr auto deadline = std::chrono::milliseconds{1500};
    auto post = [client, cancelled](std::string_view ep, std::string_view p)
        -> BinanceFuturesBracketAdapter::response
    {
        if (!client->ensure_clock_fresh_for_order(std::chrono::milliseconds{500}))
            throw std::runtime_error("clock refresh failed before futures bracket placement");
        auto r = client->safety_post(
            std::string(ep), std::string(p), deadline, cancelled.get());
        if (r.request_written && (r.status == 0 || r.status >= 500))
            throw std::runtime_error("ambiguous post-write futures bracket placement");
        return {r.status, r.body};
    };
    auto del = [client, cancelled](std::string_view ep, std::string_view p)
        -> BinanceFuturesBracketAdapter::response
    {
        if (!client->ensure_clock_fresh_for_order(std::chrono::milliseconds{500}))
            throw std::runtime_error("clock refresh failed before futures bracket cancellation");
        auto r = client->safety_del(
            std::string(ep), std::string(p), deadline, cancelled.get());
        if (r.request_written && (r.status == 0 || r.status >= 500))
            throw std::runtime_error("ambiguous post-write futures bracket cancellation");
        return {r.status, r.body};
    };
    auto get = [client, cancelled](std::string_view ep, std::string_view p)
        -> BinanceFuturesBracketAdapter::response
    {
        if (!client->ensure_clock_fresh_for_order(std::chrono::milliseconds{500}))
            throw std::runtime_error("clock refresh failed before futures bracket query");
        auto r = client->safety_get(
            std::string(ep), std::string(p), deadline, cancelled.get());
        return {r.status, r.body};
    };
    return std::make_shared<BinanceFuturesBracketAdapter>(
        std::move(post), std::move(del), std::move(get),
        std::move(configured_symbol));
}

#endif // HAS_BINANCE
