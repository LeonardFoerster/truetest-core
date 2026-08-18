#pragma once

#include "../../execution/fill_parser.h"
#include "binance_parser.h"
#include "providers/recovery_payload.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class BinanceUserDataParser : public IFillParser
{
public:
    bool is_harmless_private_control(
        std::string_view raw) const noexcept override
    {
        // Binance documents the private-stream heartbeat as a raw text
        // ping/pong.  Do not bless a JSON result/control envelope here: an
        // unrelated authenticated JSON frame may carry account truth that
        // this parser does not model.
        return raw == "ping" || raw == "pong";
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
        // Both notifications mean the authenticated private stream is no
        // longer a source of truth.  They are not harmless control traffic:
        // let the bridge poison admission instead of continuing blind.
        if (event_type == "listenKeyExpired"
            || event_type == "eventStreamTerminated")
            return execution_parse_result::malformed;
        // Spot sends listStatus alongside individual executionReports for
        // OCO/order-list state.  Slice 3 owns typed list lifecycle delivery;
        // until then, silently ignoring a terminal/rejected list would leave
        // local protection state unprovable, so fail closed.
        if (event_type == "listStatus")
            return execution_parse_result::malformed;
        if (event_type != "executionReport")
            return looks_like_execution_envelope(raw)
                ? execution_parse_result::malformed
                : execution_parse_result::unrelated;

        std::string_view symbol;
        std::string_view client;
        std::string_view exchange;
        std::string_view side;
        std::string_view execution_type;
        std::string_view order_status;
        if (!required_plain_string(raw, "s", symbol)
            || !optional_plain_string(raw, "c", client)
            || !required_numeric_order_id(raw, "i", exchange)
            || !required_plain_string(raw, "S", side)
            || !required_plain_string(raw, "x", execution_type)
            || !required_plain_string(raw, "X", order_status))
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
        if (!parse_optional_finite(raw, "l", candidate.last_fill_qty,
                                   has_last_qty)
            || !parse_optional_finite(raw, "L", candidate.last_fill_price,
                                      has_last_price)
            || !parse_optional_finite(raw, "z", candidate.cumulative_qty,
                                      has_cumulative)
            || !parse_optional_finite(raw, "n", candidate.commission,
                                      has_commission)
            || candidate.cumulative_qty < 0.0)
            return execution_parse_result::malformed;
        candidate.has_cumulative_qty = has_cumulative;

        const bool is_economic_fill =
            candidate.k == parsed_exec::kind::partial_fill
            || candidate.k == parsed_exec::kind::full_fill;
        if (is_economic_fill)
        {
            // Spot's `I` is the venue execution identity.  Do not substitute
            // the order-level `i` or the optional trade id `t`: replay proof
            // needs the immutable identity of this economic execution.
            std::uint64_t execution_id = 0;
            if (!has_last_qty || !has_last_price || !has_cumulative
                || !has_commission
                || candidate.last_fill_qty <= 0.0
                || candidate.last_fill_price <= 0.0
                || candidate.cumulative_qty < candidate.last_fill_qty
                || !provider_recovery::top_level_positive_u64(
                    raw, "I", execution_id))
                return execution_parse_result::malformed;
            candidate.execution_id = std::to_string(execution_id);
        }
        // A non-fill report may carry the cumulative quantity already booked
        // by a prior fill, but it cannot carry a fresh price/quantity/fee
        // delta.  Otherwise a lifecycle update would become an untracked
        // economic mutation on the private fast path.
        else if (((candidate.k == parsed_exec::kind::canceled
                   || candidate.k == parsed_exec::kind::rejected
                   || candidate.k == parsed_exec::kind::expired)
                  && !has_cumulative)
                 || (has_last_qty && candidate.last_fill_qty != 0.0)
                 || (has_last_price && candidate.last_fill_price != 0.0)
                 || (has_commission && candidate.commission != 0.0))
            return execution_parse_result::malformed;

        std::string_view commission_asset;
        if (!optional_nullable_plain_string(raw, "N", commission_asset))
            return execution_parse_result::malformed;
        candidate.commission_asset.assign(
            commission_asset.data(), commission_asset.size());
        if (is_economic_fill
            && candidate.commission != 0.0
            && candidate.commission_asset.empty())
            return execution_parse_result::malformed;
        if (candidate.k == parsed_exec::kind::rejected)
        {
            std::string_view error;
            if (!optional_plain_string(raw, "r", error))
                return execution_parse_result::malformed;
            candidate.error.assign(error.data(), error.size());
        }

        out = std::move(candidate);
        return execution_parse_result::valid;
    }

private:
    // A missing or unknown discriminator is harmless only when the frame does
    // not otherwise look like an execution report.  This avoids silently
    // discarding a corrupt order/fill envelope while keeping account/control
    // frames on their dedicated path.  Any duplicate candidate member is
    // itself evidence of a malformed execution-shaped frame.
    static bool looks_like_execution_envelope(std::string_view object) noexcept
    {
        for (const auto key : {std::string_view{"x"}, std::string_view{"X"},
                               std::string_view{"l"}, std::string_view{"L"},
                               std::string_view{"z"}, std::string_view{"n"}})
        {
            std::string_view ignored;
            if (provider_recovery::payload_parser(object)
                    .inspect_top_level_member(key, ignored)
                != provider_recovery::payload_parser::member_result::missing)
                return true;
        }

        std::size_t identity_members = 0;
        for (const auto key : {std::string_view{"s"}, std::string_view{"c"},
                               std::string_view{"S"}, std::string_view{"i"}})
        {
            std::string_view ignored;
            if (provider_recovery::payload_parser(object)
                    .inspect_top_level_member(key, ignored)
                != provider_recovery::payload_parser::member_result::missing)
                ++identity_members;
        }
        return identity_members >= 2;
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

    // Binance sends a JSON null commission asset when a report has no fee.
    // It is the only nullable string admitted by the execution schema.
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

    // Binance documents `i` as an integer order identity. Numeric JSON
    // strings remain interoperable, but arbitrary scalar text must never be
    // accepted as a venue identity in a safety decision.
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
        const auto [end, ec] = std::from_chars(
            text.data(), text.data() + text.size(), out);
        return ec == std::errc{} && end == text.data() + text.size();
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
        const auto [end, ec] = std::from_chars(
            text.data(), text.data() + text.size(), out,
            std::chars_format::general);
        return ec == std::errc{} && end == text.data() + text.size()
            && std::isfinite(out);
    }

    static bool parse_optional_finite(std::string_view object,
                                      std::string_view key,
                                      double& out) noexcept
    {
        bool ignored = false;
        return parse_optional_finite(object, key, out, ignored);
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
        // Spot's STP expiration is reported as TRADE_PREVENTION with the
        // EXPIRED_IN_MATCH terminal status.  It is a real venue lifecycle,
        // not malformed execution data and must reach the typed expiry path.
        if (x == "TRADE_PREVENTION"
            && (X == "EXPIRED" || X == "EXPIRED_IN_MATCH"))
            return parsed_exec::kind::expired;
        if (x == "EXPIRED"
            && (X == "EXPIRED" || X == "EXPIRED_IN_MATCH"))
            return parsed_exec::kind::expired;
        // PENDING_NEW and a replacement's NEW state remain non-terminal.
        // The bridge intentionally keeps the existing identity mapping until
        // a subsequent terminal private lifecycle is observed.
        if ((x == "NEW" || x == "REPLACED")
            && (X == "NEW" || X == "PENDING_NEW"))
            return parsed_exec::kind::ack;
        if (x == "CANCELED" && X == "CANCELED")
            return parsed_exec::kind::canceled;
        if (x == "REJECTED" && X == "REJECTED")
            return parsed_exec::kind::rejected;
        return std::nullopt;
    }
};
