#pragma once

#include "providers/footprint/footprint_research_service.h"
#include "ui/desk/desk_context.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace truetest::ui::desk {

// Process-wide monotonic counter for live research sources' publish
// versions (FootprintLiveSource and, in future, any other live surface
// source). A per-instance-local counter starting at 0 risks two distinct
// source instances (e.g. across a reconnect that constructs a fresh
// FootprintLiveSource) both producing version 1 on their first publish -
// which would corrupt anything caching on version (e.g. research_panels.cpp's
// FootprintBoundsCache/FootprintViewportCache) into reusing stale data.
// NOT used by the demo fixture, which deliberately keeps a small, stable,
// deterministic version of its own (see demo_research.cpp) - the cache
// layer instead disambiguates demo from live via DeskDataState, not by
// making every version process-wide unique.
inline std::uint64_t next_research_version() noexcept
{
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

inline constexpr std::size_t max_visible_research_cells = 12'000;

constexpr std::size_t research_render_stride(
    std::size_t cell_count,
    std::size_t budget = max_visible_research_cells) noexcept
{
    if (cell_count == 0 || budget == 0) return 1;
    return 1 + (cell_count - 1) / budget;
}

constexpr std::size_t research_rendered_cell_count(
    std::size_t cell_count,
    std::size_t budget = max_visible_research_cells) noexcept
{
    if (cell_count == 0 || budget == 0) return 0;
    const auto stride = research_render_stride(cell_count, budget);
    return 1 + (cell_count - 1) / stride;
}

enum class ResearchSurface : std::uint8_t
{
    footprint,
    dom,
    heatmap,
    profile,
    funding,
    watchlist,
    liquidations,
    correlation,
    count,
};

struct ResearchSurfaceStatus
{
    DeskDataState state = DeskDataState::unavailable;
    std::string source;
    std::int64_t age_ms = 0;
    std::uint64_t version = 0;
};

// footprint.md §2.2 diagonal-imbalance direction, mirrored for the desk.
enum class FootprintImbalance : std::uint8_t { none, buy, sell };

struct FootprintLevelView
{
    double price = 0.0;
    double sell_qty = 0.0;
    double buy_qty = 0.0;
    double unknown_qty = 0.0; // contributes to total/OHLC only, never delta (§2.2)

    FootprintImbalance diagonal = FootprintImbalance::none;
    bool stacked = false;
    bool is_poc = false;
};

// footprint.md §2.2 bar_state, mirrored for the desk (forming bars draw
// distinctly; EMPTY is a real interval with zero trades, not a gap).
enum class FootprintBarState : std::uint8_t { forming, complete, empty };

struct FootprintBarView
{
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    FootprintBarState state = FootprintBarState::forming;
    bool gap = false; // reconciliation hook (§2.2) - hatched region when set
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double cvd = 0.0;
    std::vector<FootprintLevelView> levels;
};

struct DomLevelView
{
    double price = 0.0;
    double bid_qty = 0.0;
    double ask_qty = 0.0;
    double bought = 0.0;
    double sold = 0.0;
};

struct HeatmapCellView
{
    std::uint16_t column = 0;
    std::uint16_t row = 0;
    float value = 0.0f;
};

struct ProfileRowView
{
    double price = 0.0;
    double buy = 0.0;
    double sell = 0.0;
    std::string tpo;
};

struct FundingRowView
{
    std::string symbol;
    std::string venue;
    double funding_rate = 0.0;
    double annualized = 0.0;
    double basis_bps = 0.0;
    std::int64_t next_seconds = 0;
};

struct WatchlistRowView
{
    std::string symbol;
    double last = 0.0;
    double change_pct = 0.0;
    double volume = 0.0;
};

struct LiquidationRowView
{
    std::int64_t ts_ms = 0;
    double price = 0.0;
    double notional = 0.0;
    bool long_liquidated = false;
};

struct ResearchPresentation
{
    // Aggregate publication state is diagnostic only. Panel truth comes from
    // the per-surface entries so partial wiring cannot label missing data LIVE.
    DeskDataState state = DeskDataState::unavailable;
    std::string source;
    std::int64_t age_ms = 0;
    std::uint64_t version = 0;
    std::array<ResearchSurfaceStatus,
               static_cast<std::size_t>(ResearchSurface::count)> surfaces{};

    // footprint.md §2.2's richer data-status vocabulary (BACKFILLING,
    // RECOVERING, PARTIAL, REPLAY) doesn't fit the desk-wide DeskDataState
    // enum used by every other surface, so the footprint surface carries
    // its own alongside the generic ResearchSurfaceStatus entry above.
    truetest::footprint::data_status footprint_status =
        truetest::footprint::data_status::unavailable;

    std::vector<FootprintBarView> footprint;
    std::vector<DomLevelView> dom;
    std::vector<HeatmapCellView> heatmap;
    std::uint16_t heatmap_columns = 0;
    std::uint16_t heatmap_rows = 0;
    std::int64_t heatmap_start_ms = 0;
    std::int64_t heatmap_end_ms = 0;
    double heatmap_min_price = 0.0;
    double heatmap_max_price = 0.0;
    std::vector<ProfileRowView> profile;
    std::vector<FundingRowView> funding;
    std::vector<WatchlistRowView> watchlist;
    std::vector<LiquidationRowView> liquidations;
    std::vector<std::string> correlation_symbols;
    std::vector<double> correlation;
};

inline const ResearchSurfaceStatus& research_surface_status(
    const ResearchPresentation& presentation,
    ResearchSurface surface) noexcept
{
    return presentation.surfaces[static_cast<std::size_t>(surface)];
}

// Classify a research surface's state into a coarse provenance bucket.
enum class ResearchProvenanceClass : std::uint8_t { none, demo, real };

constexpr ResearchProvenanceClass research_provenance_class(DeskDataState state) noexcept
{
    switch (state)
    {
    case DeskDataState::demo:        return ResearchProvenanceClass::demo;
    case DeskDataState::live:
    case DeskDataState::snapshot:
    case DeskDataState::stale:       return ResearchProvenanceClass::real;
    case DeskDataState::unavailable:
    case DeskDataState::error:       return ResearchProvenanceClass::none;
    }
    return ResearchProvenanceClass::none;
}

constexpr bool research_surfaces_have_mixed_sources(
    std::span<const ResearchSurfaceStatus> surfaces) noexcept
{
    bool has_demo = false;
    bool has_real = false;
    for (const auto& surface : surfaces)
    {
        const auto cls = research_provenance_class(surface.state);
        has_demo = has_demo || cls == ResearchProvenanceClass::demo;
        has_real = has_real || cls == ResearchProvenanceClass::real;
    }
    return has_demo && has_real;
}

inline bool research_presentation_has_mixed_sources(
    const ResearchPresentation& presentation) noexcept
{
    return research_surfaces_have_mixed_sources(presentation.surfaces);
}

using research_view_handle = std::shared_ptr<const ResearchPresentation>;
using research_snapshot_fn = std::function<research_view_handle()>;

} // namespace truetest::ui::desk
