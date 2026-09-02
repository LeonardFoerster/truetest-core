#include "ui/desk/footprint_live_source.h"

#include "ui/desk/footprint_presentation_bridge.h"

namespace truetest::ui::desk {

namespace {
truetest::footprint::FootprintAggregatorConfig build_agg_config(
    const FootprintLiveSourceConfig& c)
{
    auto cfg = c.agg_overrides;
    cfg.tick_size = c.tick_size;
    cfg.qty_atom_scale = c.qty_atom_scale;
    return cfg;
}
} // namespace

FootprintLiveSource::FootprintLiveSource(FootprintLiveSourceConfig config)
    : config_(std::move(config))
    , aggregator_(build_agg_config(config_))
{
    tap_ctx_.venue = config_.venue;
    tap_ctx_.symbol_id = config_.symbol_id;
    tap_ctx_.symbol = config_.symbol_label;
    tap_ctx_.session_id = 1; // bumped externally on reconnect once that plumbing exists (Phase 2b)
    tap_ctx_.tick_size = config_.tick_size;
    tap_ctx_.qty_atom_scale = config_.qty_atom_scale;
}

void FootprintLiveSource::tap(const provider::event& ev) noexcept
{
    if (const auto* t = std::get_if<provider::tick>(&ev))
        tap_tick(*t);
}

void FootprintLiveSource::tap_tick(const provider::tick& t) noexcept
{
    if (!truetest::footprint::try_tap_push(tap_ctx_, t, ring_))
        rejected_count_.fetch_add(1, std::memory_order_relaxed);
}

research_view_handle FootprintLiveSource::poll()
{
    truetest::footprint::PublicTrade trade;
    std::size_t drained = 0;
    std::size_t accepted = 0;
    // Bounded by the ring's own capacity - never an unbounded/hanging loop
    // even under a sustained burst (footprint.md §2.1 "must never halt or
    // slow the engine" applies to the tap; this cold consumer just must not
    // itself spin forever).
    constexpr std::size_t kMaxPerPoll = decltype(ring_)::capacity();
    while (drained < kMaxPerPoll && ring_.try_pop(trade))
    {
        if (aggregator_.on_trade(trade)) {
            ++received_count_;
            ++accepted;
        } else {
            rejected_count_.fetch_add(1, std::memory_order_relaxed);
        }
        ++drained;
    }

    const auto status_before = status_;
    const auto rejected_now = rejected_count_.load(std::memory_order_relaxed);
    const bool newly_rejected = rejected_now != observed_rejected_count_;
    observed_rejected_count_ = rejected_now;
    const auto discontinuity_generation = ring_.discontinuity_generation();
    if (ring_.discontinuous() || newly_rejected)
    {
        status_ = truetest::footprint::data_status::recovering;
        if (ring_.discontinuous())
            ring_.acknowledge_discontinuity(discontinuity_generation);
    }
    else if (accepted > 0
             && status_ == truetest::footprint::data_status::recovering)
    {
        // Ring caught back up after a drop - without cache/reconciliation
        // (Phase 2b) there is no bounded repair to attempt, so the honest
        // next state is BACKFILLING (live-tailing, nothing reconciled
        // against history yet), never a silent return to LIVE.
        status_ = truetest::footprint::data_status::backfilling;
    }
    // else: stays BACKFILLING - see the class-level doc comment; this
    // source has no cache/reconciliation layer yet, so it can never
    // honestly claim LIVE (footprint.md §2.2's definition of LIVE requires
    // a completed, verified backfill).

    FootprintPresentationOptions opts; // base units; desk toolbar settings
                                        // are layered on top by the caller,
                                        // matching FootprintDemoState's split.
    auto view = std::make_shared<ResearchPresentation>();
    view->footprint = to_footprint_bar_views(aggregator_, opts);
    view->footprint_status = status_;
    view->state = status_ == truetest::footprint::data_status::recovering
        ? DeskDataState::error
        : received_count_ > 0 ? DeskDataState::stale
                              : DeskDataState::unavailable;
    view->source = config_.symbol_label;
    // Only mint a new version when something actually changed - new trades
    // drained or a status transition - not on every ~100ms poll cadence
    // regardless of whether anything moved. aggregator_.on_trade() is the
    // only thing that mutates bar state, so drained==0 with no status
    // change means to_footprint_bar_views() above reproduced byte-identical
    // output; republishing a fresh version for it would defeat
    // research_panels.cpp's FootprintBoundsCache/FootprintViewportCache,
    // forcing their O(bars) rescans every tick even on a quiet market.
    if (drained > 0 || status_ != status_before || last_published_version_ == 0)
        last_published_version_ = next_research_version();
    view->version = last_published_version_;

    auto& surface = view->surfaces[static_cast<std::size_t>(ResearchSurface::footprint)];
    surface.state = view->state;
    surface.source = config_.symbol_label;
    surface.version = view->version;

    return view;
}

} // namespace truetest::ui::desk
