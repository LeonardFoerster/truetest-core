#pragma once
#ifdef HAS_BINANCE

#include "../../execution/order_encoder.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

class BinanceOrderEncoder : public IOrderEncoder
{
public:
    explicit BinanceOrderEncoder(std::string default_symbol = "")
        : default_symbol_(std::move(default_symbol))
    {}

    void set_default_symbol(std::string sym) { default_symbol_ = std::move(sym); }

    encoded_order encode_submit(const order_event& o,
                                std::string_view client_order_id) override
    {
        const std::string sym = resolve_symbol(o.get_symbol());
        const char* type_str = order_type_to_binance(o.get_order_type());
        const char* side_str = (o.get_side() == order_side::buy) ? "BUY" : "SELL";

        std::string params = "symbol=" + sym
            + "&side=" + side_str
            + "&type=" + type_str
            + "&quantity=" + format_decimal(o.get_quantity());

        if (o.get_order_type() != order_type::market)
        {
            params += "&price=" + format_decimal(o.get_price());
            params += "&timeInForce=" + std::string(tif_to_binance(o.get_tif()));
        }

        if (o.get_order_type() == order_type::stop ||
            o.get_order_type() == order_type::stop_limit)
        {
            params += "&stopPrice=" + format_decimal(o.get_stop_price());
        }

        if (!client_order_id.empty())
        {
            params += "&newClientOrderId=" + std::string(client_order_id);
        }

        encoded_order e;
        e.endpoint        = "/api/v3/order";
        e.wire_payload    = std::move(params);
        e.client_order_id = std::string(client_order_id);
        return e;
    }

    encoded_order encode_cancel(std::string_view symbol,
                                std::string_view exchange_order_id,
                                std::string_view client_order_id) override
    {
        std::string sym = symbol.empty() ? default_symbol_ : std::string(symbol);
        std::string sym_upper = upper(std::move(sym));

        std::string params = "symbol=" + sym_upper;
        if (!exchange_order_id.empty())
        {
            params += "&orderId=" + std::string(exchange_order_id);
        }
        else if (!client_order_id.empty())
        {
            params += "&origClientOrderId=" + std::string(client_order_id);
        }

        encoded_order e;
        e.endpoint        = "/api/v3/order";
        e.wire_payload    = std::move(params);
        e.client_order_id = std::string(client_order_id);
        return e;
    }

private:
    std::string default_symbol_;

    std::string resolve_symbol(const std::string& event_sym) const
    {
        return upper(event_sym.empty() ? default_symbol_ : event_sym);
    }

    static std::string upper(std::string s)
    {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    static const char* order_type_to_binance(order_type t)
    {
        switch (t)
        {
        case order_type::market:     return "MARKET";
        case order_type::limit:      return "LIMIT";
        case order_type::stop:       return "STOP_LOSS_LIMIT";
        case order_type::stop_limit: return "STOP_LOSS_LIMIT";
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

    static std::string format_decimal(double v)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.8f", v);
        return std::string(buf);
    }
};

#endif // HAS_BINANCE
