#pragma once
#ifdef HAS_BITGET

#include "execution/fill_parser.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/recovery_payload.h"

#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Bitget UTA private WS → parsed_exec / parsed_position_snapshot.
// Needle-scan only (no nlohmann). Channels (arg.topic):
//   order    → lifecycle (ack / cancel / status map §9.4)
//   fill     → last_fill_* accuracy (prefer over order for fills)
//   position → signed qty snapshot §9.5
class BitgetFuturesUserDataParser : public IFillParser
{
public:
    funding_parse_result parse_funding_update(
        std::string_view raw, parsed_funding_update& out) noexcept override
    {
        if (!provider_recovery::is_authoritative_object(raw))
            return funding_parse_result::not_funding;

        // A post-ready control envelope is never an account funding push.
        // Let the regular execution parser classify it: that preserves
        // harmless subscribe/login acknowledgements while its stricter
        // control-vs-data checks reject contradictory data-bearing controls.
        for (const auto key : {std::string_view{"event"},
                               std::string_view{"op"}})
        {
            std::string_view ignored;
            if (provider_recovery::payload_parser(raw)
                    .inspect_top_level_member(key, ignored)
                != provider_recovery::payload_parser::member_result::missing)
                return funding_parse_result::not_funding;
        }

        std::string_view arg;
        const auto arg_state = provider_recovery::payload_parser(raw)
            .inspect_top_level_member("arg", arg);
        if (arg_state == provider_recovery::payload_parser::member_result::missing)
            return funding_parse_result::not_funding;
        if (arg_state != provider_recovery::payload_parser::member_result::unique
            || !provider_recovery::is_authoritative_object(arg))
            return funding_parse_result::invalid;

        std::string_view topic;
        if (!provider_recovery::top_level_plain_string(arg, "topic", topic))
            return funding_parse_result::invalid;
        if (topic != "account")
            return funding_parse_result::not_funding;
        // Funding is authoritative only on our subscribed UTA account
        // channel.  Do not let a same-named foreign/malformed channel enter
        // the bridge funding fast-path and bypass execution admission.
        if (!provider_recovery::top_level_exact_string(arg, "instType", "UTA"))
            return funding_parse_result::invalid;

        // Only an authoritative account-channel envelope may be considered
        // funding-like.  A client id, symbol, or error text containing
        // "fund" on the order/fill channels must never preempt execution
        // parsing and turn an otherwise valid lifecycle record fatal.
        const bool funding_like = contains_fund(raw);

        std::string_view data;
        if (!provider_recovery::top_level_member(raw, "data", data)
            || !provider_recovery::is_authoritative_object_array(data))
            return funding_parse_result::invalid;

        std::size_t funding_rows = 0;
        bool non_funding_row = false;
        double delta = 0.0;
        const bool valid_rows = provider_recovery::every_top_level_object(
            data, [&](std::string_view row) noexcept {
                std::string_view reason;
                std::size_t reason_fields = 0;
                for (const auto key : {std::string_view{"bizType"},
                                       std::string_view{"type"},
                                       std::string_view{"changeType"}})
                {
                    std::string_view raw_reason;
                    const auto result = provider_recovery::payload_parser(row)
                        .inspect_top_level_member(key, raw_reason);
                    if (result == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
                        return false;
                    if (result == provider_recovery::payload_parser::member_result::unique)
                    {
                        ++reason_fields;
                        if (!provider_recovery::top_level_plain_string(
                                row, key, reason))
                            return false;
                    }
                }
                if (reason_fields == 0)
                {
                    if (contains_fund(row)) return false;
                    non_funding_row = true;
                    return true;
                }
                if (reason_fields != 1) return false;
                if (reason != "funding_fee")
                {
                    if (contains_fund(reason)) return false;
                    non_funding_row = true;
                    return true;
                }

                std::string_view asset;
                std::size_t asset_fields = 0;
                for (const auto key : {std::string_view{"coin"},
                                       std::string_view{"asset"}})
                {
                    std::string_view ignored;
                    const auto result = provider_recovery::payload_parser(row)
                        .inspect_top_level_member(key, ignored);
                    if (result == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
                        return false;
                    if (result == provider_recovery::payload_parser::member_result::unique)
                    {
                        ++asset_fields;
                        if (!provider_recovery::top_level_plain_string(
                                row, key, asset))
                            return false;
                    }
                }

                std::string_view raw_delta;
                std::size_t delta_fields = 0;
                for (const auto key : {std::string_view{"balanceChange"},
                                       std::string_view{"change"},
                                       std::string_view{"delta"}})
                {
                    std::string_view ignored;
                    const auto result = provider_recovery::payload_parser(row)
                        .inspect_top_level_member(key, ignored);
                    if (result == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
                        return false;
                    if (result == provider_recovery::payload_parser::member_result::unique)
                    {
                        ++delta_fields;
                        if (!provider_recovery::top_level_scalar_text(
                                row, key, raw_delta))
                            return false;
                    }
                }

                double parsed = 0.0;
                if (asset_fields != 1 || delta_fields != 1
                    || !is_usdt(asset)
                    || !parse_finite_double(raw_delta, parsed)
                    || parsed == 0.0)
                    return false;
                ++funding_rows;
                delta = parsed;
                return true;
            });

        if (!valid_rows)
            return funding_like ? funding_parse_result::invalid
                                : funding_parse_result::not_funding;
        if (funding_rows == 0)
            return funding_like ? funding_parse_result::invalid
                                : funding_parse_result::not_funding;
        if (funding_rows != 1 || non_funding_row)
            return funding_parse_result::invalid;

        std::string_view raw_ts;
        std::int64_t event_time_ms = 0;
        if (!provider_recovery::top_level_scalar_text(raw, "ts", raw_ts)
            || !parse_int64(raw_ts, event_time_ms)
            || event_time_ms <= 0
            || !system_clock_millis_is_representable(event_time_ms))
            return funding_parse_result::invalid;

        out = parsed_funding_update{event_time_ms, delta};
        return funding_parse_result::valid;
    }

    execution_parse_result parse(std::string_view raw,
                                 parsed_exec& out) override
    {
        if (raw == "ping" || raw == "pong")
            return execution_parse_result::unrelated;
        if (!provider_recovery::is_authoritative_object(raw))
            return execution_parse_result::malformed;

        std::string_view control_event;
        const auto control_member = provider_recovery::payload_parser(raw)
            .inspect_top_level_member("event", control_event);
        if (control_member
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return execution_parse_result::malformed;
        if (control_member == provider_recovery::payload_parser::member_result::unique)
        {
            if (!provider_recovery::top_level_plain_string(
                    raw, "event", control_event))
                return execution_parse_result::malformed;
            if (is_harmless_control(raw, control_event))
                return execution_parse_result::unrelated;
            // Post-ready server errors and unrecognised controls are not a
            // harmless payload.  The transport only classifies login/
            // subscription failures during setup, so preserve fail-closed
            // behaviour after readiness as well.
            return execution_parse_result::malformed;
        }

        std::string_view control_op;
        const auto op_member = provider_recovery::payload_parser(raw)
            .inspect_top_level_member("op", control_op);
        if (op_member
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return execution_parse_result::malformed;
        if (op_member == provider_recovery::payload_parser::member_result::unique)
        {
            if (!provider_recovery::top_level_plain_string(raw, "op", control_op))
                return execution_parse_result::malformed;
            if (is_harmless_control(raw, control_op))
                return execution_parse_result::unrelated;
            return execution_parse_result::malformed;
        }

        std::string_view arg;
        const auto arg_member = provider_recovery::payload_parser(raw)
            .inspect_top_level_member("arg", arg);
        if (arg_member
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return execution_parse_result::malformed;
        if (arg_member == provider_recovery::payload_parser::member_result::missing)
            return looks_like_private_push(raw)
                ? execution_parse_result::malformed
                : execution_parse_result::unrelated;
        if (!provider_recovery::is_authoritative_object(arg))
            return execution_parse_result::malformed;

        std::string_view topic;
        if (!provider_recovery::top_level_exact_string(arg, "instType", "UTA")
            || !provider_recovery::top_level_plain_string(arg, "topic", topic))
            return execution_parse_result::malformed;

        if (topic == "position" || topic == "account")
            return looks_like_execution_data(raw)
                ? execution_parse_result::malformed
                : execution_parse_result::unrelated;

        if (topic != "order" && topic != "fill"
            && topic != "fast-fill")
            return looks_like_private_push(raw)
                ? execution_parse_result::malformed
                : execution_parse_result::unrelated;

        // The UTA order/fill channels currently specify snapshot as their
        // only push action.  Missing, duplicate, non-string, or unknown
        // action makes a known execution envelope structurally ambiguous.
        std::string_view action;
        const auto action_member = provider_recovery::payload_parser(raw)
            .inspect_top_level_member("action", action);
        if (action_member
            != provider_recovery::payload_parser::member_result::unique
            || !provider_recovery::top_level_plain_string(raw, "action", action)
            || action != "snapshot")
            return execution_parse_result::malformed;

        std::string_view data;
        if (!provider_recovery::top_level_member(raw, "data", data)
            || !provider_recovery::is_authoritative_object_array(data))
            return execution_parse_result::malformed;

        std::string_view object;
        std::size_t rows = 0;
        const bool single_row = provider_recovery::every_top_level_object(
            data, [&](std::string_view row) noexcept {
                if (++rows != 1) return false;
                object = row;
                return true;
            });
        if (!single_row || rows != 1)
            return execution_parse_result::malformed;

        parsed_exec candidate;
        if (!parse_execution_object(topic, object, raw, candidate))
            return execution_parse_result::malformed;
        out = std::move(candidate);
        return execution_parse_result::valid;
    }

    bool parse_position_snapshot(std::string_view raw,
                                 parsed_position_snapshot& out) override
    {
        auto arg = bitget::detail::extract_object(raw, "arg");
        std::string_view topic;
        if (!arg.empty())
            topic = bitget::extract_sv_string(arg, "topic");

        // position + account (Phase 4 funding/balance) only.
        if (!topic.empty() && topic != "position" && topic != "account")
            return false;

        // Without topic, only accept frames that look like position rows
        // (posSide / size) — never order/fill payloads.
        auto looks_like_position = [](std::string_view obj) {
            if (obj.empty())
                return false;
            if (!bitget::extract_sv_string(obj, "orderStatus").empty())
                return false;
            if (!bitget::extract_sv_string(obj, "execQty").empty()
                || !bitget::extract_sv_number(obj, "execQty").empty())
                return false;
            const bool has_side =
                !bitget::extract_sv_string(obj, "posSide").empty()
                || !bitget::extract_sv_string(obj, "holdSide").empty();
            const bool has_size =
                !bitget::extract_sv_string(obj, "size").empty()
                || !bitget::extract_sv_number(obj, "size").empty()
                || !bitget::extract_sv_string(obj, "total").empty()
                || !bitget::extract_sv_number(obj, "total").empty();
            return has_side || has_size;
        };

        auto looks_like_account = [](std::string_view obj) {
            if (obj.empty())
                return false;
            // coin / available / equity / balanceChange / asset style fields
            if (!bitget::extract_sv_string(obj, "coin").empty())
                return true;
            if (!bitget::extract_sv_string(obj, "asset").empty())
                return true;
            if (!bitget::extract_sv_string(obj, "available").empty()
                || !bitget::extract_sv_number(obj, "available").empty())
                return true;
            if (!bitget::extract_sv_string(obj, "equity").empty()
                || !bitget::extract_sv_number(obj, "equity").empty())
                return true;
            return false;
        };

        auto classify_account_reason = [](std::string_view obj)
            -> parsed_position_snapshot::reason {
            auto t = bitget::extract_sv_string(obj, "bizType");
            if (t.empty())
                t = bitget::extract_sv_string(obj, "type");
            if (t.empty())
                t = bitget::extract_sv_string(obj, "changeType");
            // Common funding labels across venues / UTA variants.
            if (t.find("fund") != std::string_view::npos
                || t.find("Fund") != std::string_view::npos
                || t.find("FUND") != std::string_view::npos)
                return parsed_position_snapshot::reason::funding_fee;
            if (t.find("liq") != std::string_view::npos
                || t.find("Liq") != std::string_view::npos)
                return parsed_position_snapshot::reason::liquidation;
            return parsed_position_snapshot::reason::other;
        };

        auto parse_balance_row = [](std::string_view obj)
            -> parsed_position_snapshot::balance_row {
            parsed_position_snapshot::balance_row b;
            auto coin = bitget::extract_sv_string(obj, "coin");
            if (coin.empty())
                coin = bitget::extract_sv_string(obj, "asset");
            b.asset.assign(coin.data(), coin.size());

            auto bal = first_sv(obj, "available");
            if (bal.empty())
                bal = first_sv(obj, "equity");
            if (bal.empty())
                bal = first_sv(obj, "walletBalance");
            if (bal.empty())
                bal = first_sv(obj, "balance");
            b.wallet_balance = to_double(bal);

            auto delta = first_sv(obj, "balanceChange");
            if (delta.empty())
                delta = first_sv(obj, "change");
            if (delta.empty())
                delta = first_sv(obj, "delta");
            b.balance_change = to_double(delta);
            return b;
        };

        // --- account channel (balances / funding) ---
        if (topic == "account")
        {
            out = parsed_position_snapshot{};
            out.r = parsed_position_snapshot::reason::other;
            auto arr = bitget::detail::extract_array(raw, "data");
            if (arr.empty())
            {
                auto data_obj = bitget::detail::extract_object(raw, "data");
                std::string_view body =
                    !data_obj.empty() ? data_obj : raw;
                out.r = classify_account_reason(body);
                auto bal = parse_balance_row(body);
                if (!bal.asset.empty() || bal.wallet_balance != 0.0
                    || bal.balance_change != 0.0)
                    out.balances.push_back(std::move(bal));
                out.ts = parse_ts(body, raw);
                return !out.balances.empty();
            }
            bitget::detail::for_each_array_object(arr, [&](std::string_view obj) {
                if (out.r == parsed_position_snapshot::reason::other)
                    out.r = classify_account_reason(obj);
                auto bal = parse_balance_row(obj);
                if (!bal.asset.empty() || bal.wallet_balance != 0.0
                    || bal.balance_change != 0.0)
                    out.balances.push_back(std::move(bal));
            });
            auto ts_sv = first_sv(raw, "ts");
            if (!ts_sv.empty())
            {
                int64_t ms = 0;
                if (bitget::parse_int64_sv(ts_sv, ms))
                    out.ts = std::chrono::system_clock::time_point(
                        std::chrono::milliseconds(ms));
            }
            return !out.balances.empty();
        }

        auto arr = bitget::detail::extract_array(raw, "data");
        if (arr.empty())
        {
            auto data_obj = bitget::detail::extract_object(raw, "data");
            std::string_view body =
                !data_obj.empty() ? data_obj : (topic == "position" ? raw : std::string_view{});
            if (body.empty() && topic.empty() && looks_like_position(raw))
                body = raw;
            if (body.empty())
                return false;
            if (topic.empty() && !looks_like_position(body))
                return false;

            out = parsed_position_snapshot{};
            // Position topic is a venue state push, not an order lifecycle
            // event — use `other` so provider logging is not filtered out
            // (handler skips reason::order as fill-redundant).
            out.r = (topic == "position")
                ? parsed_position_snapshot::reason::other
                : parsed_position_snapshot::reason::unknown;
            if (auto row = parse_position_row(body); !row.symbol.empty())
                out.positions.push_back(std::move(row));
            out.ts = parse_ts(body, raw);
            // Empty snapshot on topic=position is still valid (flat book).
            return topic == "position" || !out.positions.empty();
        }

        if (topic.empty())
        {
            std::string_view first;
            bitget::detail::for_each_array_object(arr, [&](std::string_view o) {
                if (first.empty())
                    first = o;
            });
            if (!looks_like_position(first) && !looks_like_account(first))
                return false;
        }

        out = parsed_position_snapshot{};
        out.r = (topic == "position")
            ? parsed_position_snapshot::reason::other
            : parsed_position_snapshot::reason::unknown;

        bitget::detail::for_each_array_object(arr, [&](std::string_view obj) {
            auto row = parse_position_row(obj);
            if (!row.symbol.empty())
                out.positions.push_back(std::move(row));
        });

        auto ts_sv = bitget::extract_sv_number(raw, "ts");
        if (ts_sv.empty())
            ts_sv = bitget::extract_sv_string(raw, "ts");
        if (!ts_sv.empty())
        {
            int64_t ms = 0;
            if (bitget::parse_int64_sv(ts_sv, ms))
                out.ts = std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(ms));
        }

        return true;
    }

    // long > 0, short < 0. size may already be signed; posSide normalizes.
    static double signed_position_qty(std::string_view size_sv,
                                      std::string_view pos_side)
    {
        double size = 0.0;
        if (!size_sv.empty())
            bitget::parse_double_sv(size_sv, size);

        const bool short_side =
            pos_side == "short" || pos_side == "SHORT"
            || pos_side == "sell" || pos_side == "SELL";
        const bool long_side =
            pos_side == "long" || pos_side == "LONG"
            || pos_side == "buy" || pos_side == "BUY";

        if (short_side)
            return -std::abs(size);
        if (long_side)
            return std::abs(size);
        // No side hint: keep venue sign (one-way may already be signed).
        return size;
    }

    static parsed_exec::kind classify_order_status(std::string_view status)
    {
        if (status == "new" || status == "live" || status == "init")
            return parsed_exec::kind::ack;
        if (status == "partially_filled")
            return parsed_exec::kind::partial_fill;
        if (status == "filled")
            return parsed_exec::kind::full_fill;
        if (status == "cancelled" || status == "canceled")
            return parsed_exec::kind::canceled;
        if (status == "rejected")
            return parsed_exec::kind::rejected;
        return parsed_exec::kind::other;
    }

    static std::string canonical_margin_mode(std::string_view mode)
    {
        if (mode.empty())
            return {};
        // crossed / cross → CROSSED; isolated → ISOLATED
        char c0 = static_cast<char>(
            std::tolower(static_cast<unsigned char>(mode[0])));
        if (c0 == 'c')
            return "CROSSED";
        if (c0 == 'i')
            return "ISOLATED";
        return std::string(mode);
    }

private:
    // A data-bearing push with no usable routing envelope cannot be safely
    // reclassified as a position/control update.  Keep the test deliberately
    // narrow so a pure control frame remains unrelated.
    static bool looks_like_private_push(std::string_view raw) noexcept
    {
        for (const auto key : {std::string_view{"action"},
                               std::string_view{"data"}})
        {
            std::string_view ignored;
            if (provider_recovery::payload_parser(raw)
                    .inspect_top_level_member(key, ignored)
                != provider_recovery::payload_parser::member_result::missing)
                return true;
        }
        return false;
    }

    // Account and position state live on separate channels, but a corrupt
    // routing discriminator must not turn a complete order/fill row into an
    // ignorable account update.  Look only for order-specific row fields so
    // legitimate position/account snapshots keep their existing route.
    static bool looks_like_execution_data(std::string_view raw) noexcept
    {
        std::string_view data;
        const auto data_state = provider_recovery::payload_parser(raw)
            .inspect_top_level_member("data", data);
        if (data_state == provider_recovery::payload_parser::member_result::missing)
            return false;
        // A duplicate/invalid data member on an execution-shaped private push
        // is not a harmless position snapshot.  The outer channel can be
        // corrupted too, so fail closed rather than delegating to the legacy
        // snapshot parser's permissive object fallback.
        if (data_state != provider_recovery::payload_parser::member_result::unique)
            return true;

        bool found_execution_field = false;
        const auto row_has_execution_field = [&](std::string_view row) noexcept {
            for (const auto key : {std::string_view{"orderId"},
                                   std::string_view{"clientOid"},
                                   std::string_view{"execQty"},
                                   std::string_view{"execPrice"},
                                   std::string_view{"orderStatus"}})
            {
                std::string_view ignored;
                if (provider_recovery::payload_parser(row)
                        .inspect_top_level_member(key, ignored)
                    != provider_recovery::payload_parser::member_result::missing)
                    return true;
            }
            return false;
        };

        if (provider_recovery::is_authoritative_object_array(data))
        {
            (void)provider_recovery::every_top_level_object(
                data, [&](std::string_view row) noexcept {
                    found_execution_field = row_has_execution_field(row);
                    return !found_execution_field;
                });
        }
        else if (provider_recovery::is_authoritative_object(data))
        {
            found_execution_field = row_has_execution_field(data);
        }
        return found_execution_field;
    }

    static bool control_code_is_success(std::string_view raw,
                                        bool required) noexcept
    {
        std::string_view raw_code;
        const auto state = provider_recovery::payload_parser(raw)
            .inspect_top_level_member("code", raw_code);
        if (state
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return false;
        if (state == provider_recovery::payload_parser::member_result::missing)
            return !required;
        std::string_view code;
        return provider_recovery::top_level_scalar_text(raw, "code", code)
            && (code == "0" || code == "00000");
    }

    static bool is_harmless_control(std::string_view raw,
                                    std::string_view control) noexcept
    {
        // A control discriminator must never mask a data-bearing execution
        // frame.  This is intentionally strict: an embedded order/fill is a
        // contradictory private envelope, not an ignorable notification.
        // `action` is equally execution-specific on Bitget's private push
        // schema.  In particular, an `info` control carrying
        // `action:"snapshot"` plus an order/fill routing arg but missing
        // `data` is a malformed execution envelope, not an informational
        // control frame.
        std::string_view action;
        if (provider_recovery::payload_parser(raw).inspect_top_level_member(
                "action", action)
            != provider_recovery::payload_parser::member_result::missing)
            return false;
        std::string_view data;
        if (provider_recovery::payload_parser(raw).inspect_top_level_member(
                "data", data)
            != provider_recovery::payload_parser::member_result::missing)
            return false;

        if (control == "subscribe")
        {
            if (!control_code_is_success(raw, false)) return false;
            std::string_view arg;
            std::string_view topic;
            return provider_recovery::top_level_member(raw, "arg", arg)
                && provider_recovery::is_authoritative_object(arg)
                && provider_recovery::top_level_exact_string(
                    arg, "instType", "UTA")
                && provider_recovery::top_level_plain_string(arg, "topic", topic)
                && (topic == "order" || topic == "fill"
                    || topic == "position" || topic == "account");
        }
        if (control == "login")
            return control_code_is_success(raw, true);
        if (control == "info" || control == "ping" || control == "pong")
            return control_code_is_success(raw, false);
        return false;
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
        return parse_finite_double(text, out);
    }

    static bool parse_optional_finite(std::string_view object,
                                      std::string_view key,
                                      double& out) noexcept
    {
        bool ignored = false;
        return parse_optional_finite(object, key, out, ignored);
    }

    static bool parse_positive_timestamp(std::string_view object,
                                         std::string_view key,
                                         std::int64_t& out) noexcept
    {
        std::string_view text;
        if (!provider_recovery::top_level_scalar_text(object, key, text)
            || text.empty() || !parse_int64(text, out))
            return false;
        return out > 0 && system_clock_millis_is_representable(out);
    }

    static bool parse_timestamp(std::string_view object,
                                std::string_view wrapper,
                                std::chrono::system_clock::time_point& out) noexcept
    {
        std::int64_t wrapper_ms = 0;
        if (!parse_positive_timestamp(wrapper, "ts", wrapper_ms))
            return false;

        std::int64_t selected_ms = wrapper_ms;
        bool selected_from_object = false;
        for (const auto key : {std::string_view{"execTime"},
                               std::string_view{"updatedTime"},
                               std::string_view{"createdTime"},
                               std::string_view{"ts"}})
        {
            std::string_view value;
            const auto result = provider_recovery::payload_parser(object)
                .inspect_top_level_member(key, value);
            if (result
                == provider_recovery::payload_parser::member_result::missing)
                continue;
            if (result
                != provider_recovery::payload_parser::member_result::unique
                || !provider_recovery::top_level_scalar_text(object, key, value)
                || value.empty())
                return false;
            std::int64_t parsed = 0;
            if (!parse_int64(value, parsed) || parsed <= 0
                || !system_clock_millis_is_representable(parsed))
                return false;
            if (!selected_from_object)
            {
                selected_ms = parsed;
                selected_from_object = true;
            }
        }

        out = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(selected_ms));
        return true;
    }

    static std::optional<order_side>
    parse_execution_side(std::string_view side) noexcept
    {
        if (side == "buy" || side == "BUY") return order_side::buy;
        if (side == "sell" || side == "SELL") return order_side::sell;
        return std::nullopt;
    }

    static bool known_order_status(std::string_view status) noexcept
    {
        return status == "new" || status == "live" || status == "init"
            || status == "partially_filled" || status == "filled"
            || status == "cancelled" || status == "canceled"
            || status == "rejected";
    }

    static bool parse_fee(std::string_view object, parsed_exec& out)
    {
        std::string_view fee_detail;
        const auto detail_result = provider_recovery::payload_parser(object)
            .inspect_top_level_member("feeDetail", fee_detail);
        if (detail_result
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return false;

        if (detail_result == provider_recovery::payload_parser::member_result::unique
            && !provider_recovery::is_exact_null(fee_detail))
        {
            if (!provider_recovery::is_authoritative_object_array(fee_detail))
                return false;
            std::size_t rows = 0;
            return provider_recovery::every_top_level_object(
                fee_detail, [&](std::string_view fee_object) {
                    if (++rows != 1) return false;
                    bool has_fee = false;
                    double fee = 0.0;
                    std::string_view asset;
                    if (!parse_optional_finite(fee_object, "fee", fee, has_fee)
                        || !has_fee || !optional_plain_string(
                            fee_object, "feeCoin", asset))
                        return false;
                    if (fee != 0.0 && asset.empty()) return false;
                    out.commission = std::abs(fee);
                    out.commission_asset.assign(asset.data(), asset.size());
                    return true;
                });
        }

        bool has_fee = false;
        double fee = 0.0;
        std::string_view asset;
        if (!parse_optional_finite(object, "fee", fee, has_fee)
            || !optional_plain_string(object, "feeCoin", asset))
            return false;
        if (has_fee && fee != 0.0 && asset.empty()) return false;
        if (has_fee) out.commission = std::abs(fee);
        out.commission_asset.assign(asset.data(), asset.size());
        return true;
    }

    static bool parse_execution_object(std::string_view topic,
                                       std::string_view object,
                                       std::string_view wrapper,
                                       parsed_exec& out)
    {
        if (!provider_recovery::is_authoritative_object(object)) return false;

        std::string_view client;
        std::string_view exchange;
        std::string_view symbol;
        std::string_view side_text;
        if (!optional_plain_string(object, "clientOid", client)
            || !required_scalar(object, "orderId", exchange)
            || !required_plain_string(object, "symbol", symbol)
            || !required_plain_string(object, "side", side_text))
            return false;
        const auto side = parse_execution_side(side_text);
        if ((client.empty() && exchange.empty()) || !side) return false;

        std::string_view category;
        if (!required_plain_string(object, "category", category)
            || (category != "usdt-futures" && category != "USDT-FUTURES"))
            return false;

        parsed_exec candidate;
        candidate.client_order_id.assign(client.data(), client.size());
        candidate.exchange_order_id.assign(exchange.data(), exchange.size());
        candidate.symbol.assign(symbol.data(), symbol.size());
        candidate.side = *side;

        if (topic == "fill" || topic == "fast-fill")
        {
            bool has_qty = false;
            bool has_price = false;
            bool has_cumulative = false;
            if (!parse_optional_finite(object, "execQty", candidate.last_fill_qty,
                                       has_qty)
                || !parse_optional_finite(object, "execPrice",
                                          candidate.last_fill_price, has_price)
                || !parse_optional_finite(object, "cumExecQty",
                                          candidate.cumulative_qty, has_cumulative)
                || !has_qty || !has_price
                || candidate.last_fill_qty <= 0.0
                || candidate.last_fill_price <= 0.0
                || (has_cumulative
                    && candidate.cumulative_qty < candidate.last_fill_qty))
                return false;
            candidate.k = parsed_exec::kind::partial_fill;
        }
        else
        {
            std::string_view status;
            bool has_cumulative = false;
            if (!required_plain_string(object, "orderStatus", status)
                || !known_order_status(status)
                || !parse_optional_finite(object, "cumExecQty",
                                          candidate.cumulative_qty, has_cumulative)
                || candidate.cumulative_qty < 0.0)
                return false;
            candidate.k = classify_order_status(status);

            if ((candidate.k == parsed_exec::kind::partial_fill
                 || candidate.k == parsed_exec::kind::full_fill)
                && (!has_cumulative || candidate.cumulative_qty <= 0.0))
                return false;

            // Acknowledgements and rejections cannot truthfully report an
            // already-executed quantity.  Cancellation is intentionally
            // exempt: a partially filled order may be cancelled afterwards.
            if ((candidate.k == parsed_exec::kind::ack
                 || candidate.k == parsed_exec::kind::rejected)
                && has_cumulative && candidate.cumulative_qty > 0.0)
                return false;

            // UTA's order channel is lifecycle-only.  The fill channel owns
            // incremental economic quantities, so keep completed status
            // tracked until the corresponding fill slice is processed.
            if (candidate.k == parsed_exec::kind::partial_fill
                || candidate.k == parsed_exec::kind::full_fill)
                candidate.k = parsed_exec::kind::other;

            if (candidate.k == parsed_exec::kind::rejected
                || candidate.k == parsed_exec::kind::canceled)
            {
                std::string_view error;
                if (!optional_plain_string(object, "cancelReason", error))
                    return false;
                candidate.error.assign(error.data(), error.size());
            }
        }

        if (!parse_fee(object, candidate)
            || !parse_timestamp(object, wrapper, candidate.ts))
            return false;
        out = std::move(candidate);
        return true;
    }

    static bool contains_fund(std::string_view text) noexcept
    {
        if (text.size() < 4) return false;
        for (std::size_t i = 0; i + 4 <= text.size(); ++i)
        {
            const auto lower = [](char c) noexcept {
                return static_cast<char>(std::tolower(
                    static_cast<unsigned char>(c)));
            };
            if (lower(text[i]) == 'f' && lower(text[i + 1]) == 'u'
                && lower(text[i + 2]) == 'n' && lower(text[i + 3]) == 'd')
                return true;
        }
        return false;
    }

    static bool is_usdt(std::string_view asset) noexcept
    {
        return asset.size() == 4
            && (asset[0] == 'U' || asset[0] == 'u')
            && (asset[1] == 'S' || asset[1] == 's')
            && (asset[2] == 'D' || asset[2] == 'd')
            && (asset[3] == 'T' || asset[3] == 't');
    }

    static bool parse_int64(std::string_view text, std::int64_t& out) noexcept
    {
        const auto [end, ec] = std::from_chars(
            text.data(), text.data() + text.size(), out);
        return ec == std::errc{} && end == text.data() + text.size();
    }

    static bool parse_finite_double(
        std::string_view text, double& out) noexcept
    {
        const auto [end, ec] = std::from_chars(
            text.data(), text.data() + text.size(), out,
            std::chars_format::general);
        return ec == std::errc{} && end == text.data() + text.size()
            && std::isfinite(out);
    }

    static double to_double(std::string_view sv)
    {
        if (sv.empty())
            return 0.0;
        double v = 0.0;
        if (bitget::parse_double_sv(sv, v))
            return v;
        return 0.0;
    }

    static std::string_view first_sv(std::string_view obj, std::string_view key)
    {
        auto s = bitget::extract_sv_string(obj, key);
        if (!s.empty())
            return s;
        return bitget::extract_sv_number(obj, key);
    }

    static order_side map_side(std::string_view side)
    {
        if (side == "sell" || side == "SELL" || side == "Sell")
            return order_side::sell;
        return order_side::buy;
    }

    static void extract_fee(std::string_view obj, parsed_exec& out)
    {
        auto fee_arr = bitget::detail::extract_array(obj, "feeDetail");
        if (!fee_arr.empty())
        {
            bool done = false;
            bitget::detail::for_each_array_object(
                fee_arr, [&](std::string_view fee_obj) {
                    if (done)
                        return;
                    out.commission = std::abs(to_double(first_sv(fee_obj, "fee")));
                    auto coin = bitget::extract_sv_string(fee_obj, "feeCoin");
                    out.commission_asset.assign(coin.data(), coin.size());
                    done = true;
                });
            return;
        }
        // Flat fee fields (fast-fill / REST-shaped).
        auto fee = first_sv(obj, "fee");
        if (!fee.empty())
            out.commission = std::abs(to_double(fee));
        auto coin = bitget::extract_sv_string(obj, "feeCoin");
        if (!coin.empty())
            out.commission_asset.assign(coin.data(), coin.size());
    }

    static std::chrono::system_clock::time_point
    parse_ts(std::string_view obj, std::string_view wrapper)
    {
        for (const char* key : {"execTime", "updatedTime", "createdTime", "ts"})
        {
            auto sv = first_sv(obj, key);
            if (sv.empty() && key[0] == 't')
                sv = first_sv(wrapper, "ts");
            if (sv.empty())
                continue;
            int64_t ms = 0;
            if (bitget::parse_int64_sv(sv, ms))
                return std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(ms));
        }
        return {};
    }

    static parsed_position_snapshot::position_row
    parse_position_row(std::string_view obj)
    {
        parsed_position_snapshot::position_row row;
        auto sym = bitget::extract_sv_string(obj, "symbol");
        row.symbol.assign(sym.data(), sym.size());

        auto size_sv = first_sv(obj, "size");
        if (size_sv.empty())
            size_sv = first_sv(obj, "total");
        auto pos_side = bitget::extract_sv_string(obj, "posSide");
        if (pos_side.empty())
            pos_side = bitget::extract_sv_string(obj, "holdSide");
        row.qty = signed_position_qty(size_sv, pos_side);
        row.position_side.assign(pos_side.data(), pos_side.size());

        auto mm = bitget::extract_sv_string(obj, "marginMode");
        if (mm.empty())
            mm = bitget::extract_sv_string(obj, "marginType");
        row.margin_type = canonical_margin_mode(mm);
        return row;
    }
};

#endif // HAS_BITGET
