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
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

// Translates ExitManager intents into Binance spot OCO orders.
// Single-bracket only (one SL + one TP per opener) - multi-leg scale-outs
// (TP1/TP2/SL) are deferred; for those the engine-side eval remains the
// only enforcement until a higher-fidelity adapter ships.
// Constructed with two callables (post + del) following the same pattern
// as BinanceRestOrderTransport so production wiring uses the real signed
// REST client and tests inject a fake. The adapter never touches a socket
// itself.
class BinanceOcoBracketAdapter : public truetest::exits::IBracketAdapter
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using request_fn = std::function<response(std::string_view endpoint,
                                              std::string_view params)>;

    using request_get_fn = std::function<response(std::string_view endpoint,
                                                  std::string_view params)>;

    BinanceOcoBracketAdapter(request_fn post, request_fn del,
                             request_get_fn get = nullptr,
                             double stop_limit_offset_pct = 0.001)
        : post_(std::move(post)), del_(std::move(del)),
          get_(std::move(get)),
          stop_limit_offset_pct_(stop_limit_offset_pct)
    {}

    truetest::exits::bracket_caps capabilities() const override
    {
        truetest::exits::bracket_caps c;
        c.stop_limit = true;
        c.oco        = true;
        // stop_market and trailing_stop intentionally false - Binance spot
        // OCO requires a stop-limit leg, not stop-market, and trailing
        // stops aren't part of the OCO contract on spot.
        return c;
    }

    truetest::exits::bracket_handles place(
        std::uint64_t opener_order_id,
        const truetest::exits::exit_intent& intent,
        double opener_fill_price) override
    {
        (void)opener_fill_price;
        truetest::exits::bracket_handles handles;

        // Single-bracket OCO needs both SL and TP. Multi-intent strategies
        // (TP1/TP2 scale-outs) call us once per intent - refusing the
        // partial ones keeps semantics honest until we ship multi-leg.
        if (!intent.stop_loss || !intent.take_profit)
        {
            std::cerr << "BinanceOcoBracketAdapter: intent missing SL or TP "
                         "(opener=" << opener_order_id << ") - declining; "
                         "engine-side eval remains the only enforcer\n";
            return handles;
        }
        if (!post_) return handles;

        const std::string side  = (intent.close_side == order_side::sell)
                                    ? "SELL" : "BUY";
        const std::string symbol = upper(intent.symbol);
        const double sl  = *intent.stop_loss;
        const double tp  = *intent.take_profit;
        const double slim = stop_limit_price_for(intent.close_side, sl);

        // listClientOrderId encodes the engine opener id so the reconciler
        // can rehydrate brackets after a restart without an external store.
        const std::string list_cli = "tt-oco-" + std::to_string(opener_order_id);

        std::string params;
        params.reserve(256);
        binance::append_param(params, "symbol", symbol);
        binance::append_param(params, "side", side);
        binance::append_param(params, "quantity", fmt_double(intent.qty));
        binance::append_param(params, "price", fmt_double(tp));
        binance::append_param(params, "stopPrice", fmt_double(sl));
        binance::append_param(params, "stopLimitPrice", fmt_double(slim));
        binance::append_param(params, "stopLimitTimeInForce", "GTC");
        binance::append_param(params, "listClientOrderId", list_cli);
        binance::append_param(params, "newOrderRespType", "RESULT");

        auto resp = post_("/api/v3/order/oco", params);
        if (resp.status < 200 || resp.status >= 300)
        {
            std::cerr << "BinanceOcoBracketAdapter: place failed for opener="
                      << opener_order_id << " HTTP " << resp.status
                      << " body=" << binance::redact_for_log(resp.body, 240)
                      << " - engine-side eval remains the only enforcer\n";
            return handles;
        }

        // Response shape (RESULT, abbreviated):
        //  {"orderListId":1,"listClientOrderId":"tt-oco-7","contingencyType":"OCO",
        //   "listStatusType":"EXEC_STARTED","listOrderStatus":"EXECUTING",
        //   "transactionTime":...,"symbol":"BTCUSDT",
        //   "orders":[{"symbol":...,"orderId":111,"clientOrderId":...},
        //             {"symbol":...,"orderId":222,"clientOrderId":...}],
        //   "orderReports":[{...,"type":"STOP_LOSS_LIMIT",...,"orderId":111,...},
        //                   {...,"type":"LIMIT_MAKER","orderId":222,...}]}
        if (!provider_recovery::is_authoritative_object(resp.body))
            throw std::runtime_error(
                "Binance OCO placement returned malformed 2xx payload");

        std::uint64_t parsed_list_id = 0;
        std::string_view returned_list_cli;
        std::string_view returned_symbol;
        if (!provider_recovery::top_level_positive_u64(
                resp.body, "orderListId", parsed_list_id)
            || !provider_recovery::top_level_plain_string(
                resp.body, "listClientOrderId", returned_list_cli)
            || !provider_recovery::top_level_plain_string(
                resp.body, "symbol", returned_symbol)
            || returned_list_cli != list_cli || returned_symbol != symbol)
            throw std::runtime_error(
                "Binance OCO placement returned ambiguous list identity");
        const auto list_id = std::to_string(parsed_list_id);

        std::string_view orders;
        std::string_view reports;
        if (!provider_recovery::top_level_member(resp.body, "orders", orders)
            || !provider_recovery::top_level_member(
                resp.body, "orderReports", reports)
            || !provider_recovery::is_authoritative_object_array(orders)
            || !provider_recovery::is_authoritative_object_array(reports))
            throw std::runtime_error(
                "Binance OCO placement returned incomplete leg payload");

        std::unordered_set<std::string> accepted_ids;
        std::size_t accepted_count = 0;
        const bool orders_ok = provider_recovery::every_top_level_object(
            orders, [&](std::string_view obj) {
                std::uint64_t parsed_id = 0;
                if (!provider_recovery::top_level_positive_u64(
                        obj, "orderId", parsed_id))
                    return false;
                const std::string id = std::to_string(parsed_id);
                if (!accepted_ids.insert(id).second)
                    return false;
                ++accepted_count;
                return true;
            });

        std::string sl_id;
        std::string tp_id;
        std::size_t report_count = 0;
        const bool reports_ok = provider_recovery::every_top_level_object(
            reports, [&](std::string_view obj) {
                std::uint64_t parsed_id = 0;
                std::string_view type;
                if (!provider_recovery::top_level_positive_u64(
                        obj, "orderId", parsed_id)
                    || !provider_recovery::top_level_plain_string(
                        obj, "type", type))
                    return false;
                const std::string id = std::to_string(parsed_id);
                if (!accepted_ids.contains(id)) return false;
                if (type == "STOP_LOSS_LIMIT" && sl_id.empty()) sl_id = id;
                else if (type == "LIMIT_MAKER" && tp_id.empty()) tp_id = id;
                else return false;
                ++report_count;
                return true;
            });

        if (!orders_ok || !reports_ok || accepted_count != 2
            || report_count != 2 || sl_id.empty() || tp_id.empty())
            throw std::runtime_error(
                "Binance OCO placement returned ambiguous leg identities");

        handles.oco_list_id = list_id;
        handles.symbol = symbol;
        handles.sl_exchange_id = sl_id;
        handles.tp_exchange_id = tp_id;

        return handles;
    }

    std::vector<truetest::exits::IBracketAdapter::recovered_bracket>
    list_open() override
    {
        std::vector<truetest::exits::IBracketAdapter::recovered_bracket> out;
        if (!get_)
            throw std::runtime_error(
                "Binance OCO bracket recovery unavailable: missing GET transport");

        // GET /api/v3/openOrderList -> array of OCO list summaries; each
        // contains orderListId + listClientOrderId + orders[].
        // To recover SL/TP prices we need the per-leg orders, fetched via
        // GET /api/v3/openOrders (also array, includes price+stopPrice).
        auto lists_resp = get_("/api/v3/openOrderList", "");
        if (lists_resp.status < 200 || lists_resp.status >= 300)
        {
            throw std::runtime_error(
                "Binance OCO bracket recovery failed: openOrderList HTTP "
                + std::to_string(lists_resp.status));
        }
        if (!provider_recovery::is_authoritative_object_array(lists_resp.body))
            throw std::runtime_error(
                "Binance OCO bracket recovery failed: malformed openOrderList payload");
        if (!provider_recovery::every_top_level_object(
                lists_resp.body, [](std::string_view obj) {
                    std::string_view client;
                    return provider_recovery::top_level_plain_string(
                               obj, "listClientOrderId", client)
                        && !client.empty();
                }))
            throw std::runtime_error(
                "Binance OCO bracket recovery failed: list identity missing");

        auto orders_resp = get_("/api/v3/openOrders", "");
        if (orders_resp.status < 200 || orders_resp.status >= 300)
        {
            throw std::runtime_error(
                "Binance OCO bracket recovery failed: openOrders HTTP "
                + std::to_string(orders_resp.status));
        }
        if (!provider_recovery::is_authoritative_object_array(orders_resp.body))
            throw std::runtime_error(
                "Binance OCO bracket recovery failed: malformed openOrders payload");
        if (!provider_recovery::every_top_level_object(
                orders_resp.body, [](std::string_view obj) {
                    std::uint64_t id = 0;
                    return provider_recovery::top_level_positive_u64(
                        obj, "orderId", id);
                }))
            throw std::runtime_error(
                "Binance OCO bracket recovery failed: leg identity missing");

        // Walk every list object: only those whose listClientOrderId
        // starts with our prefix belong to TrueTest.
        const std::string prefix = "tt-oco-";
        std::unordered_set<std::uint64_t> recovered_openers;
        std::unordered_set<std::string> recovered_venue_ids;
        const bool lists_ok = provider_recovery::every_top_level_object(
            lists_resp.body, [&](std::string_view list_view) {
            std::string_view list_cli;
            if (!provider_recovery::top_level_plain_string(
                    list_view, "listClientOrderId", list_cli))
                return false;
            if (list_cli.size() <= prefix.size() ||
                list_cli.substr(0, prefix.size()) != prefix)
                return true;

            std::uint64_t opener = 0;
            if (!provider_recovery::parse_positive_u64(
                    std::string_view(list_cli).substr(prefix.size()), opener))
            {
                throw std::runtime_error(
                    "Binance OCO bracket recovery failed: invalid TrueTest list identity");
            }
            if (!recovered_openers.insert(opener).second)
                throw std::runtime_error(
                    "Binance OCO bracket recovery failed: duplicate TrueTest opener");

            std::uint64_t parsed_list_id = 0;
            std::string_view symbol_view;
            if (!provider_recovery::top_level_positive_u64(
                    list_view, "orderListId", parsed_list_id)
                || !provider_recovery::top_level_plain_string(
                    list_view, "symbol", symbol_view))
                throw std::runtime_error(
                    "Binance OCO bracket recovery failed: incomplete list identity");
            const std::string order_list_id = std::to_string(parsed_list_id);
            const std::string symbol(symbol_view);

            // Pull leg orderIds from list_obj.orders[]
            std::string_view orders_view;
            if (!provider_recovery::top_level_member(
                    list_view, "orders", orders_view))
                throw std::runtime_error(
                    "Binance OCO bracket recovery failed: orders missing");
            std::string orders_slice(orders_view);
            auto leg_ids = parse_leg_ids(orders_slice);
            if (symbol.empty() || leg_ids.size() != 2)
                throw std::runtime_error(
                    "Binance OCO bracket recovery failed: incomplete TrueTest list");

            // Find each leg in the global openOrders response.
            truetest::exits::IBracketAdapter::recovered_bracket rb;
            rb.opener_order_id = opener;
            rb.symbol          = symbol;
            rb.handles.symbol  = symbol;
            rb.handles.oco_list_id = order_list_id;

            std::unordered_set<std::string> unique_leg_ids;
            for (const auto& leg_id : leg_ids)
            {
                if (!unique_leg_ids.insert(leg_id).second)
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: duplicate referenced leg");
                if (!recovered_venue_ids.insert(leg_id).second)
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: venue leg reused across openers");
                auto leg = find_open_order(orders_resp.body, leg_id);
                if (leg.empty())
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: referenced leg missing");

                std::string_view type;
                std::string_view side;
                std::string_view qty;
                std::string_view px;
                std::string_view stop;
                if (!provider_recovery::top_level_plain_string(
                        leg, "type", type)
                    || !provider_recovery::top_level_plain_string(
                        leg, "side", side)
                    || !provider_recovery::top_level_scalar_text(
                        leg, "origQty", qty)
                    || !provider_recovery::top_level_scalar_text(
                        leg, "price", px)
                    || !provider_recovery::top_level_scalar_text(
                        leg, "stopPrice", stop))
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: incomplete referenced leg");
                if (side != "BUY" && side != "SELL")
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: invalid close side");

                double qty_value = 0.0;
                if (!binance::parse_double_sv(qty, qty_value)
                    || qty_value <= 0.0)
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: invalid quantity");
                if (rb.qty > 0.0 && rb.qty != qty_value)
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: inconsistent leg quantity");
                rb.qty = qty_value;
                const auto close_side = (side == "SELL")
                    ? order_side::sell : order_side::buy;
                if ((rb.stop_loss || rb.take_profit)
                    && rb.close_side != close_side)
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: inconsistent leg side");
                rb.close_side = close_side;

                if (type == "STOP_LOSS_LIMIT" || type == "STOP_LOSS")
                {
                    double stop_value = 0.0;
                    if (!binance::parse_double_sv(stop, stop_value)
                        || stop_value <= 0.0 || rb.stop_loss)
                        throw std::runtime_error(
                            "Binance OCO bracket recovery failed: invalid stop leg");
                    rb.stop_loss = stop_value;
                    rb.handles.sl_exchange_id = leg_id;
                }
                else if (type == "LIMIT_MAKER")
                {
                    double price_value = 0.0;
                    if (!binance::parse_double_sv(px, price_value)
                        || price_value <= 0.0 || rb.take_profit)
                        throw std::runtime_error(
                            "Binance OCO bracket recovery failed: invalid take-profit leg");
                    rb.take_profit = price_value;
                    rb.handles.tp_exchange_id = leg_id;
                }
                else
                    throw std::runtime_error(
                        "Binance OCO bracket recovery failed: unexpected leg type");
            }

            if (!rb.stop_loss || !rb.take_profit)
                throw std::runtime_error(
                    "Binance OCO bracket recovery failed: TrueTest OCO legs incomplete");

            // Approximate entry price as midpoint of SL/TP for trailing
            // bookkeeping. The portfolio holds the real cost basis from
            // the regular reconciler - this only feeds best_price.
            if (rb.stop_loss && rb.take_profit)
                rb.entry_price = (*rb.stop_loss + *rb.take_profit) * 0.5;
            else if (rb.stop_loss)   rb.entry_price = *rb.stop_loss;
            else if (rb.take_profit) rb.entry_price = *rb.take_profit;

            out.push_back(std::move(rb));
            return true;
        });
        if (!lists_ok)
            throw std::runtime_error(
                "Binance OCO bracket recovery failed: ambiguous list schema");
        return out;
    }

    void cancel(std::uint64_t opener_order_id,
                const truetest::exits::bracket_handles& handles) override
    {
        if (!del_ || handles.empty()) return;

        // DELETE /api/v3/orderList?symbol=&orderListId=  cancels both legs
        // atomically. If we lost the list id (rare), fall back to per-leg
        // DELETE /api/v3/order so we still drop the venue state.
        if (handles.oco_list_id)
        {
            std::string params;
            binance::append_param(params, "orderListId", *handles.oco_list_id);
            try
            {
                auto resp = del_("/api/v3/orderList", params);
                if (resp.status >= 200 && resp.status < 300)
                {
                    std::uint64_t returned_id = 0;
                    std::uint64_t expected_id = 0;
                    if (provider_recovery::parse_positive_u64(
                            *handles.oco_list_id, expected_id)
                        && provider_recovery::top_level_positive_u64(
                        resp.body, "orderListId", returned_id)
                        && returned_id == expected_id
                        && provider_recovery::top_level_exact_string(
                            resp.body, "listStatusType", "ALL_DONE")
                        && provider_recovery::top_level_exact_string(
                            resp.body, "listOrderStatus", "ALL_DONE"))
                        return;
                }
                std::cerr << "BinanceOcoBracketAdapter: cancel(orderList="
                          << *handles.oco_list_id << ") for opener="
                          << opener_order_id << " HTTP " << resp.status
                          << " - falling back to per-leg cancel\n";
            }
            catch (...)
            {
                std::cerr << "BinanceOcoBracketAdapter: cancel(orderList="
                          << *handles.oco_list_id << ") for opener="
                          << opener_order_id
                          << " threw - falling back to per-leg cancel\n";
            }
        }

        // If grouped cancellation was not authoritative, both known legs are
        // required to prove the OCO no longer rests at the venue.
        const bool complete_leg_identity = handles.sl_exchange_id.has_value()
            && handles.tp_exchange_id.has_value();
        bool cancel_failed = false;
        for (const auto* id : { &handles.sl_exchange_id, &handles.tp_exchange_id })
        {
            if (!*id) continue;
            std::string params;
            if (!handles.symbol.empty())
            {
                params.reserve(handles.symbol.size() + 32);
                binance::append_param(params, "symbol", handles.symbol);
            }
            binance::append_param(params, "orderId", **id);
            try
            {
                auto resp = del_("/api/v3/order", params);
                if (resp.status >= 200 && resp.status < 300)
                {
                    std::uint64_t returned_id = 0;
                    std::uint64_t expected_id = 0;
                    if (provider_recovery::parse_positive_u64(
                            **id, expected_id)
                        && provider_recovery::top_level_positive_u64(
                            resp.body, "orderId", returned_id)
                        && returned_id == expected_id
                        && provider_recovery::top_level_exact_string(
                            resp.body, "status", "CANCELED"))
                        continue;
                }
                else if (provider_recovery::has_exact_top_level_code(
                             resp.body, -2011)
                         || provider_recovery::has_exact_top_level_code(
                             resp.body, -2013))
                    continue;
                cancel_failed = true;
            }
            catch (...) { cancel_failed = true; }
        }
        if (cancel_failed || !complete_leg_identity)
            throw std::runtime_error(
                "Binance OCO per-leg cancel did not prove venue cancellation");
    }

private:
    request_fn     post_;
    request_fn     del_;
    request_get_fn get_;
    double stop_limit_offset_pct_;

    // Walk a JSON array of {orderId,clientOrderId,...} and return all orderIds.
    static std::vector<std::string> parse_leg_ids(std::string_view orders_arr)
    {
        std::vector<std::string> ids;
        if (!provider_recovery::is_authoritative_object_array(orders_arr))
            return ids;
        const bool parsed = provider_recovery::every_top_level_object(
            orders_arr, [&](std::string_view obj) {
            std::uint64_t parsed_id = 0;
            if (!provider_recovery::top_level_positive_u64(
                    obj, "orderId", parsed_id))
                return false;
            ids.emplace_back(std::to_string(parsed_id));
            return true;
        });
        if (!parsed) ids.clear();
        return ids;
    }

    // Linear scan through openOrders array - N is small in production
    // (hundreds at worst), so a hash index isn't worth the complexity.
    static std::string find_open_order(const std::string& orders_arr,
                                       const std::string& target_order_id)
    {
        std::string found;
        std::size_t matches = 0;
        const bool parsed = provider_recovery::every_top_level_object(
            orders_arr, [&](std::string_view obj) {
            std::uint64_t parsed_id = 0;
            if (!provider_recovery::top_level_positive_u64(
                    obj, "orderId", parsed_id))
                return false;
            if (std::to_string(parsed_id) == target_order_id)
            {
                ++matches;
                if (matches == 1) found.assign(obj);
            }
            return true;
        });
        return parsed && matches == 1 ? found : std::string{};
    }

    static std::string upper(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(::toupper(c));
        return s;
    }

    // Binance OCO STOP_LOSS_LIMIT leg needs a stopPrice (trigger) and
    // stopLimitPrice (limit). For longs the limit must be ≤ stopPrice
    // (we want to sell ≤ trigger), for shorts ≥. Offset gives the limit
    // a small buffer past the trigger so it actually fills on a fast move.
    double stop_limit_price_for(order_side close_side, double stop_px) const
    {
        if (close_side == order_side::sell)
            return stop_px * (1.0 - stop_limit_offset_pct_);
        return stop_px * (1.0 + stop_limit_offset_pct_);
    }

    static std::string fmt_double(double v)
    {
        // Binance accepts plain decimal strings; it rejects scientific
        // notation. 8 dp is enough for spot tick sizes and quantities.
        char tmp[48];
        std::snprintf(tmp, sizeof(tmp), "%.8f", v);
        // trim trailing zeros + dangling dot
        std::string s(tmp);
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.')   s.pop_back();
        }
        return s;
    }

};

inline std::shared_ptr<BinanceOcoBracketAdapter>
make_binance_oco_bracket_adapter(
    std::shared_ptr<BinanceRestClient> client,
    std::shared_ptr<std::atomic<bool>> cancelled =
        std::make_shared<std::atomic<bool>>(false))
{
    constexpr auto deadline = std::chrono::milliseconds{1500};
    auto post = [client, cancelled](std::string_view ep, std::string_view p)
        -> BinanceOcoBracketAdapter::response
    {
        if (!client->ensure_clock_fresh_for_order(std::chrono::milliseconds{500}))
            throw std::runtime_error("clock refresh failed before OCO placement");
        auto r = client->safety_post(
            std::string(ep), std::string(p), deadline, cancelled.get());
        if (r.request_written && (r.status == 0 || r.status >= 500))
            throw std::runtime_error("ambiguous post-write OCO placement");
        return {r.status, r.body};
    };
    auto del = [client, cancelled](std::string_view ep, std::string_view p)
        -> BinanceOcoBracketAdapter::response
    {
        if (!client->ensure_clock_fresh_for_order(std::chrono::milliseconds{500}))
            throw std::runtime_error("clock refresh failed before OCO cancellation");
        auto r = client->safety_del(
            std::string(ep), std::string(p), deadline, cancelled.get());
        if (r.request_written && (r.status == 0 || r.status >= 500))
            throw std::runtime_error("ambiguous post-write OCO cancellation");
        return {r.status, r.body};
    };
    auto get = [client, cancelled](std::string_view ep, std::string_view p)
        -> BinanceOcoBracketAdapter::response
    {
        if (!client->ensure_clock_fresh_for_order(std::chrono::milliseconds{500}))
            throw std::runtime_error("clock refresh failed before OCO query");
        auto r = client->safety_get(
            std::string(ep), std::string(p), deadline, cancelled.get());
        return {r.status, r.body};
    };
    return std::make_shared<BinanceOcoBracketAdapter>(
        std::move(post), std::move(del), std::move(get));
}

#endif // HAS_BINANCE
