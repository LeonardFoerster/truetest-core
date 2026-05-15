#pragma once
#ifdef HAS_BINANCE

#include "exits/bracket_adapter.h"
#include "exits/exit_intent.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_rest_client.h"

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

// USDT-M futures bracket adapter. Places SL+TP as two SEPARATE conditional
// orders (STOP_MARKET + TAKE_PROFIT_MARKET), both with closePosition=true
// and reduceOnly=true. Binance has no /fapi/v1/order/oco endpoint, so
// placement is NOT atomic — if the second POST fails after the first
// succeeds, we have a hanging leg that cancel() must clean up.
// Cancel-other-when-fires is delivered by Binance's `closePosition=true`
// semantics: when a closePosition order triggers and brings the position
// to zero, the venue automatically cancels every other closePosition
// order on that symbol. So once both legs are placed, fill-of-one →
// cancellation-of-other is exchange-side, exactly like spot OCO.
// Constraint: closePosition=true requires the order to close the entire
// position. Partial brackets (qty_fraction < 1.0 for TP1/TP2 scale-outs)
// cannot use closePosition=true without splitting into a different
// shape; this adapter declines them and the engine-side ExitManager
// remains the only enforcer for that intent. Single full-position
// SL+TP brackets are the supported case — same scope as spot OCO.
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
                                 request_fn get = nullptr)
        : post_(std::move(post))
        , del_(std::move(del))
        , get_(std::move(get))
    {}

    truetest::exits::bracket_caps capabilities() const override
    {
        truetest::exits::bracket_caps c;
        c.stop_market = true;
        // oco intentionally false: placement is two separate POSTs, not
        // atomic. The cancel-other-when-fires guarantee from
        // closePosition=true does not require advertising oco capability,
        // which the engine reads as "atomic placement available".
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
                         "(opener=" << opener_order_id << ") — declining; "
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

        auto tp_id = place_leg("TAKE_PROFIT_MARKET", symbol, side,
                               *intent.take_profit, tp_cli, opener_order_id);
        if (tp_id.empty())
        {
            // Hanging SL after TP failure: log loudly, return what we have.
            // The engine will see SL-only handles and rely on its own TP.
            // cancel() on teardown will sweep the SL.
            std::cerr << "BinanceFuturesBracketAdapter: TP leg failed for opener="
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
        if (!del_ || handles.empty()) return;

        // Per-leg DELETE. We don't try the cancel-orderList endpoint
        // because there's no list — placement is two independent orders.
        // -2011 / -2013 ("unknown order" / "order does not exist") arrive
        // when the venue already closed the leg via closePosition=true
        // auto-cancel; treat as success.
        struct leg { const std::optional<std::string>* id; const char* tag; };
        const leg legs[] = {
            {&handles.sl_exchange_id, "SL"},
            {&handles.tp_exchange_id, "TP"},
        };
        for (const auto& l : legs)
        {
            if (!*l.id) continue;
            // /fapi/v1/order DELETE requires symbol. handles.symbol is
            // populated by place() and list_open(); empty here means
            // the caller constructed the handles themselves without
            // the field set, in which case we fall through to a
            // symbol-less request and let the venue surface 4xx.
            std::string params;
            if (!handles.symbol.empty())
            {
                params.reserve(handles.symbol.size() + 32);
                params.append("symbol=", 7);
                params.append(handles.symbol);
                params.append("&orderId=", 9);
            }
            else
            {
                params.append("orderId=", 8);
            }
            params.append(**l.id);
            auto resp = del_("/fapi/v1/order", params);
            if (resp.status >= 400 &&
                resp.body.find("-2011") == std::string::npos &&
                resp.body.find("-2013") == std::string::npos)
            {
                std::cerr << "BinanceFuturesBracketAdapter: cancel "
                          << l.tag << " (orderId=" << **l.id
                          << ") for opener=" << opener_order_id
                          << " HTTP " << resp.status << " body="
                          << resp.body << "\n";
            }
        }
    }

    std::vector<truetest::exits::IBracketAdapter::recovered_bracket>
    list_open() override
    {
        std::vector<truetest::exits::IBracketAdapter::recovered_bracket> out;
        if (!get_) return out;

        // Futures /fapi/v1/openOrders returns a flat array of orders;
        // conditional orders (STOP_MARKET, TAKE_PROFIT_MARKET) are
        // included alongside regular ones. Filter by the tt-fb- prefix.
        auto resp = get_("/fapi/v1/openOrders", "");
        if (resp.status < 200 || resp.status >= 300)
        {
            std::cerr << "BinanceFuturesBracketAdapter: openOrders HTTP "
                      << resp.status << " — restart recovery skipped\n";
            return out;
        }

        // Walk the array, parse one object at a time, group SL/TP per
        // opener_id we extract from the clientOrderId suffix.
        struct partial
        {
            truetest::exits::IBracketAdapter::recovered_bracket rb;
            bool sl_seen = false;
            bool tp_seen = false;
        };
        std::unordered_map<std::uint64_t, partial> by_opener;

        const std::string& body = resp.body;
        std::size_t i = 0;
        const std::string sl_pref = "tt-fb-sl-";
        const std::string tp_pref = "tt-fb-tp-";
        while (i < body.size())
        {
            auto open = body.find('{', i);
            if (open == std::string::npos) break;
            int depth = 0;
            std::size_t j = open;
            for (; j < body.size(); ++j)
            {
                if (body[j] == '{') ++depth;
                else if (body[j] == '}')
                {
                    --depth;
                    if (depth == 0) { ++j; break; }
                }
            }
            std::string obj(body, open, j - open);
            i = j;

            auto cli = binance::extract_string(obj, "clientOrderId");
            bool is_sl = cli.size() > sl_pref.size() &&
                         cli.compare(0, sl_pref.size(), sl_pref) == 0;
            bool is_tp = cli.size() > tp_pref.size() &&
                         cli.compare(0, tp_pref.size(), tp_pref) == 0;
            if (!is_sl && !is_tp) continue;

            std::uint64_t opener = 0;
            try
            {
                const auto& pref = is_sl ? sl_pref : tp_pref;
                opener = std::stoull(cli.substr(pref.size()));
            }
            catch (...) { continue; }

            auto& p = by_opener[opener];
            auto& rb = p.rb;

            auto symbol = binance::extract_string(obj, "symbol");
            auto side   = binance::extract_string(obj, "side");
            auto stop   = binance::extract_string(obj, "stopPrice");
            auto qty    = binance::extract_string(obj, "origQty");
            auto id     = binance::extract_number(obj, "orderId");
            if (id.empty()) id = binance::extract_string(obj, "orderId");

            rb.opener_order_id = opener;
            rb.symbol = symbol;
            rb.handles.symbol = symbol;
            rb.close_side = (side == "SELL") ? order_side::sell : order_side::buy;
            // closePosition=true emits qty=0 in openOrders; only believe
            // it if non-zero to avoid clobbering a legitimate value from
            // the other leg.
            double qty_d = std::strtod(qty.c_str(), nullptr);
            if (qty_d > 0.0) rb.qty = qty_d;
            double stop_d = std::strtod(stop.c_str(), nullptr);

            if (is_sl)
            {
                rb.stop_loss = stop_d;
                if (!id.empty()) rb.handles.sl_exchange_id = id;
                p.sl_seen = true;
            }
            else
            {
                rb.take_profit = stop_d;
                if (!id.empty()) rb.handles.tp_exchange_id = id;
                p.tp_seen = true;
            }
        }

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

    // Returns the orderId on success, "" on failure (caller treats as decline).
    std::string place_leg(const char* type,
                          const std::string& symbol,
                          const std::string& side,
                          double trigger_price,
                          const std::string& client_id,
                          std::uint64_t opener_order_id)
    {
        char buf[512];
        // closePosition=true means quantity is omitted (Binance derives
        // it from current position size at trigger time). reduceOnly is
        // implied by closePosition but keeping it explicit makes the
        // intent self-documenting and survives if Binance ever loosens
        // the implication.
        std::snprintf(buf, sizeof(buf),
            "symbol=%s"
            "&side=%s"
            "&type=%s"
            "&stopPrice=%s"
            "&closePosition=true"
            "&reduceOnly=true"
            "&newClientOrderId=%s",
            symbol.c_str(),
            side.c_str(),
            type,
            fmt_double(trigger_price).c_str(),
            client_id.c_str());

        auto resp = post_("/fapi/v1/order", buf);
        if (resp.status < 200 || resp.status >= 300)
        {
            std::cerr << "BinanceFuturesBracketAdapter: " << type
                      << " place failed for opener=" << opener_order_id
                      << " HTTP " << resp.status << " body=" << resp.body
                      << "\n";
            return {};
        }

        auto id = binance::extract_number(resp.body, "orderId");
        if (id.empty()) id = binance::extract_string(resp.body, "orderId");
        return id;
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
make_binance_futures_bracket_adapter(std::shared_ptr<BinanceRestClient> client)
{
    auto post = [client](std::string_view ep, std::string_view p)
        -> BinanceFuturesBracketAdapter::response
    {
        auto r = client->post(std::string(ep), std::string(p));
        return {r.status, r.body};
    };
    auto del = [client](std::string_view ep, std::string_view p)
        -> BinanceFuturesBracketAdapter::response
    {
        auto r = client->del(std::string(ep), std::string(p));
        return {r.status, r.body};
    };
    auto get = [client](std::string_view ep, std::string_view p)
        -> BinanceFuturesBracketAdapter::response
    {
        auto r = client->get(std::string(ep), std::string(p));
        return {r.status, r.body};
    };
    return std::make_shared<BinanceFuturesBracketAdapter>(
        std::move(post), std::move(del), std::move(get));
}

#endif // HAS_BINANCE
