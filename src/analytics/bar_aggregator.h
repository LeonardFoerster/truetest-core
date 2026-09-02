#pragma once

#include "../core/event.h"
#include "../types/quantity_scale.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
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
            || volume <= 0 || quantity_scale == 0
            || interval_.count() <= 0
            || timestamp.time_since_epoch().count() <= 0
            || (last_input_timestamp_
                && timestamp < *last_input_timestamp_))
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
        }

        // Validate every mutation that can still fail before advancing global
        // event time or publishing completed bars. A rejected tick must not
        // close quiet-symbol bars or move the replay watermark.
        int64_t next_volume = volume;
        const bool adds_to_open_bar =
            state && state->bar_open && timestamp - state->bar_start < interval_;
        if (adds_to_open_bar) {
            next_volume = state->volume;
            if (quantity_scale == state->quantity_scale) {
                if (state->volume > std::numeric_limits<int64_t>::max() - volume)
                    return false;
                next_volume += volume;
            } else {
                std::uint64_t normalized = 0;
                if (!tt::quantity_scale::rescale_nonnegative_exact(
                        volume, quantity_scale, state->quantity_scale,
                        normalized)
                    || normalized > static_cast<std::uint64_t>(
                        std::numeric_limits<int64_t>::max() - state->volume))
                    return false;
                next_volume += static_cast<int64_t>(normalized);
            }
        }

        std::optional<bar_state> new_state;
        if (!state) {
            new_state.emplace();
            new_state->symbol = symbol;
        }

        // A quiet symbol's completed bar must be emitted before a newer
        // symbol's bar. Close every interval that is knowably complete at the
        // current global event time, in deterministic start/symbol order.
        emit_due(timestamp);

        if (!state) {
            states_.push_back(std::move(*new_state));
            state = &states_.back();
        }
        auto& s = *state;

        if (!s.bar_open) {
            s.bar_start = timestamp;
            s.open = s.high = s.low = s.close = price;
            s.volume = volume;
            s.quantity_scale = quantity_scale;
            s.bar_open = true;
        } else {
            s.high = std::max(s.high, price);
            s.low = std::min(s.low, price);
            s.close = price;
            s.volume = next_volume;
        }

        last_input_timestamp_ = timestamp;
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
        last_input_timestamp_.reset();
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

    void emit_due(std::chrono::system_clock::time_point now)
    {
        while (true)
        {
            bar_state* next = nullptr;
            for (auto& state : states_)
            {
                if (!state.bar_open || now - state.bar_start < interval_)
                    continue;
                if (!next || state.bar_start < next->bar_start
                    || (state.bar_start == next->bar_start
                        && state.symbol < next->symbol))
                    next = &state;
            }
            if (!next) return;
            emit_bar(*next);
            next->bar_open = false;
        }
    }

    std::chrono::milliseconds interval_;
    bar_callback callback_;

    std::size_t max_symbols_ = 16;
    std::vector<bar_state> states_;
    std::optional<std::chrono::system_clock::time_point> last_input_timestamp_;
};
