#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct swing_point
{
    double      price     = 0.0;
    std::size_t bar_index = 0;
    int         strength  = 0;   // bars on each side that confirmed this pivot
};

enum class structure_phase : int
{
    unknown   = 0,
    uptrend   = 1,   // series of Higher Highs + Higher Lows
    downtrend = 2,   // series of Lower Highs + Lower Lows
    ranging   = 3
};

struct structure_snapshot
{
    structure_phase phase = structure_phase::unknown;

    std::optional<swing_point> last_swing_high;
    std::optional<swing_point> last_swing_low;

    std::optional<double> last_higher_high;   // price of most recent confirmed HH
    std::optional<double> last_lower_low;     // price of most recent confirmed LL

    std::size_t bars_since_last_swing_high = 0;
    std::size_t bars_since_last_swing_low  = 0;
    std::size_t bars_since_last_bos        = SIZE_MAX;  // SIZE_MAX = never observed

    int trend_direction = 0;   // +1 uptrend bias, -1 downtrend, 0 ranging/unknown

    std::size_t bos_count   = 0;
    std::size_t choch_count = 0;   // reserved for future

    // Small history of recent confirmed pivots (newest last)
    std::vector<swing_point> recent_pivots;
};

/**
 * Swing / Market Structure Detector (middle variant).
 *
 * Responsibilities:
 * - Detect confirmed swing highs and lows using configurable strength (left/right bars).
 * - Track the sequence of swings and identify Higher Highs / Lower Lows.
 * - Maintain current structure phase (uptrend / downtrend / ranging).
 * - Detect Break of Structure (BOS).
 * - Provide a small history of recent pivots.
 * - Offer helpers for regime classification (e.g. swing range < ATR).
 *
 * Design goals:
 * - Header-only, lightweight, allocation-light after warmup.
 * - Excellent Monte-Carlo support via reset().
 * - Rich but cheap observability via snapshot() and get_indicator_values().
 * - No trading rules inside (body strength, volume, etc.).
 */
class swing_detector
{
public:
    /**
     * strength = number of bars required on each side of a potential pivot.
     * Example: strength=2 means a high is a swing high only if it is strictly
     * higher than the 2 bars before and the 2 bars after.
     */
    explicit swing_detector(std::size_t strength = 2,
                            std::size_t max_history = 32);

    /**
     * Feed one completed bar.
     * Must be called strictly sequentially with increasing bar data.
     */
    void update(double high, double low, double close);

    bool ready() const;

    structure_phase phase() const;
    structure_snapshot snapshot() const;

    // Convenience accessors (hot path friendly)
    std::optional<double> last_higher_high_price() const;
    std::optional<double> last_lower_low_price() const;

    std::optional<swing_point> last_confirmed_swing_high() const;
    std::optional<swing_point> last_confirmed_swing_low() const;

    std::size_t bars_since_last_bos() const;

    // Helper for the user's sideways rule: "Range der letzten N Swings < ATR"
    bool is_sideways_by_swing_range(std::size_t n, double atr) const;
    double recent_swing_range(std::size_t n) const
    {
        return last_n_swings_range(n);
    }

    // Monte Carlo object reuse (landed with monte-carlo merge)
    void reset();

    // Diagnostics
    std::size_t strength() const;
    std::size_t bar_count() const;   // total bars fed since construction or last reset

    // Exposure for TUI, analytics, MC reports (consistent with other indicators)
    std::vector<std::pair<std::string, double>> get_indicator_values() const;

private:
    std::size_t strength_;
    std::size_t max_history_;

    std::size_t bar_count_ = 0;

    // Rolling buffers for pivot detection (we keep 1 + 2*strength_ bars)
    std::deque<double> recent_highs_;
    std::deque<double> recent_lows_;

    // Confirmed pivots (newest at back). Bounded by max_history_.
    std::deque<swing_point> confirmed_pivots_;

    // Current structure state
    structure_snapshot state_{};

    // Internal helpers
    bool try_confirm_pivot();
    void update_structure_after_new_pivot(const swing_point& new_pivot, bool is_high);
    void trim_history();

    static bool is_strictly_higher(const std::deque<double>& window, std::size_t center, std::size_t strength);
    static bool is_strictly_lower (const std::deque<double>& window, std::size_t center, std::size_t strength);

    // Update the "bars since" counters (called every bar)
    void increment_bars_since_counters();

    // Compute range of the last n confirmed swing prices (highs and lows mixed)
    double last_n_swings_range(std::size_t n) const;
};

// -----------------------------------------------------------------------------
// Implementation (header-only, consistent with ema.h / stochastic.h style)
// -----------------------------------------------------------------------------

inline swing_detector::swing_detector(std::size_t strength, std::size_t max_history)
    : strength_(strength)
    , max_history_(max_history)
{
    // We need 1 + 2*strength bars in the rolling window to confirm a pivot
    // (center bar + strength on each side)
    const std::size_t window_size = 1 + 2 * strength_;
    recent_highs_.resize(window_size, 0.0);
    recent_lows_.resize(window_size, 0.0);
}

inline void swing_detector::update(double high, double low, double /*close*/)
{
    ++bar_count_;

    // Shift rolling windows
    if (recent_highs_.size() > 0)
    {
        recent_highs_.pop_front();
        recent_lows_.pop_front();
    }
    recent_highs_.push_back(high);
    recent_lows_.push_back(low);

    increment_bars_since_counters();

    // Try to confirm a new pivot (the potential pivot is at position 'strength_' in the current window)
    if (try_confirm_pivot())
    {
        // A pivot was just confirmed and appended inside try_confirm_pivot
    }

    trim_history();
}

inline bool swing_detector::ready() const
{
    // We need at least one confirmed pivot and enough bars to have a meaningful structure
    return !confirmed_pivots_.empty() && bar_count_ > (1 + 2 * strength_);
}

inline structure_phase swing_detector::phase() const
{
    return state_.phase;
}

inline structure_snapshot swing_detector::snapshot() const
{
    structure_snapshot snap = state_;
    // Fill recent_pivots from the bounded deque (newest last)
    snap.recent_pivots.clear();
    snap.recent_pivots.reserve(confirmed_pivots_.size());
    for (const auto& p : confirmed_pivots_)
        snap.recent_pivots.push_back(p);
    return snap;
}

inline std::optional<double> swing_detector::last_higher_high_price() const
{
    return state_.last_higher_high;
}

inline std::optional<double> swing_detector::last_lower_low_price() const
{
    return state_.last_lower_low;
}

inline std::optional<swing_point> swing_detector::last_confirmed_swing_high() const
{
    return state_.last_swing_high;
}

inline std::optional<swing_point> swing_detector::last_confirmed_swing_low() const
{
    return state_.last_swing_low;
}

inline std::size_t swing_detector::bars_since_last_bos() const
{
    return state_.bars_since_last_bos;
}

inline bool swing_detector::is_sideways_by_swing_range(std::size_t n, double atr) const
{
    if (atr <= 0.0) return false;
    double rng = last_n_swings_range(n);
    return rng > 0.0 && rng < atr;
}

inline void swing_detector::reset()
{
    bar_count_ = 0;

    const std::size_t window_size = 1 + 2 * strength_;
    recent_highs_.assign(window_size, 0.0);
    recent_lows_.assign(window_size, 0.0);

    confirmed_pivots_.clear();

    state_ = structure_snapshot{};
    // Ensure sane initial values
    state_.bars_since_last_bos = SIZE_MAX;
}

inline std::size_t swing_detector::strength() const { return strength_; }
inline std::size_t swing_detector::bar_count() const { return bar_count_; }

inline std::vector<std::pair<std::string, double>> swing_detector::get_indicator_values() const
{
    std::vector<std::pair<std::string, double>> vals;
    vals.reserve(12);

    vals.emplace_back("swing_phase", static_cast<double>(static_cast<int>(state_.phase)));
    vals.emplace_back("swing_trend_dir", static_cast<double>(state_.trend_direction));
    vals.emplace_back("swing_bars_since_bos", static_cast<double>(state_.bars_since_last_bos));

    if (auto hh = last_higher_high_price())
        vals.emplace_back("swing_last_hh", *hh);
    if (auto ll = last_lower_low_price())
        vals.emplace_back("swing_last_ll", *ll);

    vals.emplace_back("swing_pivot_count", static_cast<double>(confirmed_pivots_.size()));
    vals.emplace_back("swing_bos_count", static_cast<double>(state_.bos_count));

    return vals;
}

// -----------------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------------

inline bool swing_detector::try_confirm_pivot()
{
    const std::size_t center = strength_;
    if (recent_highs_.size() < 1 + 2 * strength_)
        return false;

    bool made_high = false;
    bool made_low  = false;

    if (is_strictly_higher(recent_highs_, center, strength_))
    {
        swing_point p;
        p.price     = recent_highs_[center];
        p.bar_index = (bar_count_ > strength_) ? (bar_count_ - strength_) : 0;
        p.strength  = static_cast<int>(strength_);

        confirmed_pivots_.push_back(p);
        update_structure_after_new_pivot(p, true);
        state_.last_swing_high = p;
        state_.bars_since_last_swing_high = 0;
        made_high = true;
    }

    if (is_strictly_lower(recent_lows_, center, strength_))
    {
        swing_point p;
        p.price     = recent_lows_[center];
        p.bar_index = (bar_count_ > strength_) ? (bar_count_ - strength_) : 0;
        p.strength  = static_cast<int>(strength_);

        confirmed_pivots_.push_back(p);
        update_structure_after_new_pivot(p, false);
        state_.last_swing_low = p;
        state_.bars_since_last_swing_low = 0;
        made_low = true;
    }

    return made_high || made_low;
}

inline void swing_detector::update_structure_after_new_pivot(const swing_point& new_pivot,
                                                             bool is_high)
{
    if (is_high)
    {
        // We just confirmed a new swing high
        bool is_higher = false;
        const bool had_prior_structure_high = state_.last_higher_high.has_value();
        if (state_.last_higher_high.has_value())
        {
            if (new_pivot.price > *state_.last_higher_high)
                is_higher = true;
        }
        else
        {
            is_higher = true; // first one
        }

        if (is_higher)
        {
            state_.last_higher_high = new_pivot.price;

            if (had_prior_structure_high ||
                state_.phase == structure_phase::downtrend ||
                state_.phase == structure_phase::ranging)
            {
                ++state_.bos_count;
                state_.bars_since_last_bos = 0;
                state_.phase = structure_phase::uptrend;
                state_.trend_direction = 1;
            }
            else if (state_.phase == structure_phase::unknown)
            {
                state_.phase = structure_phase::uptrend;
                state_.trend_direction = 1;
            }
        }
    }
    else
    {
        // We just confirmed a new swing low
        bool is_lower = false;
        const bool had_prior_structure_low =
            state_.last_lower_low.has_value() || state_.last_swing_low.has_value();
        if (state_.last_lower_low.has_value())
        {
            if (new_pivot.price < *state_.last_lower_low)
                is_lower = true;
        }
        else
        {
            // No previous LL yet — use the most recent swing low as reference
            // (important for breaking the first pullback low in an uptrend)
            if (state_.last_swing_low.has_value())
            {
                if (new_pivot.price < state_.last_swing_low->price)
                    is_lower = true;
            }
            else
            {
                is_lower = true; // first swing low ever
            }
        }

        if (is_lower)
        {
            state_.last_lower_low = new_pivot.price;

            if (had_prior_structure_low ||
                state_.phase == structure_phase::uptrend ||
                state_.phase == structure_phase::ranging)
            {
                ++state_.bos_count;
                state_.bars_since_last_bos = 0;
                state_.phase = structure_phase::downtrend;
                state_.trend_direction = -1;
            }
            else if (state_.phase == structure_phase::unknown)
            {
                state_.phase = structure_phase::downtrend;
                state_.trend_direction = -1;
            }
        }
    }
}

inline void swing_detector::increment_bars_since_counters()
{
    if (state_.bars_since_last_swing_high != SIZE_MAX)
        ++state_.bars_since_last_swing_high;
    if (state_.bars_since_last_swing_low != SIZE_MAX)
        ++state_.bars_since_last_swing_low;
    if (state_.bars_since_last_bos != SIZE_MAX)
        ++state_.bars_since_last_bos;
}

inline void swing_detector::trim_history()
{
    while (confirmed_pivots_.size() > max_history_)
        confirmed_pivots_.pop_front();
}

inline bool swing_detector::is_strictly_higher(const std::deque<double>& window,
                                               std::size_t center,
                                               std::size_t strength)
{
    if (center >= window.size()) return false;
    const double val = window[center];
    for (std::size_t i = center - strength; i <= center + strength; ++i)
    {
        if (i == center) continue;
        if (window[i] >= val) return false;
    }
    return true;
}

inline bool swing_detector::is_strictly_lower(const std::deque<double>& window,
                                              std::size_t center,
                                              std::size_t strength)
{
    if (center >= window.size()) return false;
    const double val = window[center];
    for (std::size_t i = center - strength; i <= center + strength; ++i)
    {
        if (i == center) continue;
        if (window[i] <= val) return false;
    }
    return true;
}

inline double swing_detector::last_n_swings_range(std::size_t n) const
{
    if (confirmed_pivots_.size() < n || n == 0) return 0.0;

    auto it = confirmed_pivots_.rbegin();
    double mn = it->price;
    double mx = it->price;
    ++it;

    for (std::size_t i = 1; i < n && it != confirmed_pivots_.rend(); ++i, ++it)
    {
        mn = std::min(mn, it->price);
        mx = std::max(mx, it->price);
    }
    return mx - mn;
}
