#pragma once
#ifdef HAS_BITGET

#include "execution/fill_parser.h"
#include "providers/bitget/bitget_parser.h"

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
            // Single fill slice — cumulative unknown on this channel.
            out.cumulative_qty = out.last_fill_qty;
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

        // Dual-channel safety: order-channel partially_filled with no
        // incremental qty is lifecycle noise — demote to other so the
        // bridge never sees a zero-qty partial. Keep filled as full_fill
        // so ExecutionBridge can untrack (it skips zero-qty fill emit).
        if (out.k == parsed_exec::kind::partial_fill
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

        // Explicit non-position private topics must not become snapshots.
        if (!topic.empty() && topic != "position")
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
            out.r = parsed_position_snapshot::reason::order;
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
            if (!looks_like_position(first))
                return false;
        }

        out = parsed_position_snapshot{};
        out.r = parsed_position_snapshot::reason::order;

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
