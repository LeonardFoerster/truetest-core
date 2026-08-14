#pragma once

#include "analytics/footprint/footprint_aggregator.h"
#include "ui/desk/desk_context.h"
#include "ui/desk/footprint_camera.h"

#include <cstdint>
#include <vector>

// Toolbar-controlled state for the footprint canvas (footprint.md §2.3) and
// the demo fixture that exercises the real §2.2 aggregator end-to-end until
// a live venue/cache source is wired (Phase 2b/4). Kept ImGui-free so the
// settings/reaggregation plumbing stays testable independent of rendering.
//
// NOT yet implemented: footprint.md §2.3 "Persist settings atomically per
// venue/symbol: bar type, interval/threshold, tick grouping, units,
// imbalance parameters, CVD boundary, camera, and follow state." DeskApp
// currently owns exactly one FootprintDemoState (settings + camera) that
// resets to defaults on every process restart - there is no per-symbol
// keying or on-disk persistence yet. This matters once multiple
// venue/symbol combinations are wired (Phase 4); flagged explicitly here
// rather than left as a silent gap.
namespace truetest::ui::desk {

struct FootprintPanelSettings
{
    enum class BarType : std::uint8_t
    {
        time_1s, time_5s, time_15s, time_1m, time_5m, volume,
    };

    BarType bar_type = BarType::time_1m;
    double volume_threshold = 5000.0; // quote-notional units; only used when bar_type == volume
    int tick_group = 1;               // footprint.md §1: Auto or x1/2/5/10/25 - Auto not yet
                                       // modeled (needs a live tick-size/volatility feed to pick
                                       // one), defaults to the explicit x1 multiplier
    bool quote_units = false;         // base/quote display toggle - pure display transform
    std::int64_t imbalance_min_volume = 10; // base qty atoms... display units here, see bridge
    int cvd_reset_hour_utc = 0;
    bool cvd_collapsed = false;       // §2.3 "CVD occupies a collapsible 18% lower sub-chart"

    // Aggregation-affecting fields only - quote_units/cvd_collapsed are
    // pure display transforms and never require re-aggregating.
    bool aggregation_equal(const FootprintPanelSettings& other) const noexcept
    {
        return bar_type == other.bar_type
            && volume_threshold == other.volume_threshold
            && tick_group == other.tick_group
            && imbalance_min_volume == other.imbalance_min_volume
            && cvd_reset_hour_utc == other.cvd_reset_hour_utc;
    }
};

std::int64_t footprint_bar_type_interval_ns(FootprintPanelSettings::BarType type) noexcept;
const char* footprint_bar_type_label(FootprintPanelSettings::BarType type) noexcept;

// Time/price extent of a footprint bar series (research_panels.cpp's
// compute_footprint_bounds()). Kept ImGui-free like the rest of this header.
struct FootprintDataBounds
{
    std::int64_t time_min_ms = 0;
    std::int64_t time_max_ms = 1;
    double price_min = 0.0;
    double price_max = 1.0;
    bool valid = false;
};

// Caches compute_footprint_bounds()'s O(bars) scan, keyed on the footprint
// surface's own (state, publish version) pair (ResearchSurfaceStatus::state
// + ::version, NOT ResearchPresentation::version - the top-level version is
// not bumped by refresh_demo_footprint()/refresh_live_footprint() when only
// the footprint field republishes, so it would never invalidate this
// cache). Recompute when either changes; otherwise reuse `bounds` as-is.
//
// `state` is part of the key, not just `version`, because version alone is
// only unique WITHIN one source: the demo fixture deliberately keeps a
// small, fixed, deterministic version (see demo_research.cpp), while a live
// FootprintLiveSource starts its own counter near 0 too - so "version 1"
// can legitimately mean either "the demo fixture" or "a live source's first
// publish". Without `state` disambiguating them, switching from demo to a
// freshly-connected live source (or back) could hit a coincidental version
// match and silently reuse the WRONG source's cached bounds - exactly the
// kind of dishonest-display bug the DeskDataState contract elsewhere in the
// desk exists to prevent.
struct FootprintBoundsCache
{
    DeskDataState state = DeskDataState::unavailable;
    std::uint64_t version = 0;
    bool has_value = false;
    FootprintDataBounds bounds{};
};

// Caches draw_footprint_canvas()'s viewport-culled bar index list and heat-
// intensity normalization max. Both are recomputed together from the same
// inputs - footprint data (state, version) and the camera's visible TIME
// range only (culling in draw_footprint_canvas is time-only, never
// price-filtered) - so one staleness check covers both. See
// FootprintBoundsCache's comment for why `state` is part of the key, not
// just `version`. `visible_indices`' storage persists across frames
// (cleared, not reallocated) to avoid a per-frame heap allocation once
// capacity stabilizes.
struct FootprintViewportCache
{
    DeskDataState state = DeskDataState::unavailable;
    std::uint64_t version = 0;
    std::int64_t time_min_ms = 0;
    std::int64_t time_max_ms = 0;
    bool has_value = false;
    std::vector<std::size_t> visible_indices;
    double max_total_all = 1.0;
};

// Bundles the camera + settings + the real aggregator behind the footprint
// demo fixture. footprint.md §2.2's own definition of raw-frame replay -
// "use the same parser and aggregator ... perform no REST requests and
// will not write into the live cache" - describes exactly this: a fixed,
// deterministic trade sequence fed through the production aggregator.
struct FootprintDemoState
{
    FootprintDemoState();

    FootprintCamera camera;
    FootprintPanelSettings settings;
    truetest::footprint::FootprintAggregator aggregator;
    std::vector<truetest::footprint::PublicTrade> trades;
    // Per-frame recompute caches for the footprint canvas - see
    // FootprintBoundsCache/FootprintViewportCache above. Named generically
    // (not "demo_*") because they cache whatever ResearchPresentation is
    // currently active, demo or external live source alike.
    FootprintBoundsCache bounds_cache;
    FootprintViewportCache viewport_cache;

    // Rebuilds `aggregator`'s config from `settings` and replays `trades`
    // into it from scratch. Call whenever an aggregation-affecting setting
    // changes (bar type/interval, tick grouping, imbalance minimum, CVD
    // boundary).
    void reaggregate();
};

} // namespace truetest::ui::desk
