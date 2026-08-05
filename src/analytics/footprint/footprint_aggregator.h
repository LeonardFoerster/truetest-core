#pragma once

#include "analytics/footprint/footprint_bar_types.h"
#include "types/public_trade.h"

#include <cstdint>
#include <deque>

// Incremental footprint bar aggregator. footprint.md §2.2 "Aggregation
// rules". Pure cold-path research math: feed it PublicTrade in event order
// (the §2.2 reconciliation/reorder-window layer is responsible for that
// ordering - this class trusts its input), read back bars()/cvd().
//
// Not thread-safe. Intended owner is a single cold worker (matches
// cyrex/00-architecture.md's ResearchAggregator role); the desk (§2.3) polls
// a version()-gated read, never mutates.
namespace truetest::footprint {

struct FootprintAggregatorConfig
{
    BarSpec bar_spec;
    std::int64_t group_size = 1; // grouped_level = price_ticks / group_size; >= 1

    // Quote-notional inputs - display and volume-bar-threshold only, never
    // used for price bucketing (see footprint_bar_types.h FootprintBar::quote_notional).
    double tick_size = 0.0;
    double qty_atom_scale = 1.0;

    // Saved gate for diagonal-imbalance eligibility (base qty atoms). The
    // 300% ratio itself is fixed by spec, not configurable.
    std::int64_t imbalance_min_volume = 0;

    // footprint.md §2.3 "materialize up to 512 bars around the requested
    // viewport" - this is the in-memory retention cap for THIS aggregator
    // instance, not the viewport/prefetch policy itself (a later §2.3 concern).
    std::size_t max_bars = 512;

    // UTC time-of-day (nanoseconds since midnight) CVD resets at. Default 0
    // = 00:00 UTC (footprint.md §1 default).
    std::int64_t cvd_reset_ns_of_day = 0;
};

class FootprintAggregator
{
public:
    explicit FootprintAggregator(FootprintAggregatorConfig config);

    // Trades must arrive in non-decreasing event_ns order (the §2.2
    // reconciliation layer's job, not this class's). Rolls/closes bars as
    // needed, including inserting EMPTY time bars for skipped intervals.
    void on_trade(const PublicTrade& trade);

    // Force-closes the current forming bar without waiting for the next
    // trade to trigger a roll. footprint.md §2.2 "End-of-stream flushes
    // deterministic final bars." A no-op if there is no forming bar with
    // trades in progress.
    void flush();

    // Oldest-first, capped at config().max_bars. The last element is
    // `forming` (or `empty`) until flush()/the next roll closes it.
    const std::deque<FootprintBar>& bars() const noexcept { return bars_; }

    // Cumulative buy-aggressor minus sell-aggressor base qty since the last
    // UTC session boundary crossing (§2.2). Unknown aggression excluded.
    std::int64_t cvd() const noexcept { return cvd_; }

    // Bumped on every mutation - cheap dirty-flag signal for a future
    // desk/UI consumer (cyrex/00-architecture.md §7 convention). Never
    // decreases.
    std::uint64_t version() const noexcept { return version_; }

    const FootprintAggregatorConfig& config() const noexcept { return config_; }

    // Clears all bars/CVD state. footprint.md §2.2 "Reconfiguration rebuilds
    // the requested range on the cold worker and swaps it atomically when
    // ready" - the rebuild-from-history and atomic-swap orchestration is a
    // higher layer's job (a future service owns the cold worker + the
    // atomic pointer swap of a whole presentation snapshot); this method is
    // only the "start clean and re-feed trades" primitive that layer needs.
    void reset(FootprintAggregatorConfig new_config);

private:
    void ensure_bar_for(std::int64_t event_ns);
    void roll_time_bars_to(std::int64_t event_ns);
    void close_current_bar();
    void recompute_derived(FootprintBar& bar) const;
    void recompute_poc(FootprintBar& bar) const;
    void recompute_imbalance(FootprintBar& bar) const;
    void apply_cvd(const PublicTrade& trade);
    void trim_to_max_bars();

    FootprintAggregatorConfig config_;
    std::deque<FootprintBar> bars_;
    std::uint64_t version_ = 0;

    std::int64_t cvd_ = 0;
    std::int64_t cvd_last_boundary_ns_ = 0;
    bool cvd_initialized_ = false;
};

} // namespace truetest::footprint
