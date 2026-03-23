#pragma once

#include "event.h"
#include "../execution/portfolio.h"
#include "../analytics/analytics.h"

#include <cstdio>
#include <string>
#include <chrono>

// Simple JSON serialization for engine events.
// No external JSON library — uses snprintf for speed.

namespace event_json {

inline int64_t epoch_ms(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()).count();
}

inline std::string to_json(const market_event& e)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"market","timestamp":%lld,"data":{"symbol":"%s","open":%.6f,"high":%.6f,"low":%.6f,"close":%.6f,"volume":%lld}})",
        static_cast<long long>(epoch_ms(e.get_timestamp())),
        e.get_symbol().c_str(),
        e.get_open(), e.get_high(), e.get_low(), e.get_close(),
        static_cast<long long>(e.get_volume()));
    return buf;
}

inline std::string to_json(const order_event& e)
{
    const char* side_str = (e.get_side() == order_side::buy) ? "buy" : "sell";
    const char* type_str = "market";
    switch (e.get_order_type()) {
    case order_type::market: type_str = "market"; break;
    case order_type::limit:  type_str = "limit"; break;
    case order_type::stop:   type_str = "stop"; break;
    case order_type::stop_limit: type_str = "stop_limit"; break;
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"order","timestamp":%lld,"data":{"order_id":%llu,"symbol":"%s","side":"%s","order_type":"%s","quantity":%.8g,"price":%.6f}})",
        static_cast<long long>(epoch_ms(e.get_timestamp())),
        static_cast<unsigned long long>(e.get_order_id()),
        e.get_symbol().c_str(),
        side_str, type_str,
        e.get_quantity(), e.get_price());
    return buf;
}

inline std::string to_json(const fill_event& e)
{
    const char* side_str = (e.get_side() == order_side::buy) ? "buy" : "sell";

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"fill","timestamp":%lld,"data":{"order_id":%llu,"symbol":"%s","side":"%s","quantity":%.8g,"price":%.6f,"commission":%.6f}})",
        static_cast<long long>(epoch_ms(e.get_timestamp())),
        static_cast<unsigned long long>(e.get_order_id()),
        e.get_symbol().c_str(),
        side_str,
        e.get_filled_quantity(), e.get_fill_price(),
        e.get_commission());
    return buf;
}

inline std::string to_json(const tick_event& e)
{
    const char* side_str = "unknown";
    switch (e.get_side()) {
    case tick_side::bid: side_str = "bid"; break;
    case tick_side::ask: side_str = "ask"; break;
    case tick_side::unknown: side_str = "unknown"; break;
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"tick","timestamp":%lld,"data":{"symbol":"%s","price":%.6f,"quantity":%lld,"side":"%s"}})",
        static_cast<long long>(epoch_ms(e.get_timestamp())),
        e.get_symbol().c_str(),
        e.get_price(),
        static_cast<long long>(e.get_quantity()),
        side_str);
    return buf;
}

inline std::string portfolio_to_json(const portfolio& p)
{
    std::string positions = "[";
    bool first = true;
    for (const auto& [symbol, pos] : p.get_positions())
    {
        if (pos.qty <= 0.0) continue;
        if (!first) positions += ",";
        first = false;

        char pos_buf[256];
        std::snprintf(pos_buf, sizeof(pos_buf),
            R"({"symbol":"%s","qty":%.8g,"cost_basis":%.6f})",
            symbol.c_str(), pos.qty, pos.cost_basis);
        positions += pos_buf;
    }
    positions += "]";

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"portfolio","data":{"cash":%.2f,"total_trades":%zu,"positions":%s}})",
        p.get_cash(),
        p.get_total_trades(),
        positions.c_str());
    return buf;
}

inline std::string analytics_to_json(const AnalyticsReport& r)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"analytics","data":{"initial_equity":%.2f,"final_equity":%.2f,"cumulative_return":%.6f,)"
        R"("sharpe_ratio":%.6f,"sortino_ratio":%.6f,"max_drawdown":%.6f,"calmar_ratio":%.6f,)"
        R"("win_rate":%.6f,"profit_factor":%.6f,"total_trades":%zu,)"
        R"("avg_win":%.6f,"avg_loss":%.6f,"largest_winner":%.6f,"largest_loser":%.6f,)"
        R"("time_in_market_pct":%.4f,"buy_and_hold_return":%.6f}})",
        r.initial_equity, r.final_equity, r.cumulative_return,
        r.sharpe_ratio, r.sortino_ratio, r.max_drawdown, r.calmar_ratio,
        r.win_rate, r.profit_factor, r.total_trades,
        r.avg_win, r.avg_loss, r.largest_winner, r.largest_loser,
        r.time_in_market_pct, r.buy_and_hold_return);
    return buf;
}

// Generic dispatch: serialize any event_pointer to JSON.
// Returns empty string for unhandled event types.
inline std::string event_to_json(const event_pointer& ev)
{
    switch (ev->get_type())
    {
    case event_type::market:
        return to_json(static_cast<const market_event&>(*ev));
    case event_type::order:
        return to_json(static_cast<const order_event&>(*ev));
    case event_type::fill:
        return to_json(static_cast<const fill_event&>(*ev));
    case event_type::tick:
        return to_json(static_cast<const tick_event&>(*ev));
    default:
        return {};
    }
}

} // namespace event_json
