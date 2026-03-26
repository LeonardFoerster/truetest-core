#pragma once
#include "../core/event.h"
#include <optional>
#include <string>
#include <unordered_map>

// Active stop loss / take profit for a position
struct position_stops
{
    double stop_loss = 0.0;     // exit if price falls below (for longs)
    double take_profit = 0.0;   // exit if price rises above (for longs)
    double quantity = 0.0;      // position quantity for the exit order
    bool active = false;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual std::optional<order_event> on_market(const market_event& mkt) = 0;
    virtual std::optional<order_event> on_tick(const tick_event&) { return std::nullopt; }
    virtual std::optional<order_event> on_l2_update(const l2_update_event&) { return std::nullopt; }

    // Per-symbol position tracking
    virtual void set_position_open(const std::string& symbol, bool open) = 0;

    // Legacy single-symbol convenience (delegates to per-symbol with empty string)
    virtual void set_position_open(bool open) { set_position_open("", open); }

    // SL/TP management — engine calls check_stops() on every tick/bar
    void set_stops(const std::string& symbol, double sl, double tp, double qty)
    {
        stops_[symbol] = {sl, tp, qty, true};
    }

    void clear_stops(const std::string& symbol)
    {
        stops_.erase(symbol);
    }

    // Check if current price triggers any SL/TP. Returns exit order if triggered.
    std::optional<order_event> check_stops(
        const std::string& symbol, double price,
        std::chrono::system_clock::time_point ts)
    {
        auto it = stops_.find(symbol);
        if (it == stops_.end() || !it->second.active)
            return std::nullopt;

        auto& s = it->second;

        // Stop loss: price dropped below SL
        if (s.stop_loss > 0.0 && price <= s.stop_loss)
        {
            auto order = order_event(ts, symbol, order_type::market,
                                     order_side::sell, s.quantity, price);
            s.active = false;
            return order;
        }

        // Take profit: price rose above TP
        if (s.take_profit > 0.0 && price >= s.take_profit)
        {
            auto order = order_event(ts, symbol, order_type::market,
                                     order_side::sell, s.quantity, price);
            s.active = false;
            return order;
        }

        return std::nullopt;
    }

    const std::unordered_map<std::string, position_stops>& get_stops() const { return stops_; }

    // Indicator values: strategies can expose their current indicator state
    // for UI display. Called after on_market() to collect values.
    virtual std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& /*symbol*/) const
    {
        return {};
    }

protected:
    std::unordered_map<std::string, position_stops> stops_;
};
