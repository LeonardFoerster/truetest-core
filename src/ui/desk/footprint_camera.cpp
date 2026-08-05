#include "ui/desk/footprint_camera.h"

#include <algorithm>

namespace truetest::ui::desk {

void FootprintCamera::detach_if_following() noexcept
{
    if (state_ == FootprintCameraState::following)
    {
        last_seen_bar_index_ = latest_bar_index_;
        state_ = FootprintCameraState::detached;
    }
}

void FootprintCamera::fit(std::int64_t latest_bar_index,
                          std::int64_t data_time_min_ms, std::int64_t data_time_max_ms,
                          double data_price_min, double data_price_max) noexcept
{
    time_min_ms_ = data_time_min_ms;
    time_max_ms_ = std::max(data_time_max_ms, data_time_min_ms + kMinTimeSpanMs);
    price_min_ = data_price_min;
    price_max_ = std::max(data_price_max, data_price_min + kMinPriceSpan);

    initialized_ = true;
    state_ = FootprintCameraState::following;
    latest_bar_index_ = latest_bar_index;
    last_seen_bar_index_ = latest_bar_index;
}

void FootprintCamera::update_latest(std::int64_t latest_bar_index,
                                    std::int64_t data_time_min_ms, std::int64_t data_time_max_ms,
                                    double data_price_min, double data_price_max) noexcept
{
    if (!initialized_)
    {
        fit(latest_bar_index, data_time_min_ms, data_time_max_ms, data_price_min, data_price_max);
        return;
    }

    latest_bar_index_ = latest_bar_index;

    if (state_ != FootprintCameraState::following)
        return; // frozen where the user left it - only bookkeeping above changed

    const std::int64_t span = std::max(time_max_ms_ - time_min_ms_, kMinTimeSpanMs);
    time_max_ms_ = data_time_max_ms;
    time_min_ms_ = time_max_ms_ - span;
    price_min_ = data_price_min;
    price_max_ = std::max(data_price_max, data_price_min + kMinPriceSpan);
    last_seen_bar_index_ = latest_bar_index;
}

void FootprintCamera::pan(double time_frac, double price_frac) noexcept
{
    detach_if_following();

    const std::int64_t span_t = time_max_ms_ - time_min_ms_;
    const auto delta_t = static_cast<std::int64_t>(time_frac * static_cast<double>(span_t));
    time_min_ms_ += delta_t;
    time_max_ms_ += delta_t;

    const double span_p = price_max_ - price_min_;
    const double delta_p = price_frac * span_p;
    price_min_ += delta_p;
    price_max_ += delta_p;
}

void FootprintCamera::zoom_time(double factor, double anchor_frac) noexcept
{
    if (!(factor > 0.0))
        return;
    detach_if_following();

    const std::int64_t span = time_max_ms_ - time_min_ms_;
    const double anchor_time = static_cast<double>(time_min_ms_) + anchor_frac * static_cast<double>(span);
    const std::int64_t new_span = std::max(
        static_cast<std::int64_t>(static_cast<double>(span) / factor), kMinTimeSpanMs);
    time_min_ms_ = static_cast<std::int64_t>(anchor_time - anchor_frac * static_cast<double>(new_span));
    time_max_ms_ = time_min_ms_ + new_span;
}

void FootprintCamera::zoom_price(double factor, double anchor_frac) noexcept
{
    if (!(factor > 0.0))
        return;
    detach_if_following();

    const double span = price_max_ - price_min_;
    const double anchor_price = price_min_ + anchor_frac * span;
    const double new_span = std::max(span / factor, kMinPriceSpan);
    price_min_ = anchor_price - anchor_frac * new_span;
    price_max_ = price_min_ + new_span;
}

void FootprintCamera::jump_to_start(std::int64_t data_time_min_ms) noexcept
{
    detach_if_following();
    const std::int64_t span = time_max_ms_ - time_min_ms_;
    time_min_ms_ = data_time_min_ms;
    time_max_ms_ = data_time_min_ms + span;
}

void FootprintCamera::go_live(std::int64_t latest_bar_index) noexcept
{
    state_ = FootprintCameraState::following;
    latest_bar_index_ = latest_bar_index;
    last_seen_bar_index_ = latest_bar_index;
}

std::int64_t FootprintCamera::unseen_bars() const noexcept
{
    if (state_ == FootprintCameraState::following)
        return 0;
    return std::max<std::int64_t>(0, latest_bar_index_ - last_seen_bar_index_);
}

} // namespace truetest::ui::desk
