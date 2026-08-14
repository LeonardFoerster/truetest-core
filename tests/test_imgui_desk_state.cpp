#include <gtest/gtest.h>

#include "ui/desk/about_model.h"
#include "ui/desk/desk_command_model.h"
#include "ui/desk/desk_context.h"
#include "ui/desk/desk_layout_model.h"
#include "ui/desk/research_views.h"

#include <array>
#include <cstddef>
#include <set>
#include <string>

namespace {

TEST(ImGuiDeskPages, LabelsAndDockspaceIdsAreUnique)
{
    std::set<std::string> labels;
    std::set<std::string> dockspaces;
    std::set<std::string> focus_dockspaces;
    for (const auto page : truetest::ui::desk::desk_pages)
    {
        EXPECT_TRUE(labels.insert(truetest::ui::desk::desk_page_label(page)).second);
        EXPECT_TRUE(dockspaces.insert(truetest::ui::desk::desk_page_dockspace_name(page)).second);
        EXPECT_TRUE(focus_dockspaces.insert(
            truetest::ui::desk::desk_focus_dockspace_name(page)).second);
    }
    EXPECT_EQ(labels.size(), 1u);
    EXPECT_EQ(dockspaces.size(), 1u);
    EXPECT_EQ(focus_dockspaces.size(), 1u);
}

TEST(ImGuiDeskPages, DefaultsToMonitorAndSwitchesDeterministically)
{
    using truetest::ui::desk::DeskPage;
    truetest::ui::desk::DeskPageController controller;
    EXPECT_EQ(controller.active_page(), DeskPage::monitor);
    EXPECT_FALSE(controller.select(DeskPage::monitor));
    EXPECT_TRUE(controller.select(DeskPage::operations));
    EXPECT_EQ(controller.active_page(), DeskPage::operations);
    controller.request_layout_reset();
    ASSERT_TRUE(controller.has_layout_request());
    EXPECT_EQ(*controller.consume_layout_request(), DeskPage::operations);
    EXPECT_FALSE(controller.consume_layout_request().has_value());
}

TEST(ImGuiDeskPages, V2PanelInstancesAreUniqueAcrossWorkspaces)
{
    using namespace truetest::ui::desk;
    std::array<int, desk_panel_count> launched{};
    for (const auto page : desk_pages)
    {
        std::array<int, desk_panel_count> within_page{};
        for (const auto& assignment : desk_page_assignments(page))
        {
            const auto panel = static_cast<std::size_t>(assignment.panel);
            const auto slot = static_cast<std::size_t>(assignment.slot);
            ASSERT_LT(panel, desk_panel_count);
            ASSERT_LT(slot, static_cast<std::size_t>(DeskDockSlot::count));
            ++within_page[panel];
            ++launched[panel];
        }
        for (const int count : within_page)
            EXPECT_LE(count, 1);
    }

    // Only Monitor is in desk_pages now; it launches activity_blotter,
    // health, and risk exactly once. Everything else below is dormant
    // (still fully defined in desk_layout_model.h, just benched from
    // desk_pages) and launches zero times until re-added.
    for (const auto panel : {
             DeskPanel::activity_blotter, DeskPanel::health, DeskPanel::risk})
        EXPECT_EQ(launched[static_cast<std::size_t>(panel)], 1) << static_cast<int>(panel);
    for (const auto panel : {
             DeskPanel::watchlist, DeskPanel::orderflow_canvas, DeskPanel::orderflow_dom,
             DeskPanel::selected_context,
             DeskPanel::liquidity_heatmap, DeskPanel::liquidity_dom,
             DeskPanel::liquidations, DeskPanel::liquidity_tape,
             DeskPanel::tpo_profile, DeskPanel::volume_profile, DeskPanel::session_context,
             DeskPanel::funding, DeskPanel::correlation,
             DeskPanel::equity, DeskPanel::operations_activity, DeskPanel::strategies,
             DeskPanel::debug})
        EXPECT_EQ(launched[static_cast<std::size_t>(panel)], 0) << static_cast<int>(panel);
}

TEST(ImGuiDeskPages, FeatureMapMatchesCyrexWorkflows)
{
    using namespace truetest::ui::desk;
    EXPECT_TRUE(desk_page_contains(DeskPage::orderflow, DeskPanel::orderflow_canvas));
    EXPECT_TRUE(desk_page_contains(DeskPage::orderflow, DeskPanel::activity_blotter));
    EXPECT_FALSE(desk_page_contains(DeskPage::orderflow, DeskPanel::equity));
    EXPECT_TRUE(desk_page_contains(DeskPage::liquidity, DeskPanel::liquidity_heatmap));
    EXPECT_TRUE(desk_page_contains(DeskPage::structure, DeskPanel::tpo_profile));
    EXPECT_TRUE(desk_page_contains(DeskPage::structure, DeskPanel::volume_profile));
    EXPECT_TRUE(desk_page_contains(DeskPage::markets, DeskPanel::funding));
    EXPECT_TRUE(desk_page_contains(DeskPage::markets, DeskPanel::correlation));
    EXPECT_TRUE(desk_page_contains(DeskPage::operations, DeskPanel::risk));
    EXPECT_TRUE(desk_page_contains(DeskPage::operations, DeskPanel::health));
}

// Supersedes the docs/internal/imgui-desk-design.md §4 width-only
// "14/68/18" breakdown with footprint.md §2.3's more precise, area-based
// percentages (12% watchlist, 17% right rail, 12% bottom blotter, 60-65%
// footprint+CVD canvas, DOM ~70%/selected-context ~30% of the right rail).
TEST(ImGuiDeskLayout, OrderflowGeometryMatchesFootprintSpec)
{
    using namespace truetest::ui::desk;
    const auto geometry = desk_layout_geometry(DeskPage::orderflow);
    const auto fractions = desk_layout_fractions(geometry);

    EXPECT_NEAR(fractions.left_width_fraction, 0.12f, 0.01f);
    EXPECT_NEAR(fractions.right_width_fraction, 0.17f, 0.01f);
    EXPECT_NEAR(fractions.bottom_height_fraction, 0.12f, 0.01f);

    EXPECT_GE(fractions.primary_area_fraction, 0.60f);
    EXPECT_LE(fractions.primary_area_fraction, 0.65f);

    EXPECT_NEAR(fractions.right_top_area_fraction, 0.70f, 0.01f);
    EXPECT_NEAR(fractions.right_bottom_area_fraction, 0.30f, 0.01f);
}

TEST(ImGuiDeskLayout, OptionalSplitsAreExplicitAndValid)
{
    using namespace truetest::ui::desk;
    for (const auto page : desk_pages)
    {
        const auto geometry = desk_layout_geometry(page);
        EXPECT_GE(geometry.right_ratio, 0.0f);
        EXPECT_LT(geometry.right_ratio, 0.5f);
        EXPECT_GE(geometry.bottom_ratio, 0.0f);
        EXPECT_LE(geometry.bottom_ratio, 0.25f);
        EXPECT_GE(geometry.left_ratio, 0.0f);
        EXPECT_LT(geometry.left_ratio, 0.35f);
        EXPECT_GT(geometry.right_bottom_ratio, 0.0f);
        EXPECT_LT(geometry.right_bottom_ratio, 1.0f);
        const auto assignments = desk_page_assignments(page);
        ASSERT_FALSE(assignments.empty());
        EXPECT_TRUE(desk_panel_visible_in_focus(page, assignments.front().panel));
    }
}

TEST(ImGuiDeskLayout, KpiColumnsAdaptWithoutDroppingMetrics)
{
    using truetest::ui::desk::kpi_column_count;
    EXPECT_EQ(kpi_column_count(1760.0f, 9), 9u);
    EXPECT_EQ(kpi_column_count(940.0f, 9), 6u);
    EXPECT_EQ(kpi_column_count(620.0f, 9), 3u);
    EXPECT_EQ(kpi_column_count(420.0f, 9), 2u);
    EXPECT_EQ(kpi_column_count(120.0f, 9), 1u);
    EXPECT_EQ(kpi_column_count(1200.0f, 0), 0u);
}

TEST(ImGuiDeskLayout, VersionedPersistenceCannotRestoreLegacyPages)
{
    using namespace truetest::ui::desk;
    EXPECT_EQ(desk_layout_version, 3u);
    EXPECT_STREQ(desk_layout_ini_filename, "truetest_desk_v3.ini");
    EXPECT_TRUE(should_seed_default_layout(false, false));
    EXPECT_FALSE(should_seed_default_layout(true, false));
    EXPECT_FALSE(should_keep_inactive_dockspace(false));
    EXPECT_TRUE(should_keep_inactive_dockspace(true));
}

TEST(ImGuiDeskCommands, SearchIsCaseInsensitiveAndOperatorShortcutsRequireBareKeys)
{
    using namespace truetest::ui::desk;
    // desk_commands (6 entries): 0=WORKSPACE MONITOR, 1=RESET LAYOUT,
    // 2=TOGGLE DEMO DATA, 3=FOCUS PRIMARY, 4=TOGGLE LAYOUT LOCK,
    // 5=TOGGLE DENSITY.
    EXPECT_TRUE(desk_command_matches(desk_commands[0], "monitor"));
    EXPECT_TRUE(desk_command_matches(desk_commands[0], "RISK"));
    EXPECT_FALSE(desk_command_matches(desk_commands[0], "funding"));
    EXPECT_TRUE(desk_command_matches(desk_commands[4], "lock"));
    EXPECT_TRUE(desk_command_matches(desk_commands[5], "comfortable"));
    EXPECT_TRUE(operator_shortcut_allowed(false, false, false, false,
                                          false, false, false));
    EXPECT_FALSE(operator_shortcut_allowed(true, false, false, false,
                                           false, false, false));
    EXPECT_FALSE(operator_shortcut_allowed(false, false, false, true,
                                           false, false, false));
    EXPECT_FALSE(operator_shortcut_allowed(false, false, false, false,
                                           false, true, false));
}

TEST(ImGuiDeskDemo, FixtureIsDeterministicBoundedAndExplicit)
{
    const auto first = truetest::ui::desk::make_demo_research_presentation();
    const auto second = truetest::ui::desk::make_demo_research_presentation();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->state, truetest::ui::desk::DeskDataState::demo);
    EXPECT_EQ(first->version, second->version);
    ASSERT_EQ(first->footprint.size(), second->footprint.size());
    ASSERT_FALSE(first->footprint.empty());
    EXPECT_DOUBLE_EQ(first->footprint.front().close, second->footprint.front().close);
    EXPECT_LE(first->footprint.size(), 120u);
    EXPECT_LE(first->heatmap.size(), 12'000u);
    EXPECT_EQ(first->heatmap.size(),
              static_cast<std::size_t>(first->heatmap_columns) * first->heatmap_rows);
    EXPECT_GT(first->heatmap_end_ms, first->heatmap_start_ms);
    EXPECT_GT(first->heatmap_max_price, first->heatmap_min_price);
    for (const auto& surface : first->surfaces)
        EXPECT_EQ(surface.state, truetest::ui::desk::DeskDataState::demo);
}

TEST(ImGuiDeskDemo, SurfaceStatusDefaultsUnavailableForHonestPartialWiring)
{
    using namespace truetest::ui::desk;
    ResearchPresentation partial;
    partial.state = DeskDataState::live;
    partial.footprint.push_back({});
    EXPECT_EQ(research_surface_status(partial, ResearchSurface::footprint).state,
              DeskDataState::unavailable);
    partial.surfaces[static_cast<std::size_t>(ResearchSurface::footprint)].state
        = DeskDataState::live;
    EXPECT_EQ(research_surface_status(partial, ResearchSurface::footprint).state,
              DeskDataState::live);
}

TEST(ImGuiDeskDemo, DenseResearchRenderBudgetIsHardBounded)
{
    using namespace truetest::ui::desk;
    EXPECT_EQ(research_rendered_cell_count(0), 0u);
    EXPECT_EQ(research_rendered_cell_count(max_visible_research_cells),
              max_visible_research_cells);
    EXPECT_LE(research_rendered_cell_count(12'001), max_visible_research_cells);
    EXPECT_LE(research_rendered_cell_count(1'000'000), max_visible_research_cells);
    EXPECT_EQ(research_render_stride(24'000), 2u);
    EXPECT_EQ(research_rendered_cell_count(100, 0), 0u);
}

TEST(ImGuiBuildMetadata, FormatsConfigureTimeIdentityAndUnknownValuesHonestly)
{
    const truetest::ui::desk::DeskBuildInfo info{
        .version = "0.1.0",
        .git_sha = "abc123",
        .git_state_at_configure = "dirty",
        .configured_at_utc = "2026-08-02T12:00:00Z",
        .build_type = "",
        .compiler = "GNU 16",
        .target = "engine_shadow",
        .live_orders_compiled = false,
    };
    const auto text = truetest::ui::desk::format_build_identity(info);
    EXPECT_NE(text.find("git sha at configure: abc123"), std::string::npos);
    EXPECT_NE(text.find("worktree at configure: dirty"), std::string::npos);
    EXPECT_NE(text.find("build type: unknown"), std::string::npos);
    EXPECT_NE(text.find("target: engine_shadow"), std::string::npos);
    EXPECT_NE(text.find("live orders compiled: no"), std::string::npos);
}

} // namespace
