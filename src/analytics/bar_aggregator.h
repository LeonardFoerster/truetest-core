#pragma once

#include "../core/event.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

class BarAggregator
{
public:
    using bar_callback = std::function<void(const market_event&)>;

    explicit BarAggregator(std::chrono::milliseconds interval, bar_callback cb,
                           std::size_t max_symbols = 16)
        : interval_(interval), callback_(std::move(cb)),
          max_symbols_(max_symbols)
    {
        states_.reserve(max_symbols_);
    }

    // Emits exactly one immutable bar per completed interval (plus the
    // final partial bar via flush()). Emission depends only on event-time
    // timestamps — never on wall-clock time — so the same tick sequence
    // always produces the same bars regardless of host speed. Consumers
    // feed each emission to the strategy, so duplicate/partial emissions
    // would distort indicator state and break determinism.
    bool on_tick(const std::string& symbol, double price, int64_t volume,
                 std::chrono::system_clock::time_point timestamp,
                 std::uint64_t quantity_scale = 1)
    {
        if (symbol.empty() || !std::isfinite(price) || !(price > 0.0)
            || volume < 0 || quantity_scale == 0)
            return false;

        bar_state* state = nullptr;
        for (auto& candidate : states_)
        {
            if (candidate.symbol == symbol)
            {
                state = &candidate;
                break;
            }
        }
        if (!state)
        {
            if (states_.size() >= max_symbols_)
                return false;
            states_.push_back({});
            state = &states_.back();
            state->symbol = symbol;
        }
        auto& s = *state;

        if (!s.bar_open)
        {
            s.bar_start = timestamp;
            s.open = s.high = s.low = s.close = price;
            s.volume = volume;
            s.quantity_scale = quantity_scale;
            s.bar_open = true;
            return true;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp - s.bar_start);
        if (elapsed >= interval_)
        {
            emit_bar(s);
            s.bar_start = timestamp;
            s.open = s.high = s.low = s.close = price;
            s.volume = volume;
            s.quantity_scale = quantity_scale;
            return true;
        }

        int64_t next_volume = s.volume;
        if (quantity_scale == s.quantity_scale)
        {
            if (s.volume > std::numeric_limits<int64_t>::max() - volume)
                return false;
            next_volume += volume;
        }
        else
        {
            const long double normalized = static_cast<long double>(volume)
                * static_cast<long double>(s.quantity_scale)
                / static_cast<long double>(quantity_scale);
            const long double max_add = static_cast<long double>(
                std::numeric_limits<int64_t>::max()) - s.volume;
            if (!(normalized >= 0.0L) || normalized > max_add)
                return false;
            next_volume += static_cast<int64_t>(std::llround(normalized));
        }

        s.high = std::max(s.high, price);
        s.low = std::min(s.low, price);
        s.close = price;
        s.volume = next_volume;
        return true;
    }

    void flush()
    {
        // A symbol can roll into a newer interval while another symbol still
        // has an older partial bar.  Emit all remaining bars in event-time
        // order, not symbol first-touch order, so Analytics never observes a
        // timestamp reversal at EOS.  Symbol is the deterministic tie-break.
        std::sort(states_.begin(), states_.end(),
                  [](const bar_state& lhs, const bar_state& rhs) {
                      if (lhs.bar_open != rhs.bar_open)
                          return lhs.bar_open > rhs.bar_open;
                      if (lhs.bar_start != rhs.bar_start)
                          return lhs.bar_start < rhs.bar_start;
                      return lhs.symbol < rhs.symbol;
                  });
        for (auto& state : states_)
            if (state.bar_open)
            {
                emit_bar(state);
                state.bar_open = false;
            }
    }

    // Phase B (MC object reuse): resets internal bar state so the aggregator
    // can be reused across trials without leaking partial bars.
    void reset()
    {
        states_.clear();
    }

private:
    struct bar_state
    {
        bool bar_open = false;
        std::chrono::system_clock::time_point bar_start{};
        std::string symbol;
        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        double close = 0.0;
        int64_t volume = 0;
        std::uint64_t quantity_scale = 1;
    };

    void emit_bar(bar_state& state)
    {
        market_event bar(state.bar_start, state.symbol, state.open,
                         state.high, state.low, state.close, state.volume,
                         state.quantity_scale);
        callback_(bar);
    }

    std::chrono::milliseconds interval_;
    bar_callback callback_;

    std::size_t max_symbols_ = 16;
    std::vector<bar_state> states_;
};
