#pragma once

#include "../../execution/fill_parser.h"
#include "binance_parser.h"
#include "providers/recovery_payload.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

// USDT-M futures user-data stream wraps execution detail in
// `ORDER_TRADE_UPDATE` with a nested `o:{...}`. The inner field codes
// overlap with spot's flat `executionReport`, so we slice the inner
// object and reuse the same `binance::extract_*` helpers.
// Wrapper keys we read top-level: `e` (event guard), `E` (event time).
// Everything else lives one nesting level down. The wrapper key `o`
// itself collides with the inner type field of the same name, so we
// never `extract` `o` after slicing - only the bracketing find.
class BinanceFuturesUserDataParser : public IFillParser
{
public:
    funding_parse_result parse_funding_update(
        std::string_view raw, parsed_funding_update& out) noexcept override
    {
        if (!provider_recovery::is_authoritative_object(raw))
            return funding_parse_result::not_funding;

        std::string_view event;
        if (!provider_recovery::top_level_plain_string(raw, "e", event))
            return funding_parse_result::not_funding;
        if (event != "ACCOUNT_UPDATE")
            return funding_parse_result::not_funding;

        std::string_view account;
        if (!provider_recovery::top_level_member(raw, "a", account)
            || !provider_recovery::is_authoritative_object(account))
            return funding_parse_result::invalid;

        std::string_view reason;
        if (!provider_recovery::top_level_plain_string(account, "m", reason))
            return funding_parse_result::invalid;
        if (reason != "FUNDING_FEE")
            return funding_parse_result::not_funding;

        std::string_view event_time;
        std::int64_t event_time_ms = 0;
        if (!provider_recovery::top_level_scalar_text(raw, "E", event_time)
            || !parse_int64(event_time, event_time_ms)
            || event_time_ms <= 0
            || !system_clock_millis_is_representable(event_time_ms))
            return funding_parse_result::invalid;

        std::string_view balances;
        if (!provider_recovery::top_level_member(account, "B", balances)
            || !provider_recovery::is_authoritative_object_array(balances))
            return funding_parse_result::invalid;

        std::size_t usdt_rows = 0;
        double delta = 0.0;
        const bool valid_rows = provider_recovery::every_top_level_object(
            balances, [&](std::string_view row) noexcept {
                std::string_view asset;
                std::string_view raw_delta;
                if (!provider_recovery::top_level_plain_string(
                        row, "a", asset)
                    || !provider_recovery::top_level_scalar_text(
                        row, "bc", raw_delta))
                    return false;
                double parsed = 0.0;
                if (!parse_double(raw_delta, parsed)) return false;
                if (asset == "USDT")
                {
                    ++usdt_rows;
                    delta = parsed;
                }
                return true;
            });
        if (!valid_rows || usdt_rows != 1 || delta == 0.0)
            return funding_parse_result::invalid;

        out = parsed_funding_update{event_time_ms, delta};
        return funding_parse_result::valid;
    }

    bool parse_position_snapshot(std::string_view raw,
                                 parsed_position_snapshot& out) override
    {
        std::string json(raw);

        auto event_type = binance::extract_string(json, "e");
        if (event_type != "ACCOUNT_UPDATE")
            return false;

        auto a_pos = json.find("\"a\":{");
        if (a_pos == std::string::npos) return false;
        std::string_view inner(json.data() + a_pos, json.size() - a_pos);

        out = parsed_position_snapshot{};

        auto m_str = std::string(binance::extract_sv_string(inner, "m"));
        out.r = classify_reason(m_str);

        auto ts_ms = binance::extract_number(json, "E");
        if (!ts_ms.empty())
        {
            auto ms = std::strtoll(ts_ms.c_str(), nullptr, 10);
            out.ts = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(ms));
        }

        // Walk B[] (balances) - array starts at "B":[ inside the inner.
        for_each_object_in_array(inner, "B", [&](std::string_view obj) {
            parsed_position_snapshot::balance_row row;
            row.asset = std::string(binance::extract_sv_string(obj, "a"));
            row.wallet_balance =
                to_double(binance::extract_sv_string(obj, "wb"));
            row.balance_change =
                to_double(binance::extract_sv_string(obj, "bc"));
            if (!row.asset.empty())
                out.balances.push_back(std::move(row));
        });

        // Walk P[] (positions).
        for_each_object_in_array(inner, "P", [&](std::string_view obj) {
            parsed_position_snapshot::position_row row;
            row.symbol = std::string(binance::extract_sv_string(obj, "s"));
            row.qty    = to_double(binance::extract_sv_string(obj, "pa"));
            auto mt    = binance::extract_sv_string(obj, "mt");
            row.margin_type = canonical_margin_type(mt);
            row.position_side =
                std::string(binance::extract_sv_string(obj, "ps"));
            if (!row.symbol.empty())
                out.positions.push_back(std::move(row));
        });

        return true;
    }

    execution_parse_result parse(std::string_view raw,
                                 parsed_exec& out) override
    {
        // The private transport may forward WebSocket keepalives verbatim.
        // They carry no execution identity and are deliberately harmless.
        if (raw == "ping" || raw == "pong")
            return execution_parse_result::unrelated;
        if (!provider_recovery::is_authoritative_object(raw))
            return execution_parse_result::malformed;

        std::string_view event_type;
        const auto event_member = provider_recovery::payload_parser(raw)
            .inspect_top_level_member("e", event_type);
        if (event_member
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return execution_parse_result::malformed;
        if (event_member == provider_recovery::payload_parser::member_result::missing)
            return looks_like_execution_envelope(raw)
                ? execution_parse_result::malformed
                : execution_parse_result::unrelated;
        if (!provider_recovery::top_level_plain_string(raw, "e", event_type))
            return execution_parse_result::malformed;
        // Losing the listen token makes the private stream non-authoritative.
        // Never keep live admission open after either documented termination
        // notification.
        if (event_type == "listenKeyExpired"
            || event_type == "eventStreamTerminated")
            return execution_parse_result::malformed;
        // These are authenticated order-lifecycle feeds for conditional legs
        // that this slice cannot yet carry into the engine.  Ignoring one
        // would leave a locally armed stop/TP falsely marked as protective.
        // Until Slice 3 supplies the typed lifecycle ingress, fail closed.
        if (event_type == "CONDITIONAL_ORDER_TRIGGER_REJECT"
            || event_type == "ALGO_UPDATE"
            || event_type == "TRADE_LITE")
            return execution_parse_result::malformed;
        if (event_type != "ORDER_TRADE_UPDATE")
            return looks_like_execution_envelope(raw)
                ? execution_parse_result::malformed
                : execution_parse_result::unrelated;

        std::string_view inner;
        if (!provider_recovery::top_level_member(raw, "o", inner)
            || !provider_recovery::is_authoritative_object(inner))
            return execution_parse_result::malformed;

        std::string_view symbol;
        std::string_view client;
        std::string_view exchange;
        std::string_view side;
        std::string_view execution_type;
        std::string_view order_status;
        if (!required_plain_string(inner, "s", symbol)
            || !optional_plain_string(inner, "c", client)
            || !required_numeric_order_id(inner, "i", exchange)
            || !required_plain_string(inner, "S", side)
            || !required_plain_string(inner, "x", execution_type)
            || !required_plain_string(inner, "X", order_status))
            return execution_parse_result::malformed;
        const auto parsed_kind = classify(execution_type, order_status);

        std::int64_t event_ms = 0;
        if (symbol.empty() || (client.empty() && exchange.empty())
            || (side != "BUY" && side != "SELL")
            || !parsed_kind
            || !parse_int64_required(raw, "E", event_ms)
            || event_ms <= 0
            || !system_clock_millis_is_representable(event_ms))
            return execution_parse_result::malformed;

        parsed_exec candidate;
        candidate.k = *parsed_kind;
        candidate.symbol.assign(symbol.data(), symbol.size());
        candidate.client_order_id.assign(client.data(), client.size());
        candidate.exchange_order_id.assign(exchange.data(), exchange.size());
        candidate.side = side == "SELL" ? order_side::sell : order_side::buy;
        candidate.ts = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(event_ms));

        bool has_last_qty = false;
        bool has_last_price = false;
        bool has_cumulative = false;
        bool has_commission = false;
        if (!parse_optional_finite(inner, "l", candidate.last_fill_qty,
                                   has_last_qty)
            || !parse_optional_finite(inner, "L", candidate.last_fill_price,
                                      has_last_price)
            || !parse_optional_finite(inner, "z", candidate.cumulative_qty,
                                      has_cumulative)
            || !parse_optional_finite(inner, "n", candidate.commission,
                                      has_commission)
            || candidate.cumulative_qty < 0.0)
            return execution_parse_result::malformed;

        if (candidate.k == parsed_exec::kind::partial_fill
            || candidate.k == parsed_exec::kind::full_fill)
        {
            if (!has_last_qty || !has_last_price || !has_cumulative
                || !has_commission
                || candidate.last_fill_qty <= 0.0
                || candidate.last_fill_price <= 0.0
                || candidate.cumulative_qty < candidate.last_fill_qty)
                return execution_parse_result::malformed;
        }

        std::string_view commission_asset;
        if (!optional_nullable_plain_string(inner, "N", commission_asset))
            return execution_parse_result::malformed;
        candidate.commission_asset.assign(
            commission_asset.data(), commission_asset.size());
        if ((candidate.k == parsed_exec::kind::partial_fill
             || candidate.k == parsed_exec::kind::full_fill)
            && candidate.commission != 0.0
            && candidate.commission_asset.empty())
            return execution_parse_result::malformed;
        if (candidate.k == parsed_exec::kind::rejected)
        {
            std::string_view error;
            if (!optional_plain_string(inner, "r", error))
                return execution_parse_result::malformed;
            candidate.error.assign(error.data(), error.size());
        }

        out = std::move(candidate);
        return execution_parse_result::valid;
    }

private:
    static bool looks_like_execution_envelope(std::string_view wrapper) noexcept
    {
        std::string_view inner;
        const auto outer = provider_recovery::payload_parser(wrapper)
            .inspect_top_level_member("o", inner);
        if (outer == provider_recovery::payload_parser::member_result::missing)
            return false;
        if (outer != provider_recovery::payload_parser::member_result::unique
            || !provider_recovery::is_authoritative_object(inner))
            return true;

        for (const auto key : {std::string_view{"x"}, std::string_view{"X"},
                               std::string_view{"l"}, std::string_view{"L"},
                               std::string_view{"z"}, std::string_view{"n"}})
        {
            std::string_view ignored;
            if (provider_recovery::payload_parser(inner)
                    .inspect_top_level_member(key, ignored)
                != provider_recovery::payload_parser::member_result::missing)
                return true;
        }

        std::size_t identity_members = 0;
        for (const auto key : {std::string_view{"s"}, std::string_view{"c"},
                               std::string_view{"S"}, std::string_view{"i"}})
        {
            std::string_view ignored;
            if (provider_recovery::payload_parser(inner)
                    .inspect_top_level_member(key, ignored)
                != provider_recovery::payload_parser::member_result::missing)
                ++identity_members;
        }
        return identity_members >= 2;
    }

    static bool parse_int64(std::string_view text, std::int64_t& out) noexcept
    {
        const auto [end, ec] = std::from_chars(
            text.data(), text.data() + text.size(), out);
        return ec == std::errc{} && end == text.data() + text.size();
    }

    static bool parse_double(std::string_view text, double& out) noexcept
    {
        if (text.empty()) return false;
        const auto [end, ec] = std::from_chars(
            text.data(), text.data() + text.size(), out,
            std::chars_format::general);
        return ec == std::errc{} && end == text.data() + text.size()
            && std::isfinite(out);
    }

    static bool required_plain_string(std::string_view object,
                                      std::string_view key,
                                      std::string_view& out) noexcept
    {
        return provider_recovery::top_level_plain_string(object, key, out)
            && !out.empty();
    }

    static bool optional_plain_string(std::string_view object,
                                      std::string_view key,
                                      std::string_view& out) noexcept
    {
        const auto result = provider_recovery::payload_parser(object)
            .inspect_top_level_member(key, out);
        if (result == provider_recovery::payload_parser::member_result::missing)
        {
            out = {};
            return true;
        }
        if (result
            != provider_recovery::payload_parser::member_result::unique)
            return false;
        return provider_recovery::top_level_plain_string(object, key, out);
    }

    static bool optional_nullable_plain_string(std::string_view object,
                                               std::string_view key,
                                               std::string_view& out) noexcept
    {
        const auto result = provider_recovery::payload_parser(object)
            .inspect_top_level_member(key, out);
        if (result == provider_recovery::payload_parser::member_result::missing)
        {
            out = {};
            return true;
        }
        if (result
            != provider_recovery::payload_parser::member_result::unique)
            return false;
        if (provider_recovery::is_exact_null(out))
        {
            out = {};
            return true;
        }
        return provider_recovery::top_level_plain_string(object, key, out);
    }

    static bool optional_scalar(std::string_view object,
                                std::string_view key,
                                std::string_view& out) noexcept
    {
        const auto result = provider_recovery::payload_parser(object)
            .inspect_top_level_member(key, out);
        if (result == provider_recovery::payload_parser::member_result::missing)
        {
            out = {};
            return true;
        }
        if (result
            != provider_recovery::payload_parser::member_result::unique)
            return false;
        return provider_recovery::top_level_scalar_text(object, key, out);
    }

    static bool required_scalar(std::string_view object,
                                std::string_view key,
                                std::string_view& out) noexcept
    {
        return provider_recovery::top_level_scalar_text(object, key, out)
            && !out.empty();
    }

    // Futures' `i` is a numeric venue order ID. Numeric JSON strings are
    // tolerated for wire compatibility; arbitrary identity text is not.
    static bool required_numeric_order_id(std::string_view object,
                                          std::string_view key,
                                          std::string_view& out) noexcept
    {
        if (!required_scalar(object, key, out)) return false;
        std::uint64_t ignored = 0;
        const auto [end, ec] = std::from_chars(
            out.data(), out.data() + out.size(), ignored);
        return ec == std::errc{} && end == out.data() + out.size();
    }

    static bool parse_int64_required(std::string_view object,
                                     std::string_view key,
                                     std::int64_t& out) noexcept
    {
        std::string_view text;
        if (!provider_recovery::top_level_scalar_text(object, key, text)
            || text.empty())
            return false;
        return parse_int64(text, out);
    }

    static bool parse_optional_finite(std::string_view object,
                                      std::string_view key,
                                      double& out,
                                      bool& present) noexcept
    {
        std::string_view text;
        const auto result = provider_recovery::payload_parser(object)
            .inspect_top_level_member(key, text);
        if (result
            == provider_recovery::payload_parser::member_result::missing)
        {
            present = false;
            out = 0.0;
            return true;
        }
        if (result
            != provider_recovery::payload_parser::member_result::unique
            || !provider_recovery::top_level_scalar_text(object, key, text)
            || text.empty())
            return false;
        present = true;
        return parse_double(text, out);
    }

    static bool parse_optional_finite(std::string_view object,
                                      std::string_view key,
                                      double& out) noexcept
    {
        bool ignored = false;
        return parse_optional_finite(object, key, out, ignored);
    }

    static double to_double(std::string_view sv)
    {
        if (sv.empty()) return 0.0;
        std::string s(sv);
        return std::strtod(s.c_str(), nullptr);
    }

    // Walk a JSON array nested under `array_key` inside `body`. Calls
    // `fn` once per top-level `{...}` object in the array. Brace-aware
    // so nested objects (rare in ACCOUNT_UPDATE entries but cheap to
    // handle) don't split prematurely.
    template <typename Fn>
    static void for_each_object_in_array(std::string_view body,
                                         std::string_view array_key,
                                         Fn fn)
    {
        std::string needle;
        needle.reserve(array_key.size() + 4);
        needle += '"';
        needle += array_key;
        needle += "\":[";
        auto start = body.find(needle);
        if (start == std::string_view::npos) return;
        std::size_t i = start + needle.size();

        while (i < body.size())
        {
            if (body[i] == ']') return;
            auto open = body.find('{', i);
            if (open == std::string_view::npos) return;
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
            fn(body.substr(open, j - open));
            i = j;
        }
    }

    // a.m -> reason. Binance enumerates many; we collapse to a coarse
    // set the engine actually distinguishes. "ORDER" is folded into a
    // distinct value so a consumer can dedupe against ORDER_TRADE_UPDATE
    // (the same underlying fill drives both messages on a typical fill).
    static parsed_position_snapshot::reason
    classify_reason(const std::string& m)
    {
        if (m == "ORDER")              return parsed_position_snapshot::reason::order;
        if (m == "FUNDING_FEE")        return parsed_position_snapshot::reason::funding_fee;
        if (m == "ADJUSTMENT")         return parsed_position_snapshot::reason::adjustment;
        if (m == "DEPOSIT")            return parsed_position_snapshot::reason::deposit;
        if (m == "WITHDRAW")           return parsed_position_snapshot::reason::withdraw;
        if (m == "MARGIN_TRANSFER")    return parsed_position_snapshot::reason::margin_transfer;
        if (m == "MARGIN_TYPE_CHANGE") return parsed_position_snapshot::reason::margin_type_change;
        if (m == "INSURANCE_CLEAR")    return parsed_position_snapshot::reason::liquidation;
        if (m == "ADMIN_DEPOSIT" ||
            m == "ADMIN_WITHDRAW")     return parsed_position_snapshot::reason::admin;
        if (m.empty())                 return parsed_position_snapshot::reason::unknown;
        return parsed_position_snapshot::reason::other;
    }

    // Binance returns marginType as lowercase ("isolated" / "cross").
    // Canonicalize so downstream comparisons line up with the operator's
    // expected_margin_type (which compute_advisories also normalizes).
    static std::string canonical_margin_type(std::string_view sv)
    {
        if (sv.empty()) return {};
        char first = static_cast<char>(
            std::toupper(static_cast<unsigned char>(sv[0])));
        if (first == 'I') return "ISOLATED";
        if (first == 'C') return "CROSSED";
        return std::string(sv);
    }

    static std::optional<parsed_exec::kind>
    classify(std::string_view x, std::string_view X) noexcept
    {
        if (x == "TRADE")
        {
            if (X == "FILLED")           return parsed_exec::kind::full_fill;
            if (X == "PARTIALLY_FILLED") return parsed_exec::kind::partial_fill;
            return std::nullopt;
        }
        // A liquidation CALCULATED update carries the same economic fields
        // as a normal fill.  Route it through fill validation rather than
        // misclassifying a genuine venue execution as malformed.
        if (x == "CALCULATED")
        {
            if (X == "FILLED")           return parsed_exec::kind::full_fill;
            if (X == "PARTIALLY_FILLED") return parsed_exec::kind::partial_fill;
            return std::nullopt;
        }
        if (x == "NEW" && X == "NEW")
            return parsed_exec::kind::ack;
        if (x == "CANCELED" && X == "CANCELED")
            return parsed_exec::kind::canceled;
        if (x == "REJECTED" && X == "REJECTED")
            return parsed_exec::kind::rejected;
        // Futures STP can expire an order with EXPIRED_IN_MATCH.  It is a
        // terminal expiry, not a malformed order update.
        if (x == "EXPIRED"
            && (X == "EXPIRED" || X == "EXPIRED_IN_MATCH"))
            return parsed_exec::kind::expired;
        return std::nullopt;
    }
};
