#pragma once

#include "../core/event.h"

#include <chrono>
#include <functional>
#include <limits>

class BarAggregator
{
public:
    using bar_callback = std::function<void(const market_event&)>;

    explicit BarAggregator(std::chrono::milliseconds interval, bar_callback cb)
        : interval_(interval), callback_(std::move(cb)) {}

    // Emits exactly one immutable bar per completed interval (plus the
    // final partial bar via flush()). Emission depends only on event-time
    // timestamps — never on wall-clock time — so the same tick sequence
    // always produces the same bars regardless of host speed. Consumers
    // feed each emission to the strategy, so duplicate/partial emissions
    // would distort indicator state and break determinism.
    void on_tick(const std::string& symbol, double price, int64_t volume,
                 std::chrono::system_clock::time_point timestamp)
    {
        if (!bar_open_)
        {
            bar_start_ = timestamp;
            open_ = high_ = low_ = close_ = price;
            volume_ = volume;
            symbol_ = symbol;
            bar_open_ = true;
            return;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - bar_start_);
        if (elapsed >= interval_)
        {
            emit_bar();
            bar_start_ = timestamp;
            open_ = high_ = low_ = close_ = price;
            volume_ = volume;
            symbol_ = symbol;
            return;
        }

        high_ = std::max(high_, price);
        low_ = std::min(low_, price);
        close_ = price;
        volume_ += volume;
    }

    void flush()
    {
        if (bar_open_)
            emit_bar();
    }

    // Phase B (MC object reuse): resets internal bar state so the aggregator
    // can be reused across trials without leaking partial bars.
    void reset()
    {
        bar_open_ = false;
        bar_start_ = {};
        symbol_.clear();
        open_ = high_ = low_ = close_ = 0.0;
        volume_ = 0;
    }

private:
    void emit_bar()
    {
        market_event bar(bar_start_, symbol_, open_, high_, low_, close_, volume_);
        callback_(bar);
    }

    std::chrono::milliseconds interval_;
    bar_callback callback_;

    bool bar_open_ = false;
    std::chrono::system_clock::time_point bar_start_;
    std::string symbol_;
    double open_ = 0.0;
    double high_ = 0.0;
    double low_ = 0.0;
    double close_ = 0.0;
    int64_t volume_ = 0;
};
