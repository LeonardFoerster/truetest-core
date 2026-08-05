#pragma once

#include "analytics/footprint/footprint_aggregator.h"
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

    // Rebuilds `aggregator`'s config from `settings` and replays `trades`
    // into it from scratch. Call whenever an aggregation-affecting setting
    // changes (bar type/interval, tick grouping, imbalance minimum, CVD
    // boundary).
    void reaggregate();
};

} // namespace truetest::ui::desk
