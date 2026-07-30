#pragma once
#ifdef HAS_BYBIT

#include "execution/order_encoder.h"
#include "providers/bybit/bybit_endpoints.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

// Bybit V5 linear futures place/cancel encoder (`/v5/order/*`).
// Wire payload is raw JSON with a stable key order (manual string build —
// no nlohmann). Stop/conditional brackets go through
// BybitFuturesBracketAdapter (not this encoder).
//
// Quirks vs Binance futures:
//   - JSON body (not query-string)
//   - side Buy/Sell, orderType Market/Limit, TIF GTC/IOC/FOK
//   - positionIdx always 0 (one-way); hedge refused at open()
//   - orderLinkId max 36 chars
//   - cancel is POST /v5/order/cancel (not DELETE)
class BybitFuturesOrderEncoder : public IOrderEncoder
{
public:
    explicit BybitFuturesOrderEncoder(std::string default_symbol = "",
                                      std::string category = "linear")
        : default_symbol_(upper(std::move(default_symbol)))
        , category_(std::move(category))
    {}

    void set_default_symbol(std::string sym)
    {
        default_symbol_ = upper(std::move(sym));
    }

    void set_category(std::string category)
    {
        category_ = std::move(category);
    }

    // Default reduceOnly for place-order (false unless kill/close path).
    void set_reduce_only(bool v) { reduce_only_ = v; }
    bool reduce_only() const { return reduce_only_; }

    // Bybit orderLinkId: 1..36 printable chars (we allow alnum + ._-:/).
    static bool valid_order_link_id(std::string_view id)
    {
        if (id.empty() || id.size() > 36) return false;
        for (unsigned char c : id)
        {
            if (std::isalnum(c)) continue;
            if (c == '.' || c == '-' || c == '_' || c == ':' || c == '/')
                continue;
            return false;
        }
        return true;
    }

    encoded_order encode_submit(const order_event& o,
                                std::string_view client_order_id) override
    {
        encoded_order e;
        e.client_order_id = std::string(client_order_id);

        const order_type type = o.get_order_type();
        // Conditional brackets use BybitFuturesBracketAdapter (triggerPrice
        // Market legs). This encoder stays market/limit only.
        if (type == order_type::stop || type == order_type::stop_limit)
            return e;

        if (!client_order_id.empty() && !valid_order_link_id(client_order_id))
            return e;

        const std::string sym = upper(
            o.get_symbol().empty() ? default_symbol_ : o.get_symbol());
        if (sym.empty() || category_.empty())
            return e;

        const bool is_limit = (type == order_type::limit);
        if (!is_limit && type != order_type::market)
            return e;

        std::string body;
        body.reserve(220 + client_order_id.size() + sym.size());
        body.push_back('{');
        append_kv_str(body, "category", category_, /*first=*/true);
        append_kv_str(body, "symbol", sym);
        append_kv_str(body, "side", side_to_bybit(o.get_side()));
        append_kv_str(body, "orderType", is_limit ? "Limit" : "Market");
        append_kv_str(body, "qty", format_decimal(o.get_quantity()));

        if (is_limit)
        {
            append_kv_str(body, "price", format_decimal(o.get_price()));
            append_kv_str(body, "timeInForce", tif_to_bybit(o.get_tif()));
        }

        // One-way only (Phase 4 refuses hedge at open).
        append_kv_num(body, "positionIdx", 0);

        if (!client_order_id.empty())
            append_kv_str(body, "orderLinkId", client_order_id);

        if (reduce_only_)
            append_kv_bool(body, "reduceOnly", true);

        body.push_back('}');

        e.endpoint     = bybit::paths::order_create;
        e.wire_payload = std::move(body);
        return e;
    }

    encoded_order encode_cancel(std::string_view symbol,
                                std::string_view exchange_order_id,
                                std::string_view client_order_id) override
    {
        encoded_order e;
        e.client_order_id = std::string(client_order_id);

        // Prefer orderId; fall back to orderLinkId. Need at least one.
        const bool has_ex = !exchange_order_id.empty();
        const bool has_cli = !client_order_id.empty();
        if (!has_ex && !has_cli)
            return e;
        if (has_cli && !has_ex && !valid_order_link_id(client_order_id))
            return e;
        if (category_.empty())
            return e;

        const std::string sym = upper(
            symbol.empty() ? default_symbol_ : std::string(symbol));
        if (sym.empty())
            return e;

        std::string body;
        body.reserve(96 + exchange_order_id.size() + client_order_id.size()
                     + sym.size());
        body.push_back('{');
        append_kv_str(body, "category", category_, /*first=*/true);
        append_kv_str(body, "symbol", sym);
        if (has_ex)
            append_kv_str(body, "orderId", exchange_order_id);
        else
            append_kv_str(body, "orderLinkId", client_order_id);
        body.push_back('}');

        e.endpoint     = bybit::paths::order_cancel;
        e.wire_payload = std::move(body);
        return e;
    }

private:
    std::string default_symbol_;
    std::string category_    = "linear";
    bool        reduce_only_ = false;

    static std::string upper(std::string s)
    {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    // "%.8f" then strip trailing zeros / dangling dot.
    static std::string format_decimal(double v)
    {
        char tmp[48];
        std::snprintf(tmp, sizeof(tmp), "%.8f", v);
        std::string s(tmp);
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        if (s.empty()) s = "0";
        return s;
    }

    // Append `"key":"value"` with a leading comma unless first.
    // Values are assumed free of quotes/backslashes (symbols, enums, decimals,
    // validated orderLinkId). No JSON escaping — keeps the build simple.
    static void append_kv_str(std::string& out, std::string_view key,
                              std::string_view value, bool first = false)
    {
        if (!first) out.push_back(',');
        out.push_back('"');
        out.append(key);
        out.append("\":\"");
        out.append(value);
        out.push_back('"');
    }

    static void append_kv_num(std::string& out, std::string_view key, int value)
    {
        out.push_back(',');
        out.push_back('"');
        out.append(key);
        out.append("\":");
        out.append(std::to_string(value));
    }

    static void append_kv_bool(std::string& out, std::string_view key, bool value)
    {
        out.push_back(',');
        out.push_back('"');
        out.append(key);
        out.append("\":");
        out.append(value ? "true" : "false");
    }

    static const char* side_to_bybit(order_side s)
    {
        return s == order_side::buy ? "Buy" : "Sell";
    }

    static const char* tif_to_bybit(time_in_force t)
    {
        switch (t)
        {
        case time_in_force::ioc: return "IOC";
        case time_in_force::fok: return "FOK";
        case time_in_force::gtc: return "GTC";
        case time_in_force::day: return "GTC"; // no day TIF on Bybit linear
        }
        return "GTC";
    }
};

#endif // HAS_BYBIT
