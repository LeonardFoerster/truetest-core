#pragma once

#include "analytics/footprint/footprint_aggregator.h"
#include "providers/footprint/footprint_research_tap.h"
#include "providers/footprint/footprint_ring.h"
#include "providers/footprint/footprint_venue_capabilities.h"
#include "providers/provider_event.h"
#include "ui/desk/research_views.h"

#include <cstdint>
#include <string>

// Live counterpart to FootprintDemoState (footprint_panel_state.h): installs
// as the DataBridge research tap (footprint.md §2.1), and on a steady cold
// cadence drains the ring into the real §2.2 aggregator and republishes a
// ResearchPresentation the desk (§2.3) can render - no demo data involved.
//
// Threading contract (unchanged from footprint_ring.h/footprint_research_tap.h):
// tap()/tap_tick() must be called from exactly one thread (the DataBridge
// streaming thread) - never concurrently with itself. poll() must be
// called from exactly one thread (the desk's own poll cadence) - never
// concurrently with itself, and it may run concurrently with tap() (that's
// the whole point of the SPSC ring). Constructing/destroying this object is
// the caller's job to serialize against both.
namespace truetest::ui::desk {

struct FootprintLiveSourceConfig
{
    truetest::footprint::venue_id venue = truetest::footprint::venue_id::unknown;
    std::uint16_t symbol_id = 0;
    std::string symbol_label; // ResearchSurfaceStatus.source / logs only - not hashed/compared

    // resolve_footprint_tick_size()'s result - 0 means unavailable; the tap
    // then only accepts ticks that already arrived with has_exact_decimal
    // set (see footprint_research_tap.h::tap_context_ready).
    double tick_size = 0.0;
    double qty_atom_scale = 1.0;

    // group_size/imbalance_min_volume/cvd_reset_ns_of_day/max_bars from
    // toolbar-equivalent settings; bar_spec is filled in from these too.
    // tick_size/qty_atom_scale above always win over anything set here.
    truetest::footprint::FootprintAggregatorConfig agg_overrides;
};

class FootprintLiveSource
{
public:
    explicit FootprintLiveSource(FootprintLiveSourceConfig config);

    FootprintLiveSource(const FootprintLiveSource&) = delete;
    FootprintLiveSource& operator=(const FootprintLiveSource&) = delete;

    // Install as DataBridge<provider::event>::set_research_tap or
    // DataBridge<provider::tick>::set_research_tap. Never allocates, locks,
    // logs, retries, aggregates, or blocks (footprint.md §2.1) - the
    // heavier drain/aggregate/publish work happens in poll(), off this path.
    void tap(const provider::event& ev) noexcept;
    void tap_tick(const provider::tick& t) noexcept;

    // Cold-path: drains the ring (bounded, never loops past ring capacity)
    // into the aggregator and returns a freshly built presentation. Never
    // blocks the tap - see the class-level threading contract.
    research_view_handle poll();

    truetest::footprint::data_status status() const noexcept { return status_; }
    std::uint64_t received_count() const noexcept { return received_count_; }

private:
    FootprintLiveSourceConfig config_;
    truetest::footprint::FootprintTapContext tap_ctx_;
    truetest::footprint::FootprintResearchRing<8192> ring_;
    truetest::footprint::FootprintAggregator aggregator_;

    truetest::footprint::data_status status_ = truetest::footprint::data_status::backfilling;
    std::uint64_t received_count_ = 0;
    // Version handed out on the last publish that actually changed
    // anything (drained new trades or a status transition) - see poll()'s
    // comment. Drawn from the shared next_research_version() sequence
    // (research_views.h) rather than a local counter starting at 0, so two
    // distinct FootprintLiveSource instances (e.g. across a reconnect that
    // constructs a fresh one) can never coincidentally hand out the same
    // version number. 0 doubles as "never published" since
    // next_research_version() never hands out 0.
    std::uint64_t last_published_version_ = 0;
};

} // namespace truetest::ui::desk
