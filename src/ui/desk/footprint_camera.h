#pragma once

#include <cstdint>

// Retained camera for the footprint canvas. footprint.md §2.3: drag to pan,
// wheel for time zoom, Ctrl+wheel for price zoom, double-click to fit,
// Home/End and -/+; manual interaction detaches from live and counts unseen
// bars until an explicit "GO LIVE" - it never snaps back on its own.
//
// Deliberately ImGui-free and pure: the panel converts pixel-space mouse
// deltas/anchors into the fractional units this class takes, so the camera
// itself is fully unit-testable without a display. Time is int64
// milliseconds (matches FootprintBarView::start_ms); price is double.
namespace truetest::ui::desk {

enum class FootprintCameraState : std::uint8_t
{
    following, // viewport rides the latest bar; price/time auto-fit each frame
    detached,  // frozen where the user left it; only go_live() resumes following
};

class FootprintCamera
{
public:
    FootprintCameraState state() const noexcept { return state_; }
    bool initialized() const noexcept { return initialized_; }

    std::int64_t time_min_ms() const noexcept { return time_min_ms_; }
    std::int64_t time_max_ms() const noexcept { return time_max_ms_; }
    double price_min() const noexcept { return price_min_; }
    double price_max() const noexcept { return price_max_; }

    // "Double-click to fit": viewport becomes exactly the given data bounds
    // (full range, not the current zoom span) and the camera returns to
    // FOLLOWING with latest_bar_index marked fully seen.
    void fit(std::int64_t latest_bar_index,
             std::int64_t data_time_min_ms, std::int64_t data_time_max_ms,
             double data_price_min, double data_price_max) noexcept;

    // Call once per frame with the current data bounds. While FOLLOWING,
    // preserves the current zoom span but slides the time window so its
    // right edge tracks data_time_max_ms, and auto-fits price to the given
    // bounds; latest_bar_index is marked seen (unseen_bars() stays 0).
    // While DETACHED, the viewport is untouched - only unseen_bars()
    // bookkeeping updates. Never auto-fits before the first fit()/go_live().
    void update_latest(std::int64_t latest_bar_index,
                        std::int64_t data_time_min_ms, std::int64_t data_time_max_ms,
                        double data_price_min, double data_price_max) noexcept;

    // Manual interaction - drag to pan. Fractions are relative to the
    // CURRENT viewport span (e.g. pixel_dx / canvas_width_px), so this
    // class needs no pixel/DPI knowledge. Positive time_frac moves the
    // window later; positive price_frac moves it up. Enters DETACHED.
    void pan(double time_frac, double price_frac) noexcept;

    // Wheel for time zoom (factor > 1 zooms in / narrows the window; < 1
    // zooms out). anchor_frac in [0,1] is the point to keep fixed, as a
    // fraction across the CURRENT visible time window (e.g. mouse x
    // fraction). Enters DETACHED.
    void zoom_time(double factor, double anchor_frac) noexcept;

    // Ctrl+wheel for price zoom; anchor_frac in [0,1], bottom to top of the
    // current visible price window. Enters DETACHED.
    void zoom_price(double factor, double anchor_frac) noexcept;

    // Home: pan to the start of available data, preserving the current
    // zoom span. Enters DETACHED (Home is a manual move, not "go live").
    void jump_to_start(std::int64_t data_time_min_ms) noexcept;

    // "GO LIVE" (also End): resume FOLLOWING at the CURRENT zoom span
    // (unlike fit(), this does not reset to the full data range) and mark
    // latest_bar_index seen. The next update_latest() call snaps the
    // viewport to the live edge.
    void go_live(std::int64_t latest_bar_index) noexcept;

    // Bars that closed after the camera detached; 0 while FOLLOWING.
    std::int64_t unseen_bars() const noexcept;

private:
    void detach_if_following() noexcept;

    FootprintCameraState state_ = FootprintCameraState::following;
    bool initialized_ = false;

    std::int64_t time_min_ms_ = 0;
    std::int64_t time_max_ms_ = 0;
    double price_min_ = 0.0;
    double price_max_ = 1.0;

    std::int64_t latest_bar_index_ = 0;
    std::int64_t last_seen_bar_index_ = 0;

    static constexpr std::int64_t kMinTimeSpanMs = 1'000;
    static constexpr double kMinPriceSpan = 1e-9;
};

} // namespace truetest::ui::desk
