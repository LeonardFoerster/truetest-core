#pragma once
#ifdef HAS_BITGET

#include "execution/order_encoder.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

// UTA v3 futures place/cancel encoder (`/api/v3/trade/*`).
// Wire payload is raw JSON with a stable key order (manual string build —
// no nlohmann). Stop types refuse until Phase 4.
//
// Quirks vs Binance futures:
//   - JSON body (not query-string)
//   - side / orderType / timeInForce are lowercase
//   - reduceOnly is "yes"/"no" strings (not bool)
//   - clientOid charset: ^[.A-Z:/a-z0-9_-]{1,32}$
//   - no posSide in one-way default
class BitgetFuturesOrderEncoder : public IOrderEncoder
{
public:
    explicit BitgetFuturesOrderEncoder(std::string default_symbol = "",
                                       std::string category = "USDT-FUTURES")
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

    // Default reduceOnly for place-order ("no" unless operator/kill path
    // flips this). Kill-Switch/DMS Phase 3 prefers close-positions; this
    // flag covers reduce-only MARKET/LIMIT when needed.
    void set_reduce_only(bool v) { reduce_only_ = v; }
    bool reduce_only() const { return reduce_only_; }

    void set_margin_mode(std::string mode)
    {
        margin_mode_ = std::move(mode);
    }

    // Bitget clientOid: 1..32 chars from [.A-Z:/a-z0-9_-]
    static bool valid_client_oid(std::string_view id)
    {
        if (id.empty() || id.size() > 32) return false;
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
        // Phase 4: preset tp/sl / stop fields. Refuse until implemented.
        if (type == order_type::stop || type == order_type::stop_limit)
            return e;

        if (!client_order_id.empty() && !valid_client_oid(client_order_id))
            return e;

        const std::string sym = upper(
            o.get_symbol().empty() ? default_symbol_ : o.get_symbol());
        if (sym.empty() || category_.empty())
            return e;

        const bool is_limit = (type == order_type::limit);
        // market only; unknown enum values refuse
        if (!is_limit && type != order_type::market)
            return e;

        std::string body;
        body.reserve(192 + client_order_id.size() + sym.size());
        body.push_back('{');
        append_kv_str(body, "category", category_, /*first=*/true);
        append_kv_str(body, "symbol", sym);
        append_kv_str(body, "side", side_to_bitget(o.get_side()));
        append_kv_str(body, "orderType", is_limit ? "limit" : "market");
        append_kv_str(body, "qty", format_decimal(o.get_quantity()));

        if (is_limit)
        {
            append_kv_str(body, "price", format_decimal(o.get_price()));
            append_kv_str(body, "timeInForce", tif_to_bitget(o.get_tif()));
        }

        if (!client_order_id.empty())
            append_kv_str(body, "clientOid", client_order_id);

        append_kv_str(body, "reduceOnly", reduce_only_ ? "yes" : "no");

        // marginMode only on limit place (plan §10.1 / §10.2)
        if (is_limit && !margin_mode_.empty())
            append_kv_str(body, "marginMode", margin_mode_);

        body.push_back('}');

        e.endpoint     = "/api/v3/trade/place-order";
        e.wire_payload = std::move(body);
        return e;
    }

    encoded_order encode_cancel(std::string_view /*symbol*/,
                                std::string_view exchange_order_id,
                                std::string_view client_order_id) override
    {
        encoded_order e;
        e.client_order_id = std::string(client_order_id);

        // Prefer orderId; fall back to clientOid. Need at least one.
        const bool has_ex = !exchange_order_id.empty();
        const bool has_cli = !client_order_id.empty();
        if (!has_ex && !has_cli)
            return e;
        if (has_cli && !has_ex && !valid_client_oid(client_order_id))
            return e;
        if (category_.empty())
            return e;

        std::string body;
        body.reserve(64 + exchange_order_id.size() + client_order_id.size());
        body.push_back('{');
        append_kv_str(body, "category", category_, /*first=*/true);
        if (has_ex)
            append_kv_str(body, "orderId", exchange_order_id);
        else
            append_kv_str(body, "clientOid", client_order_id);
        body.push_back('}');

        e.endpoint     = "/api/v3/trade/cancel-order";
        e.wire_payload = std::move(body);
        return e;
    }

private:
    std::string default_symbol_;
    std::string category_     = "USDT-FUTURES";
    std::string margin_mode_  = "crossed";
    bool        reduce_only_  = false;

    static std::string upper(std::string s)
    {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    // "%.8f" then strip trailing zeros / dangling dot (Bitget decimal strings).
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
    // validated clientOid). No JSON escaping — keeps the hot-path build simple.
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

    static const char* side_to_bitget(order_side s)
    {
        return s == order_side::buy ? "buy" : "sell";
    }

    static const char* tif_to_bitget(time_in_force t)
    {
        switch (t)
        {
        case time_in_force::ioc: return "ioc";
        case time_in_force::fok: return "fok";
        case time_in_force::gtc: return "gtc";
        case time_in_force::day: return "gtc"; // no day TIF on Bitget UTA
        }
        return "gtc";
    }
};

#endif // HAS_BITGET
