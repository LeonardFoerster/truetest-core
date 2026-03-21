#pragma once

#include "../core/event.h"

#include <chrono>
#include <functional>
#include <limits>

// Accumulates tick events into OHLCV bars at a configurable interval.
// Outputs completed bars as market_event via a callback, allowing bar-based
// strategies to run unmodified on tick feeds.
class BarAggregator
{
public:
    using bar_callback = std::function<void(const market_event&)>;

    explicit BarAggregator(std::chrono::milliseconds interval, bar_callback cb)
        : interval_(interval), callback_(std::move(cb)) {}

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

        // Check if this tick belongs to the next bar
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

    // Flush any partial bar (call at end of data)
    void flush()
    {
        if (bar_open_)
            emit_bar();
    }

private:
    void emit_bar()
    {
        market_event bar(bar_start_, symbol_, open_, high_, low_, close_, volume_);
        callback_(bar);
        bar_open_ = false;
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
