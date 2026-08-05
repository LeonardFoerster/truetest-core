#pragma once

#include "ui/desk/desk_window_names.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace truetest::ui::desk {

inline constexpr std::uint32_t desk_layout_version = 2;
inline constexpr const char* desk_layout_ini_filename = "truetest_desk_v2.ini";

enum class DeskPage : std::uint8_t
{
    orderflow,
    liquidity,
    structure,
    markets,
    operations,
    count,
};

enum class DeskDockSlot : std::uint8_t
{
    primary,
    left,
    right_top,
    right_bottom,
    bottom,
    count,
};

struct DeskLayoutGeometry
{
    float right_ratio;
    float bottom_ratio;
    float left_ratio;
    float right_bottom_ratio;
};

struct DeskPanelAssignment
{
    DeskPanel panel;
    DeskDockSlot slot;
};

inline constexpr std::array<DeskPage, 5> desk_pages = {
    DeskPage::orderflow,
    DeskPage::liquidity,
    DeskPage::structure,
    DeskPage::markets,
    DeskPage::operations,
};

constexpr const char* desk_page_label(DeskPage page) noexcept
{
    switch (page)
    {
    case DeskPage::orderflow:  return "Orderflow";
    case DeskPage::liquidity:  return "Liquidity";
    case DeskPage::structure:  return "Structure";
    case DeskPage::markets:    return "Markets";
    case DeskPage::operations: return "Operations";
    case DeskPage::count:      break;
    }
    return "Unknown";
}

constexpr const char* desk_page_dockspace_name(DeskPage page) noexcept
{
    switch (page)
    {
    case DeskPage::orderflow:  return "OrderflowDockSpaceV2";
    case DeskPage::liquidity:  return "LiquidityDockSpaceV2";
    case DeskPage::structure:  return "StructureDockSpaceV2";
    case DeskPage::markets:    return "MarketsDockSpaceV2";
    case DeskPage::operations: return "OperationsDockSpaceV2";
    case DeskPage::count:      break;
    }
    return "UnknownDockSpaceV2";
}

constexpr const char* desk_focus_dockspace_name(DeskPage page) noexcept
{
    switch (page)
    {
    case DeskPage::orderflow:  return "OrderflowFocusDockSpaceV2";
    case DeskPage::liquidity:  return "LiquidityFocusDockSpaceV2";
    case DeskPage::structure:  return "StructureFocusDockSpaceV2";
    case DeskPage::markets:    return "MarketsFocusDockSpaceV2";
    case DeskPage::operations: return "OperationsFocusDockSpaceV2";
    case DeskPage::count:      break;
    }
    return "UnknownFocusDockSpaceV2";
}

constexpr bool should_seed_default_layout(bool existing_node,
                                          bool explicit_request_pending) noexcept
{
    return !existing_node && !explicit_request_pending;
}

constexpr bool should_keep_inactive_dockspace(bool existing_node) noexcept
{
    return existing_node;
}

constexpr std::size_t kpi_column_count(float available_width,
                                       std::size_t metric_count,
                                       float minimum_card_width = 150.0f,
                                       float gap = 6.0f) noexcept
{
    if (metric_count == 0)
        return 0;
    if (available_width <= minimum_card_width)
        return 1;
    const auto fit = static_cast<std::size_t>(
        (available_width + gap) / (minimum_card_width + gap));
    std::size_t columns = fit > metric_count ? metric_count : fit;
    while (columns > 2 && metric_count % columns == 1)
        --columns;
    return columns < 1 ? 1 : columns;
}

constexpr DeskLayoutGeometry desk_layout_geometry(DeskPage page) noexcept
{
    switch (page)
    {
    // footprint.md §2.3: ~12% watchlist, ~17% right rail, ~12% bottom
    // blotter, remaining 60-65% for footprint+CVD, DOM ~70%/context ~30%
    // of the right rail. See desk_layout_fractions() below for the derived
    // percentages these literal split ratios compound to, and
    // ImGuiDeskLayout.OrderflowGeometryMatchesFootprintSpec for the check.
    case DeskPage::orderflow:  return {0.17f, 0.12f, 0.145f, 0.30f};
    case DeskPage::liquidity:  return {0.18f, 0.16f, 0.00f, 0.38f};
    case DeskPage::structure:  return {0.28f, 0.00f, 0.00f, 0.48f};
    case DeskPage::markets:    return {0.32f, 0.00f, 0.00f, 0.48f};
    case DeskPage::operations: return {0.28f, 0.25f, 0.32f, 0.48f};
    case DeskPage::count:      break;
    }
    return {0.17f, 0.12f, 0.145f, 0.30f};
}

// Derived area fractions for a geometry, matching the actual
// DockBuilderSplitNode compounding order in desk_layout.cpp: right and
// bottom split off the full-size `primary` node first (so their fractions
// are exact), then left splits the already-shrunk primary (so its width
// fraction of the TOTAL is left_ratio*(1-right_ratio), not left_ratio
// itself), then right_bottom splits the `right` column last. Kept
// alongside desk_layout_geometry() so its literal ratios stay honest
// against the percentages documented in footprint.md §2.3.
struct DeskLayoutFractions
{
    float left_width_fraction = 0.0f;
    float right_width_fraction = 0.0f;
    float bottom_height_fraction = 0.0f;
    float primary_area_fraction = 0.0f;      // footprint + CVD canvas
    float right_top_area_fraction = 0.0f;    // DOM's share of the right rail
    float right_bottom_area_fraction = 0.0f; // selected-context's share
};

constexpr DeskLayoutFractions desk_layout_fractions(DeskLayoutGeometry g) noexcept
{
    const float primary_w = (1.0f - g.left_ratio) * (1.0f - g.right_ratio);
    const float primary_h = 1.0f - g.bottom_ratio;
    return DeskLayoutFractions{
        g.left_ratio * (1.0f - g.right_ratio),
        g.right_ratio,
        g.bottom_ratio,
        primary_w * primary_h,
        1.0f - g.right_bottom_ratio,
        g.right_bottom_ratio,
    };
}

inline constexpr std::array<DeskPanelAssignment, 5> orderflow_assignments = {{
    {DeskPanel::orderflow_canvas, DeskDockSlot::primary},
    {DeskPanel::watchlist, DeskDockSlot::left},
    {DeskPanel::orderflow_dom, DeskDockSlot::right_top},
    {DeskPanel::selected_context, DeskDockSlot::right_bottom},
    {DeskPanel::activity_blotter, DeskDockSlot::bottom},
}};

inline constexpr std::array<DeskPanelAssignment, 4> liquidity_assignments = {{
    {DeskPanel::liquidity_heatmap, DeskDockSlot::primary},
    {DeskPanel::liquidity_dom, DeskDockSlot::right_top},
    {DeskPanel::liquidations, DeskDockSlot::right_bottom},
    {DeskPanel::liquidity_tape, DeskDockSlot::bottom},
}};

inline constexpr std::array<DeskPanelAssignment, 3> structure_assignments = {{
    {DeskPanel::tpo_profile, DeskDockSlot::primary},
    {DeskPanel::volume_profile, DeskDockSlot::right_top},
    {DeskPanel::session_context, DeskDockSlot::right_bottom},
}};

inline constexpr std::array<DeskPanelAssignment, 2> markets_assignments = {{
    {DeskPanel::correlation, DeskDockSlot::primary},
    {DeskPanel::funding, DeskDockSlot::right_top},
}};

inline constexpr std::array<DeskPanelAssignment, 5> operations_assignments = {{
    {DeskPanel::equity, DeskDockSlot::primary},
    {DeskPanel::strategies, DeskDockSlot::left},
    {DeskPanel::risk, DeskDockSlot::right_top},
    {DeskPanel::health, DeskDockSlot::right_bottom},
    {DeskPanel::operations_activity, DeskDockSlot::bottom},
}};

constexpr std::span<const DeskPanelAssignment>
desk_page_assignments(DeskPage page) noexcept
{
    switch (page)
    {
    case DeskPage::orderflow:  return orderflow_assignments;
    case DeskPage::liquidity:  return liquidity_assignments;
    case DeskPage::structure:  return structure_assignments;
    case DeskPage::markets:    return markets_assignments;
    case DeskPage::operations: return operations_assignments;
    case DeskPage::count:      break;
    }
    return {};
}

constexpr bool desk_page_contains(DeskPage page, DeskPanel panel) noexcept
{
    for (const auto& assignment : desk_page_assignments(page))
        if (assignment.panel == panel)
            return true;
    return false;
}

constexpr bool desk_panel_visible_in_focus(DeskPage page, DeskPanel panel) noexcept
{
    const auto assignments = desk_page_assignments(page);
    return !assignments.empty() && assignments.front().panel == panel;
}

class DeskPageController
{
public:
    DeskPage active_page() const noexcept { return active_; }

    bool select(DeskPage page) noexcept
    {
        if (page == DeskPage::count || page == active_)
            return false;
        active_ = page;
        return true;
    }

    void request_layout_reset() noexcept { pending_reset_ = active_; }
    bool has_layout_request() const noexcept { return pending_reset_.has_value(); }

    std::optional<DeskPage> consume_layout_request() noexcept
    {
        auto result = pending_reset_;
        pending_reset_.reset();
        return result;
    }

private:
    DeskPage active_ = DeskPage::orderflow;
    std::optional<DeskPage> pending_reset_;
};

} // namespace truetest::ui::desk
