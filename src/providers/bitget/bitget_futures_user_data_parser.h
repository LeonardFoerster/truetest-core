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
        const bool funding_like = contains_fund(raw);
        if (!provider_recovery::is_authoritative_object(raw))
            return funding_like ? funding_parse_result::invalid
                                : funding_parse_result::not_funding;

        std::string_view arg;
        std::string_view topic;
        if (!provider_recovery::top_level_member(raw, "arg", arg)
            || !provider_recovery::is_authoritative_object(arg)
            || !provider_recovery::top_level_plain_string(
                arg, "topic", topic))
            return funding_like ? funding_parse_result::invalid
                                : funding_parse_result::not_funding;
        if (topic != "account")
            return funding_like ? funding_parse_result::invalid
                                : funding_parse_result::not_funding;

        std::string_view data;
        if (!provider_recovery::top_level_member(raw, "data", data)
            || !provider_recovery::is_authoritative_object_array(data))
            return funding_like ? funding_parse_result::invalid
                                : funding_parse_result::not_funding;

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
            || event_time_ms <= 0)
            return funding_parse_result::invalid;

        out = parsed_funding_update{event_time_ms, delta};
        return funding_parse_result::valid;
    }

    bool parse(std::string_view raw, parsed_exec& out) override
    {
        auto invalid = [&](std::string_view reason) {
            out = parsed_exec{};
            out.k = parsed_exec::kind::invalid;
            out.error.assign(reason.data(), reason.size());
            return true;
        };

        provider_recovery::payload_parser root(raw);
        std::string_view arg;
        const auto arg_result = root.inspect_top_level_member("arg", arg);
        if (arg_result
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
        {
            return invalid("malformed private execution envelope");
        }

        std::string_view data;
        const auto data_result = root.inspect_top_level_member("data", data);
        if (data_result
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
        {
            return invalid("malformed private execution data envelope");
        }

        std::string_view obj;
        std::size_t row_count = 0;
        const bool valid_rows = data_result
                == provider_recovery::payload_parser::member_result::unique
            && provider_recovery::every_top_level_object(
                data, [&](std::string_view row) {
                    ++row_count;
                    if (row_count == 1) obj = row;
                    return true;
                });

        if (arg_result
            == provider_recovery::payload_parser::member_result::missing)
        {
            const bool execution_like = valid_rows && row_count > 0
                && (!bitget::extract_sv_string(obj, "orderStatus").empty()
                    || !first_sv(obj, "execQty").empty());
            return execution_like
                ? invalid("malformed private execution: missing arg envelope")
                : false;
        }
        if (!provider_recovery::is_authoritative_object(arg))
            return invalid("malformed private execution arg envelope");

        std::string_view topic;
        std::string_view inst_type;
        if (!provider_recovery::top_level_plain_string(arg, "topic", topic)
            || !provider_recovery::top_level_plain_string(
                arg, "instType", inst_type))
            return invalid("malformed private execution routing envelope");

        if (topic == "position" || topic == "account")
            return false;

        // Require order or fill topic when arg is present.
        if (!topic.empty() && topic != "order" && topic != "fill"
            && topic != "fast-fill")
            return false;
        if (inst_type != "UTA")
            return invalid("unsupported private execution account type");

        std::string_view action;
        if (!provider_recovery::top_level_plain_string(raw, "action", action)
            || action != "snapshot")
            return invalid("unsupported private execution action");
        if (!valid_rows || row_count != 1 || obj.empty())
            return invalid(
                "private execution batch is malformed or explicitly unsupported");
        if (!provider_recovery::decision_members_are_unique(
                obj, {"clientOid", "orderId", "symbol", "side",
                      "orderStatus", "execQty", "execPrice", "execId",
                      "tradeId", "cumExecQty", "execTime", "updatedTime",
                      "feeDetail"}))
            return invalid("ambiguous private execution row");

        // Guard: order/fill payloads carry clientOid or orderId.
        auto client = bitget::extract_sv_string(obj, "clientOid");
        auto order_id = bitget::extract_sv_string(obj, "orderId");
        if (order_id.empty())
            order_id = bitget::extract_sv_number(obj, "orderId");
        if (client.empty() && order_id.empty())
            return invalid(
                "private order/fill payload lacks all order identities");

        out = parsed_exec{};
        out.client_order_id.assign(client.data(), client.size());
        out.exchange_order_id.assign(order_id.data(), order_id.size());
        out.symbol = std::string(bitget::extract_sv_string(obj, "symbol"));

        auto side = bitget::extract_sv_string(obj, "side");
        const bool valid_side = map_side(side, out.side);

        // Prefer fill channel fields for last_fill_*.
        const bool is_fill =
            topic == "fill" || topic == "fast-fill";

        if (is_fill && topic != "order")
        {
            auto execution_id = bitget::extract_sv_string(obj, "execId");
            if (execution_id.empty())
                execution_id = bitget::extract_sv_string(obj, "tradeId");
            out.venue_execution_id.assign(
                execution_id.data(), execution_id.size());
            const bool valid_qty = parse_finite_double(
                first_sv(obj, "execQty"), out.last_fill_qty)
                && out.last_fill_qty > 0.0;
            const bool valid_price = parse_finite_double(
                first_sv(obj, "execPrice"), out.last_fill_price)
                && out.last_fill_price > 0.0;
            // Fill channel is per-slice only — do not invent order-level
            // cumulative from execQty. ExecutionBridge accumulates
            // last_fill_qty; leave cumulative_qty at 0 when venue omits it.
            auto cum = first_sv(obj, "cumExecQty");
            const bool valid_cumulative = cum.empty()
                || (parse_finite_double(cum, out.cumulative_qty)
                    && out.cumulative_qty > 0.0);
            out.has_cumulative_qty = !cum.empty();
            out.k = parsed_exec::kind::partial_fill;
            const bool valid_fee = extract_fee(obj, out)
                && !out.commission_asset.empty();
            out.ts = parse_ts(obj, raw);
            if (!valid_side || out.symbol.empty()
                || out.venue_execution_id.empty()
                || !valid_qty || !valid_price || !valid_cumulative
                || !valid_fee
                || out.ts.time_since_epoch().count() <= 0)
            {
                out.k = parsed_exec::kind::invalid;
                out.error = "malformed fill: missing or invalid mandatory field";
            }
            return true;
        }

        // Order channel (or bare order object).
        auto status = bitget::extract_sv_string(obj, "orderStatus");
        out.k = classify_order_status(status);
        const bool valid_status = out.k != parsed_exec::kind::other;
        const auto cumulative = first_sv(obj, "cumExecQty");
        out.has_cumulative_qty = !cumulative.empty();
        const bool valid_cumulative = cumulative.empty()
            || (parse_finite_double(cumulative, out.cumulative_qty)
                && out.cumulative_qty >= 0.0);

        // Prefer fill channel for last_fill_*; leave 0 on pure order status
        // unless the venue embeds an incremental fill (rare on UTA order).
        // Do not invent last_fill from cumExecQty (double-counts vs fill).
        out.last_fill_qty = 0.0;
        out.last_fill_price = 0.0;

        // Dual-channel safety: order channel carries lifecycle only
        // (last_fill_*=0). Demote both partially_filled and filled so the
        // bridge does not untrack early — fill channel owns last_fill_qty
        // and completion is detected when cumulative >= total on the bridge.
        if ((out.k == parsed_exec::kind::partial_fill
             || out.k == parsed_exec::kind::full_fill)
            && out.last_fill_qty <= 0.0)
            out.k = parsed_exec::kind::other;

        const bool valid_fee = extract_fee(obj, out);

        if (out.k == parsed_exec::kind::rejected
            || out.k == parsed_exec::kind::canceled)
        {
            auto err = bitget::extract_sv_string(obj, "cancelReason");
            if (err.empty())
                err = bitget::extract_sv_string(obj, "msg");
            if (err.empty())
                err = bitget::extract_sv_string(obj, "error");
            out.error.assign(err.data(), err.size());
        }

        out.ts = parse_ts(obj, raw);
        if (!valid_side || out.symbol.empty() || !valid_cumulative
            || !valid_fee || !valid_status
            || out.ts.time_since_epoch().count() <= 0)
        {
            out.k = parsed_exec::kind::invalid;
            out.error = "malformed order lifecycle: invalid mandatory field";
        }
        return true;
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

        auto parse_balance_row = [](std::string_view obj,
                                    parsed_position_snapshot::balance_row& b)
            -> bool {
            auto coin = bitget::extract_sv_string(obj, "coin");
            if (coin.empty())
                coin = bitget::extract_sv_string(obj, "asset");
            b.asset.assign(coin.data(), coin.size());
            if (b.asset.empty())
                return false;

            auto bal = first_sv(obj, "available");
            if (bal.empty())
                bal = first_sv(obj, "equity");
            if (bal.empty())
                bal = first_sv(obj, "walletBalance");
            if (bal.empty())
                bal = first_sv(obj, "balance");

            auto delta = first_sv(obj, "balanceChange");
            if (delta.empty())
                delta = first_sv(obj, "change");
            if (delta.empty())
                delta = first_sv(obj, "delta");
            if (bal.empty() && delta.empty())
                return false;
            if (!bal.empty()
                && !parse_finite_double(bal, b.wallet_balance))
                return false;
            if (!delta.empty()
                && !parse_finite_double(delta, b.balance_change))
                return false;
            return true;
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
                parsed_position_snapshot::balance_row bal;
                if (parse_balance_row(body, bal))
                    out.balances.push_back(std::move(bal));
                out.ts = parse_ts(body, raw);
                return !out.balances.empty()
                    && out.ts.time_since_epoch().count() > 0;
            }
            bool valid_balance_rows = true;
            bitget::detail::for_each_array_object(arr, [&](std::string_view obj) {
                if (out.r == parsed_position_snapshot::reason::other)
                    out.r = classify_account_reason(obj);
                parsed_position_snapshot::balance_row bal;
                if (parse_balance_row(obj, bal))
                    out.balances.push_back(std::move(bal));
                else
                    valid_balance_rows = false;
            });
            auto ts_sv = first_sv(raw, "ts");
            if (!ts_sv.empty())
            {
                int64_t ms = 0;
                if (bitget::parse_int64_sv(ts_sv, ms))
                    out.ts = std::chrono::system_clock::time_point(
                        std::chrono::milliseconds(ms));
            }
            return valid_balance_rows && !out.balances.empty()
                && out.ts.time_since_epoch().count() > 0;
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
        if (status == "new")
            return parsed_exec::kind::ack;
        if (status == "partially_filled")
            return parsed_exec::kind::partial_fill;
        if (status == "filled")
            return parsed_exec::kind::full_fill;
        if (status == "cancelled")
            return parsed_exec::kind::canceled;
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

    static std::string_view first_sv(std::string_view obj, std::string_view key)
    {
        auto s = bitget::extract_sv_string(obj, key);
        if (!s.empty())
            return s;
        return bitget::extract_sv_number(obj, key);
    }

    static bool map_side(std::string_view side, order_side& out) noexcept
    {
        if (side == "sell" || side == "SELL" || side == "Sell")
        {
            out = order_side::sell;
            return true;
        }
        if (side == "buy" || side == "BUY" || side == "Buy")
        {
            out = order_side::buy;
            return true;
        }
        return false;
    }

    static bool extract_fee(std::string_view obj, parsed_exec& out)
    {
        auto fee_arr = bitget::detail::extract_array(obj, "feeDetail");
        if (!fee_arr.empty())
        {
            std::size_t rows = 0;
            bool valid = true;
            bitget::detail::for_each_array_object(
                fee_arr, [&](std::string_view fee_obj) {
                    ++rows;
                    if (rows != 1)
                    {
                        valid = false;
                        return;
                    }
                    const auto fee = first_sv(fee_obj, "fee");
                    auto coin = bitget::extract_sv_string(fee_obj, "feeCoin");
                    if (fee.empty() || coin.empty()
                        || !parse_finite_double(fee, out.commission))
                    {
                        valid = false;
                        return;
                    }
                    out.commission_asset.assign(coin.data(), coin.size());
                });
            return valid && rows == 1;
        }
        // Flat fee fields (fast-fill / REST-shaped).
        auto fee = first_sv(obj, "fee");
        auto coin = bitget::extract_sv_string(obj, "feeCoin");
        if (fee.empty() && coin.empty())
            return true;
        if (fee.empty() || coin.empty()
            || !parse_finite_double(fee, out.commission))
            return false;
        out.commission_asset.assign(coin.data(), coin.size());
        return true;
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
