#pragma once
#ifdef HAS_BINANCE

#include "exits/bracket_adapter.h"
#include "exits/exit_intent.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_rest_client.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
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
        auto list_id = binance::extract_number(resp.body, "orderListId");
        if (list_id.empty())
            list_id = binance::extract_string(resp.body, "listClientOrderId");
        if (!list_id.empty()) handles.oco_list_id = list_id;
        handles.symbol = symbol;

        // Pull the two leg orderIds out of orderReports so we can map
        // inbound fills back to opener_order_id without depending on
        // listClientOrderId arriving on every exec report.
        auto reports = slice(resp.body, "orderReports");
        auto sl_id = leg_order_id(reports, /*want_stop=*/true);
        auto tp_id = leg_order_id(reports, /*want_stop=*/false);
        if (!sl_id.empty()) handles.sl_exchange_id = sl_id;
        if (!tp_id.empty()) handles.tp_exchange_id = tp_id;

        return handles;
    }

    std::vector<truetest::exits::IBracketAdapter::recovered_bracket>
    list_open() override
    {
        std::vector<truetest::exits::IBracketAdapter::recovered_bracket> out;
        if (!get_) return out;

        // GET /api/v3/openOrderList -> array of OCO list summaries; each
        // contains orderListId + listClientOrderId + orders[].
        // To recover SL/TP prices we need the per-leg orders, fetched via
        // GET /api/v3/openOrders (also array, includes price+stopPrice).
        auto lists_resp = get_("/api/v3/openOrderList", "");
        if (lists_resp.status < 200 || lists_resp.status >= 300)
        {
            std::cerr << "BinanceOcoBracketAdapter: openOrderList HTTP "
                      << lists_resp.status << " - restart recovery skipped\n";
            return out;
        }

        auto orders_resp = get_("/api/v3/openOrders", "");
        if (orders_resp.status < 200 || orders_resp.status >= 300)
        {
            std::cerr << "BinanceOcoBracketAdapter: openOrders HTTP "
                      << orders_resp.status << " - restart recovery skipped\n";
            return out;
        }

        // Walk every list object: only those whose listClientOrderId
        // starts with our prefix belong to TrueTest.
        const std::string prefix = "tt-oco-";
        const std::string& lists = lists_resp.body;
        std::size_t i = 0;
        while (i < lists.size())
        {
            auto open = lists.find('{', i);
            if (open == std::string::npos) break;
            int depth = 0;
            std::size_t j = open;
            for (; j < lists.size(); ++j)
            {
                if (lists[j] == '{') ++depth;
                else if (lists[j] == '}') { --depth; if (depth == 0) { ++j; break; } }
            }
            std::string list_obj(lists, open, j - open);
            i = j;

            auto list_cli = binance::extract_string(list_obj, "listClientOrderId");
            if (list_cli.size() <= prefix.size() ||
                list_cli.compare(0, prefix.size(), prefix) != 0)
                continue;

            std::uint64_t opener = 0;
            try { opener = std::stoull(list_cli.substr(prefix.size())); }
            catch (...) { continue; }

            auto order_list_id = binance::extract_number(list_obj, "orderListId");
            auto symbol        = binance::extract_string(list_obj, "symbol");

            // Pull leg orderIds from list_obj.orders[]
            auto orders_slice = slice(list_obj, "orders");
            auto leg_ids = parse_leg_ids(orders_slice);
            if (leg_ids.size() < 2) continue;

            // Find each leg in the global openOrders response.
            truetest::exits::IBracketAdapter::recovered_bracket rb;
            rb.opener_order_id = opener;
            rb.symbol          = symbol;
            rb.handles.symbol  = symbol;
            rb.handles.oco_list_id = order_list_id.empty() ? list_cli : order_list_id;

            for (const auto& leg_id : leg_ids)
            {
                auto leg = find_open_order(orders_resp.body, leg_id);
                if (leg.empty()) continue;

                auto type = binance::extract_string(leg, "type");
                auto side = binance::extract_string(leg, "side");
                auto qty  = binance::extract_string(leg, "origQty");
                auto px   = binance::extract_string(leg, "price");
                auto stop = binance::extract_string(leg, "stopPrice");

                rb.qty = std::strtod(qty.c_str(), nullptr);
                rb.close_side = (side == "SELL") ? order_side::sell : order_side::buy;

                if (type.find("STOP") != std::string::npos)
                {
                    rb.stop_loss = std::strtod(stop.c_str(), nullptr);
                    rb.handles.sl_exchange_id = leg_id;
                }
                else
                {
                    rb.take_profit = std::strtod(px.c_str(), nullptr);
                    rb.handles.tp_exchange_id = leg_id;
                }
            }

            // Approximate entry price as midpoint of SL/TP for trailing
            // bookkeeping. The portfolio holds the real cost basis from
            // the regular reconciler - this only feeds best_price.
            if (rb.stop_loss && rb.take_profit)
                rb.entry_price = (*rb.stop_loss + *rb.take_profit) * 0.5;
            else if (rb.stop_loss)   rb.entry_price = *rb.stop_loss;
            else if (rb.take_profit) rb.entry_price = *rb.take_profit;

            out.push_back(std::move(rb));
        }
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
            auto resp = del_("/api/v3/orderList", params);
            if (resp.status >= 200 && resp.status < 300) return;
            std::cerr << "BinanceOcoBracketAdapter: cancel(orderList="
                      << *handles.oco_list_id << ") for opener="
                      << opener_order_id << " HTTP " << resp.status
                      << " - falling back to per-leg cancel\n";
        }

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
            auto resp = del_("/api/v3/order", params);
            if (resp.status >= 400)
                std::cerr << "BinanceOcoBracketAdapter: per-leg cancel(orderId="
                          << **id << ") for opener=" << opener_order_id
                          << " HTTP " << resp.status << "\n";
        }
    }

private:
    request_fn     post_;
    request_fn     del_;
    request_get_fn get_;
    double stop_limit_offset_pct_;

    // Walk a JSON array of {orderId,clientOrderId,...} and return all orderIds.
    static std::vector<std::string> parse_leg_ids(const std::string& orders_arr)
    {
        std::vector<std::string> ids;
        if (orders_arr.empty()) return ids;
        std::size_t i = 0;
        while (i < orders_arr.size())
        {
            auto open = orders_arr.find('{', i);
            if (open == std::string::npos) break;
            int depth = 0;
            std::size_t j = open;
            for (; j < orders_arr.size(); ++j)
            {
                if (orders_arr[j] == '{') ++depth;
                else if (orders_arr[j] == '}') { --depth; if (depth == 0) { ++j; break; } }
            }
            std::string obj(orders_arr, open, j - open);
            auto id = binance::extract_number(obj, "orderId");
            if (id.empty()) id = binance::extract_string(obj, "orderId");
            if (!id.empty()) ids.push_back(std::move(id));
            i = j;
        }
        return ids;
    }

    // Linear scan through openOrders array - N is small in production
    // (hundreds at worst), so a hash index isn't worth the complexity.
    static std::string find_open_order(const std::string& orders_arr,
                                       const std::string& target_order_id)
    {
        std::size_t i = 0;
        while (i < orders_arr.size())
        {
            auto open = orders_arr.find('{', i);
            if (open == std::string::npos) break;
            int depth = 0;
            std::size_t j = open;
            for (; j < orders_arr.size(); ++j)
            {
                if (orders_arr[j] == '{') ++depth;
                else if (orders_arr[j] == '}') { --depth; if (depth == 0) { ++j; break; } }
            }
            std::string obj(orders_arr, open, j - open);
            auto id = binance::extract_number(obj, "orderId");
            if (id.empty()) id = binance::extract_string(obj, "orderId");
            if (id == target_order_id) return obj;
            i = j;
        }
        return {};
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

    // Cheap substring slice - returns the body of `key`'s array as a
    // std::string (or "" if absent). We use the engine's hand-rolled
    // string extractors elsewhere; this keeps us off nlohmann/json.
    static std::string slice(const std::string& json, const std::string& key)
    {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return {};
        pos = json.find('[', pos);
        if (pos == std::string::npos) return {};
        int depth = 0;
        std::string out;
        for (auto i = pos; i < json.size(); ++i)
        {
            const char c = json[i];
            if (c == '[') ++depth;
            else if (c == ']') { --depth; if (depth == 0) { out.assign(json, pos, i - pos + 1); break; } }
        }
        return out;
    }

    // Walk the orderReports array. The SL leg's "type" contains "STOP";
    // the TP leg is "LIMIT_MAKER" (no STOP). Returns the orderId of the
    // matching leg as a string.
    static std::string leg_order_id(const std::string& reports, bool want_stop)
    {
        if (reports.empty()) return {};
        std::size_t i = 0;
        while (i < reports.size())
        {
            auto open = reports.find('{', i);
            if (open == std::string::npos) break;
            int depth = 0;
            std::size_t j = open;
            for (; j < reports.size(); ++j)
            {
                if (reports[j] == '{') ++depth;
                else if (reports[j] == '}') { --depth; if (depth == 0) { ++j; break; } }
            }
            std::string obj(reports, open, j - open);
            auto type = binance::extract_string(obj, "type");
            const bool is_stop = type.find("STOP") != std::string::npos;
            if (is_stop == want_stop)
            {
                auto id = binance::extract_number(obj, "orderId");
                if (id.empty()) id = binance::extract_string(obj, "orderId");
                return id;
            }
            i = j;
        }
        return {};
    }
};

inline std::shared_ptr<BinanceOcoBracketAdapter>
make_binance_oco_bracket_adapter(std::shared_ptr<BinanceRestClient> client)
{
    auto post = [client](std::string_view ep, std::string_view p)
        -> BinanceOcoBracketAdapter::response
    {
        auto r = client->post(std::string(ep), std::string(p));
        return {r.status, r.body};
    };
    auto del = [client](std::string_view ep, std::string_view p)
        -> BinanceOcoBracketAdapter::response
    {
        auto r = client->del(std::string(ep), std::string(p));
        return {r.status, r.body};
    };
    auto get = [client](std::string_view ep, std::string_view p)
        -> BinanceOcoBracketAdapter::response
    {
        auto r = client->get(std::string(ep), std::string(p));
        return {r.status, r.body};
    };
    return std::make_shared<BinanceOcoBracketAdapter>(
        std::move(post), std::move(del), std::move(get));
}

#endif // HAS_BINANCE
