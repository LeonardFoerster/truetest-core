#pragma once
#ifdef HAS_BYBIT

#include "execution/fill_parser.h"
#include "providers/bybit/bybit_parser.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

// Bybit V5 private WS → parsed_exec / parsed_position_snapshot.
// Needle-scan only (no nlohmann). Topics:
//   execution / execution.linear → last_fill_* accuracy
//   order / order.linear         → lifecycle (ack / cancel / status)
//   position / position.linear   → signed qty snapshot
//   wallet                       → balances / funding
class BybitFuturesUserDataParser : public IFillParser
{
public:
    bool parse(std::string_view raw, parsed_exec& out) override
    {
        auto topic = bybit::extract_sv_string(raw, "topic");
        // Normalise "execution.linear" → prefix match.
        const bool is_execution = topic_is(topic, "execution");
        const bool is_order = topic_is(topic, "order");

        if (!topic.empty() && !is_execution && !is_order)
            return false;

        // Funding executions are not order fills — snapshot path.
        auto first = bybit::detail::first_data_object(raw);
        if (first.empty())
        {
            auto data_obj = bybit::detail::extract_object(raw, "data");
            if (!data_obj.empty())
                first = data_obj;
        }
        if (first.empty())
            return false;

        if (is_execution)
        {
            auto et = bybit::extract_sv_string(first, "execType");
            if (et == "Funding" || et == "funding")
                return false;
        }

        // Guard: order/execution payloads carry orderLinkId or orderId.
        auto client = bybit::extract_sv_string(first, "orderLinkId");
        auto order_id = bybit::extract_sv_string(first, "orderId");
        if (order_id.empty())
            order_id = bybit::extract_sv_number(first, "orderId");
        if (client.empty() && order_id.empty())
            return false;

        // Without topic, only accept objects that look like order/execution.
        if (topic.empty())
        {
            auto status = bybit::extract_sv_string(first, "orderStatus");
            auto exec_qty = first_sv(first, "execQty");
            if (status.empty() && exec_qty.empty())
                return false;
        }

        out = parsed_exec{};
        out.client_order_id.assign(client.data(), client.size());
        out.exchange_order_id.assign(order_id.data(), order_id.size());
        out.symbol = std::string(bybit::extract_sv_string(first, "symbol"));

        auto side = bybit::extract_sv_string(first, "side");
        out.side = map_side(side);

        if (is_execution || (!is_order && !first_sv(first, "execQty").empty()))
        {
            out.last_fill_qty = to_double(first_sv(first, "execQty"));
            out.last_fill_price = to_double(first_sv(first, "execPrice"));
            auto cum = first_sv(first, "cumExecQty");
            out.cumulative_qty = cum.empty() ? 0.0 : to_double(cum);

            // leavesQty == 0 → full fill when Trade.
            auto leaves = first_sv(first, "leavesQty");
            if (!leaves.empty() && to_double(leaves) <= 0.0
                && out.last_fill_qty > 0.0)
                out.k = parsed_exec::kind::full_fill;
            else if (out.last_fill_qty > 0.0)
                out.k = parsed_exec::kind::partial_fill;
            else
                out.k = parsed_exec::kind::other;

            auto fee = first_sv(first, "execFee");
            if (fee.empty())
                fee = first_sv(first, "fee");
            if (!fee.empty())
                out.commission = std::abs(to_double(fee));
            auto asset = bybit::extract_sv_string(first, "feeCurrency");
            if (asset.empty())
                asset = bybit::extract_sv_string(first, "feeCoin");
            out.commission_asset.assign(asset.data(), asset.size());

            out.ts = parse_ts(first, raw);
            return true;
        }

        // Order channel (lifecycle).
        auto status = bybit::extract_sv_string(first, "orderStatus");
        out.k = classify_order_status(status);
        out.cumulative_qty = to_double(first_sv(first, "cumExecQty"));

        // Dual-channel safety: order channel carries lifecycle only
        // (last_fill_*=0). Demote partial/full so the bridge does not
        // untrack early — execution topic owns last_fill_qty.
        out.last_fill_qty = 0.0;
        out.last_fill_price = 0.0;
        if ((out.k == parsed_exec::kind::partial_fill
             || out.k == parsed_exec::kind::full_fill)
            && out.last_fill_qty <= 0.0)
            out.k = parsed_exec::kind::other;

        if (out.k == parsed_exec::kind::rejected
            || out.k == parsed_exec::kind::canceled)
        {
            auto err = bybit::extract_sv_string(first, "rejectReason");
            if (err.empty())
                err = bybit::extract_sv_string(first, "cancelType");
            if (err.empty())
                err = bybit::extract_sv_string(first, "retMsg");
            out.error.assign(err.data(), err.size());
        }

        out.ts = parse_ts(first, raw);
        return true;
    }

    bool parse_position_snapshot(std::string_view raw,
                                 parsed_position_snapshot& out) override
    {
        auto topic = bybit::extract_sv_string(raw, "topic");
        const bool is_position = topic_is(topic, "position");
        const bool is_wallet = topic_is(topic, "wallet");
        const bool is_execution = topic_is(topic, "execution");

        // Funding via execution topic (execType=Funding).
        if (is_execution)
        {
            auto first = bybit::detail::first_data_object(raw);
            if (first.empty()) return false;
            auto et = bybit::extract_sv_string(first, "execType");
            if (et != "Funding" && et != "funding")
                return false;

            out = parsed_position_snapshot{};
            out.r = parsed_position_snapshot::reason::funding_fee;
            parsed_position_snapshot::balance_row b;
            b.asset = "USDT";
            auto fee = first_sv(first, "execFee");
            // Funding fee: negative fee means paid (cash decrease).
            b.balance_change = -to_double(fee);
            b.wallet_balance = 0.0;
            out.balances.push_back(std::move(b));
            out.ts = parse_ts(first, raw);
            return true;
        }

        if (!topic.empty() && !is_position && !is_wallet)
            return false;

        auto looks_like_position = [](std::string_view obj) {
            if (obj.empty()) return false;
            if (!bybit::extract_sv_string(obj, "orderStatus").empty())
                return false;
            if (!first_sv(obj, "execQty").empty())
                return false;
            const bool has_side =
                !bybit::extract_sv_string(obj, "side").empty()
                || !bybit::extract_sv_number(obj, "positionIdx").empty();
            const bool has_size =
                !first_sv(obj, "size").empty();
            return has_side || has_size;
        };

        if (is_wallet)
        {
            out = parsed_position_snapshot{};
            out.r = parsed_position_snapshot::reason::other;

            auto arr = bybit::detail::extract_array(raw, "data");
            if (arr.empty())
            {
                auto data_obj = bybit::detail::extract_object(raw, "data");
                if (!data_obj.empty())
                    parse_wallet_object(data_obj, out);
            }
            else
            {
                bybit::detail::for_each_array_object(
                    arr, [&](std::string_view obj) {
                        parse_wallet_object(obj, out);
                    });
            }
            auto ts_sv = first_sv(raw, "ts");
            if (ts_sv.empty())
                ts_sv = first_sv(raw, "creationTime");
            if (!ts_sv.empty())
            {
                int64_t ms = 0;
                if (bybit::parse_int64_sv(ts_sv, ms))
                    out.ts = std::chrono::system_clock::time_point(
                        std::chrono::milliseconds(ms));
            }
            return !out.balances.empty();
        }

        // Position topic / bare position-shaped frames.
        auto arr = bybit::detail::extract_array(raw, "data");
        if (arr.empty())
        {
            auto data_obj = bybit::detail::extract_object(raw, "data");
            std::string_view body =
                !data_obj.empty() ? data_obj
                                  : (is_position ? raw : std::string_view{});
            if (body.empty() && topic.empty() && looks_like_position(raw))
                body = raw;
            if (body.empty())
                return false;
            if (topic.empty() && !looks_like_position(body))
                return false;

            out = parsed_position_snapshot{};
            out.r = is_position ? parsed_position_snapshot::reason::other
                                : parsed_position_snapshot::reason::unknown;
            if (auto row = parse_position_row(body); !row.symbol.empty())
                out.positions.push_back(std::move(row));
            out.ts = parse_ts(body, raw);
            return is_position || !out.positions.empty();
        }

        if (topic.empty())
        {
            std::string_view first;
            bybit::detail::for_each_array_object(arr, [&](std::string_view o) {
                if (first.empty()) first = o;
            });
            if (!looks_like_position(first))
                return false;
        }

        out = parsed_position_snapshot{};
        out.r = is_position ? parsed_position_snapshot::reason::other
                            : parsed_position_snapshot::reason::unknown;

        bybit::detail::for_each_array_object(arr, [&](std::string_view obj) {
            auto row = parse_position_row(obj);
            if (!row.symbol.empty())
                out.positions.push_back(std::move(row));
        });

        auto ts_sv = first_sv(raw, "ts");
        if (!ts_sv.empty())
        {
            int64_t ms = 0;
            if (bybit::parse_int64_sv(ts_sv, ms))
                out.ts = std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(ms));
        }

        return true;
    }

    // long > 0, short < 0. size may be unsigned; side normalizes.
    static double signed_position_qty(std::string_view size_sv,
                                      std::string_view side)
    {
        double size = 0.0;
        if (!size_sv.empty())
            bybit::parse_double_sv(size_sv, size);

        const bool short_side =
            side == "Sell" || side == "sell" || side == "SELL"
            || side == "Short" || side == "short";
        const bool long_side =
            side == "Buy" || side == "buy" || side == "BUY"
            || side == "Long" || side == "long";

        if (short_side)
            return -std::abs(size);
        if (long_side)
            return std::abs(size);
        // None / empty side with zero size → flat.
        return size;
    }

    static parsed_exec::kind classify_order_status(std::string_view status)
    {
        if (status == "New" || status == "Created" || status == "Untriggered")
            return parsed_exec::kind::ack;
        if (status == "PartiallyFilled")
            return parsed_exec::kind::partial_fill;
        if (status == "Filled")
            return parsed_exec::kind::full_fill;
        if (status == "Cancelled" || status == "Canceled")
            return parsed_exec::kind::canceled;
        if (status == "Rejected")
            return parsed_exec::kind::rejected;
        if (status == "Deactivated" || status == "Triggered")
        {
            // Deactivated ≈ expired for conditionals; Triggered is mid-state.
            if (status == "Deactivated")
                return parsed_exec::kind::expired;
            return parsed_exec::kind::other;
        }
        return parsed_exec::kind::other;
    }

    // positionIdx: 0 → BOTH, 1 → LONG, 2 → SHORT
    static std::string position_side_from_idx(std::string_view idx)
    {
        if (idx == "1") return "LONG";
        if (idx == "2") return "SHORT";
        return "BOTH";
    }

    static std::string canonical_margin_mode(std::string_view mode)
    {
        if (mode.empty()) return {};
        char c0 = static_cast<char>(
            std::tolower(static_cast<unsigned char>(mode[0])));
        // Bybit: 0 / CROSS / cross → CROSSED; 1 / ISOLATED → ISOLATED
        if (c0 == 'c' || mode == "0")
            return "CROSSED";
        if (c0 == 'i' || mode == "1")
            return "ISOLATED";
        return std::string(mode);
    }

private:
    static bool topic_is(std::string_view topic, std::string_view prefix)
    {
        if (topic.empty() || prefix.empty()) return false;
        if (topic == prefix) return true;
        if (topic.size() > prefix.size()
            && topic.substr(0, prefix.size()) == prefix
            && topic[prefix.size()] == '.')
            return true;
        return false;
    }

    static double to_double(std::string_view sv)
    {
        if (sv.empty()) return 0.0;
        double v = 0.0;
        if (bybit::parse_double_sv(sv, v))
            return v;
        return 0.0;
    }

    static std::string_view first_sv(std::string_view obj, std::string_view key)
    {
        auto s = bybit::extract_sv_string(obj, key);
        if (!s.empty()) return s;
        return bybit::extract_sv_number(obj, key);
    }

    static order_side map_side(std::string_view side)
    {
        if (side == "Sell" || side == "sell" || side == "SELL")
            return order_side::sell;
        return order_side::buy;
    }

    static std::chrono::system_clock::time_point
    parse_ts(std::string_view obj, std::string_view wrapper)
    {
        for (const char* key : {"execTime", "updatedTime", "createdTime",
                                "ts", "T"})
        {
            auto sv = first_sv(obj, key);
            if (sv.empty() && (key[0] == 't' || key[0] == 'T'))
                sv = first_sv(wrapper, "ts");
            if (sv.empty())
                continue;
            int64_t ms = 0;
            if (bybit::parse_int64_sv(sv, ms))
                return std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(ms));
        }
        return {};
    }

    static parsed_position_snapshot::position_row
    parse_position_row(std::string_view obj)
    {
        parsed_position_snapshot::position_row row;
        auto sym = bybit::extract_sv_string(obj, "symbol");
        row.symbol.assign(sym.data(), sym.size());

        auto size_sv = first_sv(obj, "size");
        auto side = bybit::extract_sv_string(obj, "side");
        row.qty = signed_position_qty(size_sv, side);

        auto idx = first_sv(obj, "positionIdx");
        row.position_side = position_side_from_idx(idx);

        auto mm = bybit::extract_sv_string(obj, "tradeMode");
        if (mm.empty())
            mm = bybit::extract_sv_string(obj, "marginMode");
        if (mm.empty())
            mm = first_sv(obj, "tradeMode");
        row.margin_type = canonical_margin_mode(mm);
        return row;
    }

    static void parse_wallet_object(std::string_view obj,
                                    parsed_position_snapshot& out)
    {
        // Wallet push: coin[] array under each account row, or flat coin fields.
        auto coins = bybit::detail::extract_array(obj, "coin");
        if (!coins.empty())
        {
            bybit::detail::for_each_array_object(
                coins, [&](std::string_view c) {
                    parsed_position_snapshot::balance_row b;
                    auto coin = bybit::extract_sv_string(c, "coin");
                    b.asset.assign(coin.data(), coin.size());
                    auto bal = first_sv(c, "walletBalance");
                    if (bal.empty())
                        bal = first_sv(c, "equity");
                    if (bal.empty())
                        bal = first_sv(c, "availableToWithdraw");
                    b.wallet_balance = to_double(bal);
                    if (!b.asset.empty() || b.wallet_balance != 0.0)
                        out.balances.push_back(std::move(b));
                });
            return;
        }

        // Flat single-coin object.
        parsed_position_snapshot::balance_row b;
        auto coin = bybit::extract_sv_string(obj, "coin");
        if (coin.empty())
            coin = bybit::extract_sv_string(obj, "currency");
        b.asset.assign(coin.data(), coin.size());
        auto bal = first_sv(obj, "walletBalance");
        if (bal.empty())
            bal = first_sv(obj, "totalEquity");
        if (bal.empty())
            bal = first_sv(obj, "totalAvailableBalance");
        b.wallet_balance = to_double(bal);
        if (!b.asset.empty() || b.wallet_balance != 0.0)
            out.balances.push_back(std::move(b));
    }
};

#endif // HAS_BYBIT
