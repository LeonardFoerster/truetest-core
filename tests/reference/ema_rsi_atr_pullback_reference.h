#pragma once

// Deliberately independent EMA/RSI/ATR evaluator for strategy tests. It must
// not include production indicator or strategy headers: agreement is evidence
// for the formula, readiness, and t/t-1 signal provenance rather than a
// second exercise of the same implementation.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <sstream>
#include <string>

namespace ema_rsi_atr_reference {

struct bar
{
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
};

enum class signal
{
    none,
    buy,
    sell
};

struct decision
{
    std::size_t index = 0;
    bool valid_bar = false;
    bool ready = false;
    std::optional<double> ema;
    std::optional<double> rsi_previous;
    std::optional<double> rsi_current;
    std::optional<double> atr;
    bool close_above_ema = false;
    bool close_below_ema = false;
    bool previous_at_or_below_long = false;
    bool current_above_long = false;
    bool previous_at_or_above_short = false;
    bool current_below_short = false;
    signal expected = signal::none;

    std::string evidence() const
    {
        auto number = [](const std::optional<double>& value) {
            return value.has_value() ? std::to_string(*value) : std::string("not-ready");
        };
        std::ostringstream out;
        out << "bar=" << index
            << " valid=" << valid_bar
            << " ready=" << ready
            << " ema[t]=" << number(ema)
            << " rsi[t-1]=" << number(rsi_previous)
            << " rsi[t]=" << number(rsi_current)
            << " atr[t]=" << number(atr)
            << " close>ema=" << close_above_ema
            << " close<ema=" << close_below_ema
            << " prev<=long=" << previous_at_or_below_long
            << " curr>long=" << current_above_long
            << " prev>=short=" << previous_at_or_above_short
            << " curr<short=" << current_below_short
            << " expected=" << (expected == signal::buy ? "buy" : expected == signal::sell ? "sell" : "none");
        return out.str();
    }
};

class evaluator
{
public:
    evaluator(std::size_t ema_period, std::size_t rsi_period, std::size_t atr_period,
              double long_threshold, double short_threshold)
        : ema_(ema_period), rsi_(rsi_period), atr_(atr_period),
          long_threshold_(long_threshold), short_threshold_(short_threshold)
    {}

    decision on_bar(const bar& input)
    {
        decision result;
        result.index = next_index_++;
        result.valid_bar = valid(input);
        if (!result.valid_bar)
            return result;

        result.ema = ema_.update(input.close);
        result.atr = atr_.update(input.high, input.low, input.close);
        result.rsi_previous = previous_rsi_;
        result.rsi_current = rsi_.update(input.close);
        result.ready = result.ema.has_value() && result.atr.has_value() &&
                       result.rsi_previous.has_value() && result.rsi_current.has_value();

        if (result.ready)
        {
            result.close_above_ema = input.close > *result.ema;
            result.close_below_ema = input.close < *result.ema;
            result.previous_at_or_below_long = *result.rsi_previous <= long_threshold_;
            result.current_above_long = *result.rsi_current > long_threshold_;
            result.previous_at_or_above_short = *result.rsi_previous >= short_threshold_;
            result.current_below_short = *result.rsi_current < short_threshold_;

            if (result.close_above_ema && result.previous_at_or_below_long && result.current_above_long)
                result.expected = signal::buy;
            else if (result.close_below_ema && result.previous_at_or_above_short && result.current_below_short)
                result.expected = signal::sell;
        }

        if (result.rsi_current.has_value())
            previous_rsi_ = result.rsi_current;
        return result;
    }

private:
    class reference_ema
    {
    public:
        explicit reference_ema(std::size_t period)
            : period_(period), multiplier_(2.0 / (static_cast<double>(period) + 1.0))
        {}

        std::optional<double> update(double price)
        {
            if (!initialized_)
            {
                sum_ += price;
                ++count_;
                if (count_ != period_)
                    return std::nullopt;
                value_ = sum_ / static_cast<double>(period_);
                initialized_ = true;
                return value_;
            }
            value_ = price * multiplier_ + *value_ * (1.0 - multiplier_);
            return value_;
        }

    private:
        std::size_t period_;
        double multiplier_;
        double sum_ = 0.0;
        std::size_t count_ = 0;
        bool initialized_ = false;
        std::optional<double> value_;
    };

    class reference_rsi
    {
    public:
        explicit reference_rsi(std::size_t period) : period_(period) {}

        std::optional<double> update(double price)
        {
            if (!has_previous_price_)
            {
                previous_price_ = price;
                has_previous_price_ = true;
                return std::nullopt;
            }

            const double change = price - previous_price_;
            previous_price_ = price;
            const double gain = std::max(change, 0.0);
            const double loss = std::max(-change, 0.0);

            if (!initialized_)
            {
                gain_sum_ += gain;
                loss_sum_ += loss;
                ++count_;
                if (count_ != period_)
                    return std::nullopt;
                average_gain_ = gain_sum_ / static_cast<double>(period_);
                average_loss_ = loss_sum_ / static_cast<double>(period_);
                initialized_ = true;
            }
            else
            {
                const double period = static_cast<double>(period_);
                average_gain_ = (average_gain_ * (period - 1.0) + gain) / period;
                average_loss_ = (average_loss_ * (period - 1.0) + loss) / period;
            }

            value_ = average_loss_ == 0.0
                ? 100.0
                : 100.0 - 100.0 / (1.0 + average_gain_ / average_loss_);
            return value_;
        }

    private:
        std::size_t period_;
        double previous_price_ = 0.0;
        bool has_previous_price_ = false;
        double gain_sum_ = 0.0;
        double loss_sum_ = 0.0;
        double average_gain_ = 0.0;
        double average_loss_ = 0.0;
        std::size_t count_ = 0;
        bool initialized_ = false;
        std::optional<double> value_;
    };

    class reference_atr
    {
    public:
        explicit reference_atr(std::size_t period) : period_(period) {}

        std::optional<double> update(double high, double low, double close)
        {
            const double true_range = previous_close_.has_value()
                ? std::max({high - low, std::abs(high - *previous_close_), std::abs(low - *previous_close_)})
                : high - low;

            if (!initialized_)
            {
                ranges_.push_back(true_range);
                range_sum_ += true_range;
                if (ranges_.size() > period_)
                {
                    range_sum_ -= ranges_.front();
                    ranges_.pop_front();
                }
                if (ranges_.size() == period_)
                {
                    value_ = range_sum_ / static_cast<double>(period_);
                    initialized_ = true;
                }
            }
            else
            {
                const double period = static_cast<double>(period_);
                value_ = (*value_ * (period - 1.0) + true_range) / period;
            }

            previous_close_ = close;
            return value_;
        }

    private:
        std::size_t period_;
        std::deque<double> ranges_;
        double range_sum_ = 0.0;
        bool initialized_ = false;
        std::optional<double> previous_close_;
        std::optional<double> value_;
    };

    static bool valid(const bar& input)
    {
        return std::isfinite(input.open) && std::isfinite(input.high) &&
               std::isfinite(input.low) && std::isfinite(input.close) &&
               input.open > 0.0 && input.high > 0.0 && input.low > 0.0 && input.close > 0.0 &&
               input.high >= input.low && input.high >= input.open &&
               input.high >= input.close && input.low <= input.open &&
               input.low <= input.close;
    }

    reference_ema ema_;
    reference_rsi rsi_;
    reference_atr atr_;
    double long_threshold_;
    double short_threshold_;
    std::optional<double> previous_rsi_;
    std::size_t next_index_ = 0;
};

} // namespace ema_rsi_atr_reference
