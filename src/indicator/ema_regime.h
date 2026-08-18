#pragma once

#include "atr.h"
#include "swing_detector.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class ema_regime : int
{
    unknown     = 0,
    sideways    = 1,   // swing range of last N < ATR (user rule) or strong EMA contraction
    contracted  = 2,   // EMAs close together
    y_expanding = 3,   // Y-förmig expansion after contraction/sideways
    wide        = 4    // caution: EMAs too far apart
};

struct ema_regime_snapshot
{
    ema_regime  regime = ema_regime::unknown;

    double ema_fast      = 0.0;
    double ema_slow      = 0.0;
    double distance_abs  = 0.0;
    double distance_pct  = 0.0;   // |fast-slow| / slow * 100

    bool is_sideways         = false;
    bool is_y_form_expanding = false;
    bool is_wide             = false;
    bool is_contracted       = false;

    double distance_atr_ratio = 0.0; // when ATR known

    // Small recent distance history (newest last) for diagnostics
    std::vector<double> recent_distances_pct;
};

/**
 * EMA Regime Detector / Y-form + Sideways helper (Phase 2.1).
 *
 * Small, reusable, header-only component.
 *
 * Responsibilities:
 * - Track EMA(50) vs EMA(100) distance history
 * - Detect Y-förmig expansion after contraction/sideways (the "Y-förmig ausbaut" signal)
 * - Provide sideways regime using the user's rule: range of last N swings < ATR
 * - Expose a "too far apart" caution filter
 *
 * Design:
 * - Observer style: you feed it the two EMA values (+ optional swing range / ATR) each bar.
 * - No ownership of the EMAs or SwingDetector.
 * - Fully reset-able for Monte Carlo object reuse (landed with monte-carlo merge).
 * - Rich but cheap observability.
 */
class ema_regime_detector
{
public:
    /**
     * swing_n            : number of recent swings for the user's sideways rule (14)
     * dist_history       : bounded history of EMA distances for expansion detection
     * expansion_ratio    : current_dist > recent_min * ratio => expansion signal
     * wide_dist_pct      : distance_pct threshold for "too far apart" caution
     * wide_dist_atr_mult : distance / ATR threshold (secondary)
     */
    explicit ema_regime_detector(std::size_t swing_n            = 14,
                                 std::size_t dist_history       = 48,
                                 double      expansion_ratio    = 1.65,
                                 double      wide_dist_pct      = 2.8,
                                 double      wide_dist_atr_mult = 1.9);

    /**
     * Primary update path (observer style).
     * Call after your ema50/ema100 and (optionally) swing_detector + ATR.
     */
    void update(double ema_fast,
                double ema_slow,
                std::optional<double> swing_range = std::nullopt,
                std::optional<double> atr         = std::nullopt,
                std::optional<double> price       = std::nullopt);

    /** Convenience overload that pulls swing range + ATR directly. */
    void update(double ema_fast,
                double ema_slow,
                const swing_detector& sd,
                const average_true_range& atr);

    bool ready() const;

    // === Core regime signals ===
    bool        is_sideways() const;
    bool        is_y_form_expanding() const;
    bool        is_wide() const;           // caution filter
    bool        is_contracted() const;

    double      ema_distance_abs() const;
    double      ema_distance_pct() const;  // primary observable
    double      ema_distance_atr_ratio() const; // 0 if ATR unknown

    ema_regime  current_regime() const;

    ema_regime_snapshot snapshot() const;

    // Monte-Carlo / strategy reuse
    void reset();

    // TUI / analytics / MC reporting
    std::vector<std::pair<std::string, double>> get_indicator_values() const;

    // Configuration accessors
    std::size_t swing_n() const;
    std::size_t dist_history_size() const;

private:
    // configuration
    std::size_t swing_n_;
    std::size_t dist_history_;
    double      expansion_ratio_;
    double      wide_dist_pct_;
    double      wide_dist_atr_mult_;

    // state
    std::deque<double> dist_abs_history_;
    std::deque<double> dist_pct_history_;

    double last_ema_fast_ = 0.0;
    double last_ema_slow_ = 0.0;
    double last_dist_abs_ = 0.0;
    double last_dist_pct_ = 0.0;
    double last_atr_      = 0.0;
    double last_swing_range_ = 0.0;

    bool sideways_flag_   = false;
    bool expanding_flag_  = false;
    bool wide_flag_       = false;
    bool contracted_flag_ = false;
    ema_regime regime_    = ema_regime::unknown;

    bool has_values_ = false;

    void recompute_flags_and_regime();
    double compute_recent_min_dist_pct(std::size_t lookback) const;
    void trim_histories();
};

// -----------------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------------

inline ema_regime_detector::ema_regime_detector(std::size_t swing_n,
                                                std::size_t dist_history,
                                                double      expansion_ratio,
                                                double      wide_dist_pct,
                                                double      wide_dist_atr_mult)
    : swing_n_(swing_n)
    , dist_history_(dist_history)
    , expansion_ratio_(expansion_ratio)
    , wide_dist_pct_(wide_dist_pct)
    , wide_dist_atr_mult_(wide_dist_atr_mult)
{
}

inline void ema_regime_detector::update(double ema_fast,
                                        double ema_slow,
                                        std::optional<double> swing_range,
                                        std::optional<double> atr,
                                        std::optional<double> /*price*/)
{
    if (ema_slow <= 0.0) return;

    last_ema_fast_ = ema_fast;
    last_ema_slow_ = ema_slow;

    double dist_abs = std::abs(ema_fast - ema_slow);
    double dist_pct = (dist_abs / ema_slow) * 100.0;

    last_dist_abs_ = dist_abs;
    last_dist_pct_ = dist_pct;

    if (swing_range) last_swing_range_ = *swing_range;
    if (atr)         last_atr_         = *atr;

    dist_abs_history_.push_back(dist_abs);
    dist_pct_history_.push_back(dist_pct);

    trim_histories();

    recompute_flags_and_regime();
    has_values_ = true;
}

inline void ema_regime_detector::update(double ema_fast,
                                        double ema_slow,
                                        const swing_detector& sd,
                                        const average_true_range& atr)
{
    std::optional<double> swing_range;
    double atr_val     = atr.ready() ? atr.value() : 0.0;

    // Use the user's rule: range of last swing_n_ swings
    if (sd.ready())
        swing_range = sd.recent_swing_range(swing_n_);

    update(ema_fast, ema_slow, swing_range, atr_val);
}

inline bool ema_regime_detector::ready() const
{
    return has_values_ && !dist_pct_history_.empty();
}

inline bool ema_regime_detector::is_sideways() const     { return sideways_flag_; }
inline bool ema_regime_detector::is_y_form_expanding() const { return expanding_flag_; }
inline bool ema_regime_detector::is_wide() const         { return wide_flag_; }
inline bool ema_regime_detector::is_contracted() const   { return contracted_flag_; }

inline double ema_regime_detector::ema_distance_abs() const  { return last_dist_abs_; }
inline double ema_regime_detector::ema_distance_pct() const  { return last_dist_pct_; }
inline double ema_regime_detector::ema_distance_atr_ratio() const
{
    if (last_atr_ <= 0.0) return 0.0;
    return last_dist_abs_ / last_atr_;
}

inline ema_regime ema_regime_detector::current_regime() const { return regime_; }

inline ema_regime_snapshot ema_regime_detector::snapshot() const
{
    ema_regime_snapshot s;
    s.regime                 = regime_;
    s.ema_fast               = last_ema_fast_;
    s.ema_slow               = last_ema_slow_;
    s.distance_abs           = last_dist_abs_;
    s.distance_pct           = last_dist_pct_;
    s.is_sideways            = sideways_flag_;
    s.is_y_form_expanding    = expanding_flag_;
    s.is_wide                = wide_flag_;
    s.is_contracted          = contracted_flag_;
    s.distance_atr_ratio     = ema_distance_atr_ratio();

    s.recent_distances_pct.reserve(dist_pct_history_.size());
    for (double d : dist_pct_history_)
        s.recent_distances_pct.push_back(d);

    return s;
}

inline void ema_regime_detector::reset()
{
    dist_abs_history_.clear();
    dist_pct_history_.clear();

    last_ema_fast_ = last_ema_slow_ = 0.0;
    last_dist_abs_ = last_dist_pct_ = 0.0;
    last_atr_ = last_swing_range_ = 0.0;

    sideways_flag_ = expanding_flag_ = wide_flag_ = contracted_flag_ = false;
    regime_ = ema_regime::unknown;
    has_values_ = false;
}

inline std::vector<std::pair<std::string, double>> ema_regime_detector::get_indicator_values() const
{
    std::vector<std::pair<std::string, double>> vals;
    vals.reserve(10);

    vals.emplace_back("ema_regime", static_cast<double>(static_cast<int>(regime_)));
    vals.emplace_back("ema_dist_pct", last_dist_pct_);
    vals.emplace_back("ema_dist_abs", last_dist_abs_);
    vals.emplace_back("ema_sideways", sideways_flag_ ? 1.0 : 0.0);
    vals.emplace_back("ema_y_expanding", expanding_flag_ ? 1.0 : 0.0);
    vals.emplace_back("ema_wide", wide_flag_ ? 1.0 : 0.0);
    vals.emplace_back("ema_contracted", contracted_flag_ ? 1.0 : 0.0);

    return vals;
}

inline std::size_t ema_regime_detector::swing_n() const          { return swing_n_; }
inline std::size_t ema_regime_detector::dist_history_size() const { return dist_history_; }

inline void ema_regime_detector::recompute_flags_and_regime()
{
    // Sideways (user rule priority)
    bool swing_sideways = (last_swing_range_ > 0.0 && last_atr_ > 0.0 && last_swing_range_ < last_atr_);

    // EMA contraction
    bool ema_contracted = (last_dist_pct_ < 0.8); // very tight EMAs

    sideways_flag_   = swing_sideways || (ema_contracted && last_dist_pct_ < 1.2);
    contracted_flag_ = ema_contracted;

    // Y-form expansion detection
    expanding_flag_ = false;
    if (dist_pct_history_.size() >= 8)
    {
        auto it = dist_pct_history_.rbegin();
        const double current = *it;
        ++it; // exclude the current expanded value from the contraction baseline
        double recent_min = (it != dist_pct_history_.rend()) ? *it : 0.0;
        for (; it != dist_pct_history_.rend(); ++it)
            recent_min = std::min(recent_min, *it);

        if (recent_min > 0.0 && current > recent_min * expansion_ratio_)
        {
            // Only count as Y if we came from a relatively contracted state
            if (recent_min < 1.4)
                expanding_flag_ = true;
        }
    }

    // Wide / caution
    wide_flag_ = (last_dist_pct_ > wide_dist_pct_) ||
                 (last_atr_ > 0.0 && ema_distance_atr_ratio() > wide_dist_atr_mult_);

    // Regime classification (simple priority)
    if (wide_flag_)
        regime_ = ema_regime::wide;
    else if (expanding_flag_)
        regime_ = ema_regime::y_expanding;
    else if (sideways_flag_)
        regime_ = ema_regime::sideways;
    else if (contracted_flag_)
        regime_ = ema_regime::contracted;
    else
        regime_ = ema_regime::unknown;
}

inline double ema_regime_detector::compute_recent_min_dist_pct(std::size_t lookback) const
{
    if (dist_pct_history_.empty()) return 0.0;

    auto it = dist_pct_history_.rbegin();
    double mn = *it;
    ++it;

    for (std::size_t i = 1; i < lookback && it != dist_pct_history_.rend(); ++i, ++it)
        mn = std::min(mn, *it);

    return mn;
}

inline void ema_regime_detector::trim_histories()
{
    while (dist_abs_history_.size() > dist_history_)
        dist_abs_history_.pop_front();
    while (dist_pct_history_.size() > dist_history_)
        dist_pct_history_.pop_front();
}
