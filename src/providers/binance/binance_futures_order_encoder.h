#pragma once
#ifdef HAS_BINANCE

#include "../../execution/order_encoder.h"
#include "providers/binance/binance_auth.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

// USDT-M futures (`/fapi/v1/order`). Same prefix-cache strategy as the
// spot encoder; differences from spot:
//   - stops are STOP_MARKET (price-less, triggers market on stopPrice) /
//     STOP (limit-on-trigger), not STOP_LOSS_LIMIT;
//   - STOP_MARKET carries no `timeInForce` and no `price`;
//   - one-way mode is assumed (no positionSide param emitted; futures
//     defaults to BOTH). Hedge mode is gated out at provider open() in a
//     later step, so encoding never has to branch on it.
class BinanceFuturesOrderEncoder : public IOrderEncoder
{
public:
    explicit BinanceFuturesOrderEncoder(std::string default_symbol = "")
        : default_symbol_(upper(std::move(default_symbol)))
    {}

    void set_default_symbol(std::string sym)
    {
        default_symbol_ = upper(std::move(sym));
        for (auto& s : prefix_cache_) s.clear();
    }

    encoded_order encode_submit(const order_event& o,
                                std::string_view client_order_id) override
    {
        const std::string sym = upper(
            o.get_symbol().empty() ? default_symbol_ : o.get_symbol());
        const order_side  side = o.get_side();
        const order_type  type = o.get_order_type();
        const time_in_force tif = o.get_tif();

        const std::string& prefix = (sym == default_symbol_)
            ? cached_prefix(side, type, tif)
            : prefix_for(sym, side, type, tif);

        std::string payload;
        payload.reserve(prefix.size() + 96 + client_order_id.size());
        payload.assign(prefix);

        char numbuf[64];
        int n = std::snprintf(numbuf, sizeof(numbuf), "%.8f", o.get_quantity());
        payload.append("&quantity=", 10);
        payload.append(numbuf, static_cast<std::size_t>(n));

        // STOP_MARKET / MARKET have no `price`; STOP / LIMIT do.
        if (takes_price(type))
        {
            n = std::snprintf(numbuf, sizeof(numbuf), "%.8f", o.get_price());
            payload.append("&price=", 7);
            payload.append(numbuf, static_cast<std::size_t>(n));
        }

        if (takes_stop_price(type))
        {
            n = std::snprintf(numbuf, sizeof(numbuf), "%.8f", o.get_stop_price());
            payload.append("&stopPrice=", 11);
            payload.append(numbuf, static_cast<std::size_t>(n));
        }

        if (!client_order_id.empty())
        {
            binance::append_param(payload, "newClientOrderId", client_order_id);
        }

        encoded_order e;
        e.endpoint        = "/fapi/v1/order";
        e.wire_payload    = std::move(payload);
        e.client_order_id = std::string(client_order_id);
        return e;
    }

    encoded_order encode_cancel(std::string_view symbol,
                                std::string_view exchange_order_id,
                                std::string_view client_order_id) override
    {
        std::string sym = symbol.empty() ? default_symbol_ : std::string(symbol);
        std::string sym_upper = upper(std::move(sym));

        std::string params;
        params.reserve(sym_upper.size() + 64 + client_order_id.size()
                       + exchange_order_id.size());
        binance::append_param(params, "symbol", sym_upper);
        if (!exchange_order_id.empty())
        {
            binance::append_param(params, "orderId", exchange_order_id);
        }
        else if (!client_order_id.empty())
        {
            binance::append_param(params, "origClientOrderId", client_order_id);
        }

        encoded_order e;
        e.endpoint        = "/fapi/v1/order";
        e.wire_payload    = std::move(params);
        e.client_order_id = std::string(client_order_id);
        return e;
    }

private:
    static constexpr std::size_t kPrefixCacheSize = 2 * 4 * 4;
    std::array<std::string, kPrefixCacheSize> prefix_cache_{};
    std::string default_symbol_;

    static std::size_t cache_index(order_side s, order_type t, time_in_force tif)
    {
        return static_cast<std::size_t>(s) * 16
             + static_cast<std::size_t>(t) * 4
             + static_cast<std::size_t>(tif);
    }

    const std::string& cached_prefix(order_side s, order_type t,
                                     time_in_force tif)
    {
        const std::size_t idx = cache_index(s, t, tif);
        if (prefix_cache_[idx].empty())
            prefix_cache_[idx] = prefix_for(default_symbol_, s, t, tif);
        return prefix_cache_[idx];
    }

    static std::string prefix_for(const std::string& sym,
                                  order_side s, order_type t,
                                  time_in_force tif)
    {
        std::string out;
        out.reserve(sym.size() + 56);
        binance::append_param(out, "symbol", sym);
        binance::append_param(out, "side", side_to_binance(s));
        binance::append_param(out, "type", order_type_to_binance(t));
        if (takes_tif(t))
        {
            binance::append_param(out, "timeInForce", tif_to_binance(tif));
        }
        return out;
    }

    static bool takes_price(order_type t)
    {
        return t == order_type::limit || t == order_type::stop_limit;
    }

    static bool takes_stop_price(order_type t)
    {
        return t == order_type::stop || t == order_type::stop_limit;
    }

    static bool takes_tif(order_type t)
    {
        // STOP_MARKET (futures `stop`) is a triggered MARKET - no TIF.
        return t == order_type::limit || t == order_type::stop_limit;
    }

    static std::string upper(std::string s)
    {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    static const char* side_to_binance(order_side s)
    {
        return s == order_side::buy ? "BUY" : "SELL";
    }

    static const char* order_type_to_binance(order_type t)
    {
        switch (t)
        {
        case order_type::market:     return "MARKET";
        case order_type::limit:      return "LIMIT";
        case order_type::stop:       return "STOP_MARKET";
        case order_type::stop_limit: return "STOP";
        }
        return "LIMIT";
    }

    static const char* tif_to_binance(time_in_force t)
    {
        switch (t)
        {
        case time_in_force::ioc: return "IOC";
        case time_in_force::fok: return "FOK";
        case time_in_force::gtc: return "GTC";
        case time_in_force::day: return "GTC";
        }
        return "GTC";
    }
};

#endif // HAS_BINANCE
