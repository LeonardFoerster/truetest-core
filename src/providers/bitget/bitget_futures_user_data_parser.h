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
        auto arg = bitget::detail::extract_object(raw, "arg");
        std::string_view topic;
        if (!arg.empty())
            topic = bitget::extract_sv_string(arg, "topic");

        if (topic == "position" || topic == "account")
            return false;

        // Require order or fill topic when arg is present.
        if (!topic.empty() && topic != "order" && topic != "fill"
            && topic != "fast-fill")
            return false;

        auto obj = bitget::detail::first_data_object(raw);
        if (obj.empty())
        {
            // Some pushes nest a single object under "data":{...}.
            auto data_obj = bitget::detail::extract_object(raw, "data");
            if (!data_obj.empty())
                obj = data_obj;
        }
        if (obj.empty())
            return false;

        // Guard: order/fill payloads carry clientOid or orderId.
        auto client = bitget::extract_sv_string(obj, "clientOid");
        auto order_id = bitget::extract_sv_string(obj, "orderId");
        if (order_id.empty())
            order_id = bitget::extract_sv_number(obj, "orderId");
        if (client.empty() && order_id.empty())
            return false;

        // Position-looking objects (size + posSide, no orderStatus/execQty)
        // must not leak into the exec path.
        if (topic.empty())
        {
            auto status = bitget::extract_sv_string(obj, "orderStatus");
            auto exec_qty = bitget::extract_sv_string(obj, "execQty");
            if (exec_qty.empty())
                exec_qty = bitget::extract_sv_number(obj, "execQty");
            if (status.empty() && exec_qty.empty())
                return false;
        }

        out = parsed_exec{};
        out.client_order_id.assign(client.data(), client.size());
        out.exchange_order_id.assign(order_id.data(), order_id.size());
        out.symbol = std::string(bitget::extract_sv_string(obj, "symbol"));

        auto side = bitget::extract_sv_string(obj, "side");
        out.side = map_side(side);

        // Prefer fill channel fields for last_fill_*.
        const bool is_fill =
            (topic == "fill" || topic == "fast-fill"
             || (!bitget::extract_sv_string(obj, "execQty").empty()
                 || !bitget::extract_sv_number(obj, "execQty").empty()));

        if (is_fill && topic != "order")
        {
            out.last_fill_qty = to_double(
                first_sv(obj, "execQty"));
            out.last_fill_price = to_double(
                first_sv(obj, "execPrice"));
            // Fill channel is per-slice only — do not invent order-level
            // cumulative from execQty. ExecutionBridge accumulates
            // last_fill_qty; leave cumulative_qty at 0 when venue omits it.
            auto cum = first_sv(obj, "cumExecQty");
            out.cumulative_qty = cum.empty() ? 0.0 : to_double(cum);
            out.k = parsed_exec::kind::partial_fill;
            extract_fee(obj, out);
            out.ts = parse_ts(obj, raw);
            return true;
        }

        // Order channel (or bare order object).
        auto status = bitget::extract_sv_string(obj, "orderStatus");
        out.k = classify_order_status(status);
        out.cumulative_qty = to_double(first_sv(obj, "cumExecQty"));

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

        extract_fee(obj, out);

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
