#include <gtest/gtest.h>

#include "ui/desk/about_model.h"
#include "ui/desk/desk_command_model.h"
#include "ui/desk/desk_context.h"
#include "ui/desk/desk_layout_model.h"
#include "ui/desk/research_views.h"

#include "ui/desk/desk_capabilities.h"

#include <algorithm>
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
    // v4: all 7 pages active (orderflow/liquidity/structure/markets under
    // MARKET, plus operations/diagnostics/research).
    EXPECT_EQ(labels.size(), 7u);
    EXPECT_EQ(dockspaces.size(), 7u);
    EXPECT_EQ(focus_dockspaces.size(), 7u);
}

TEST(ImGuiDeskPages, DefaultsToMarketFootprintAndSwitchesDeterministically)
{
    using truetest::ui::desk::DeskPage;
    truetest::ui::desk::DeskPageController controller;
    EXPECT_EQ(controller.active_page(), DeskPage::orderflow);
    EXPECT_FALSE(controller.select(DeskPage::orderflow));
    EXPECT_TRUE(controller.select(DeskPage::operations));
    EXPECT_EQ(controller.active_page(), DeskPage::operations);
    controller.request_layout_reset();
    ASSERT_TRUE(controller.has_layout_request());
    EXPECT_EQ(*controller.consume_layout_request(), DeskPage::operations);
    EXPECT_FALSE(controller.consume_layout_request().has_value());
}

TEST(ImGuiDeskWorkspaces, EveryPageMapsToExactlyOneWorkspace)
{
    using namespace truetest::ui::desk;
    EXPECT_EQ(desk_workspace_of(DeskPage::orderflow), DeskWorkspace::market);
    EXPECT_EQ(desk_workspace_of(DeskPage::liquidity), DeskWorkspace::market);
    EXPECT_EQ(desk_workspace_of(DeskPage::structure), DeskWorkspace::market);
    EXPECT_EQ(desk_workspace_of(DeskPage::markets), DeskWorkspace::market);
    EXPECT_EQ(desk_workspace_of(DeskPage::operations), DeskWorkspace::operations);
    EXPECT_EQ(desk_workspace_of(DeskPage::diagnostics), DeskWorkspace::diagnostics);
    EXPECT_EQ(desk_workspace_of(DeskPage::research), DeskWorkspace::research);

    // Every page in desk_pages must be reachable from exactly one workspace's
    // page list, and every workspace's default page must round-trip.
    for (const auto page : desk_pages)
    {
        const auto workspace = desk_workspace_of(page);
        const auto pages = desk_workspace_pages(workspace);
        EXPECT_NE(std::find(pages.begin(), pages.end(), page), pages.end());
    }
    EXPECT_EQ(desk_workspace_default_page(DeskWorkspace::market), DeskPage::orderflow);
    EXPECT_EQ(desk_workspace_default_page(DeskWorkspace::operations), DeskPage::operations);
    EXPECT_EQ(desk_workspace_default_page(DeskWorkspace::diagnostics), DeskPage::diagnostics);
    EXPECT_EQ(desk_workspace_default_page(DeskWorkspace::research), DeskPage::research);
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

    // v4: every page is active, so every declared DeskPanel launches exactly
    // once across the whole desk_pages set (no dormant panels left).
    for (std::size_t i = 0; i < desk_panel_count; ++i)
        EXPECT_EQ(launched[i], 1) << i;
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
    EXPECT_TRUE(desk_page_contains(DeskPage::diagnostics, DeskPanel::debug));
    EXPECT_TRUE(desk_page_contains(DeskPage::research, DeskPanel::research_setup));
    // Safety Status (draw_safety_strip) and the Market metric band are
    // always-visible strips, not dockable DeskPanel windows — they have no
    // desk_page_assignments entry by design (see desk_app.cpp draw_frame()).
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
    EXPECT_EQ(desk_layout_version, 4u);
    EXPECT_STREQ(desk_layout_ini_filename, "truetest_desk_v4.ini");
    EXPECT_TRUE(should_seed_default_layout(false, false));
    EXPECT_FALSE(should_seed_default_layout(true, false));
    EXPECT_FALSE(should_keep_inactive_dockspace(false));
    EXPECT_TRUE(should_keep_inactive_dockspace(true));
}

TEST(ImGuiDeskCommands, SearchIsCaseInsensitiveAndOperatorShortcutsRequireBareKeys)
{
    using namespace truetest::ui::desk;
    // desk_commands (13 entries): 0=WORKSPACE MARKET, 1=MARKET·FOOTPRINT,
    // 2=MARKET·LIQUIDITY, 3=MARKET·STRUCTURE, 4=MARKET·CROSS-MARKET,
    // 5=WORKSPACE OPERATIONS, 6=WORKSPACE DIAGNOSTICS, 7=WORKSPACE RESEARCH,
    // 8=RESET LAYOUT, 9=TOGGLE DEMO DATA, 10=FOCUS PRIMARY,
    // 11=TOGGLE LAYOUT LOCK, 12=TOGGLE DENSITY.
    ASSERT_EQ(desk_commands.size(), 13u);
    EXPECT_TRUE(desk_command_matches(desk_commands[0], "market"));
    EXPECT_EQ(desk_commands[0].page, DeskPage::orderflow);
    EXPECT_TRUE(desk_command_matches(desk_commands[1], "footprint"));
    EXPECT_EQ(desk_commands[1].page, DeskPage::orderflow);
    EXPECT_EQ(desk_commands[2].page, DeskPage::liquidity);
    EXPECT_EQ(desk_commands[3].page, DeskPage::structure);
    EXPECT_EQ(desk_commands[4].page, DeskPage::markets);
    EXPECT_TRUE(desk_command_matches(desk_commands[5], "risk"));
    EXPECT_EQ(desk_commands[5].page, DeskPage::operations);
    EXPECT_TRUE(desk_command_matches(desk_commands[6], "rings"));
    EXPECT_EQ(desk_commands[6].page, DeskPage::diagnostics);
    EXPECT_TRUE(desk_command_matches(desk_commands[7], "monte carlo"));
    EXPECT_EQ(desk_commands[7].page, DeskPage::research);
    EXPECT_FALSE(desk_command_matches(desk_commands[0], "funding"));
    EXPECT_TRUE(desk_command_matches(desk_commands[11], "lock"));
    EXPECT_TRUE(desk_command_matches(desk_commands[12], "comfortable"));
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

TEST(ImGuiDeskProvenance, MixedSourcesRequiresBothADemoAndARealSurface)
{
    using namespace truetest::ui::desk;
    ResearchPresentation presentation;
    // All-demo: never mixed.
    for (auto& surface : presentation.surfaces)
        surface.state = DeskDataState::demo;
    EXPECT_FALSE(research_presentation_has_mixed_sources(presentation));

    // All-live: never mixed.
    for (auto& surface : presentation.surfaces)
        surface.state = DeskDataState::live;
    EXPECT_FALSE(research_presentation_has_mixed_sources(presentation));

    // unavailable/error siblings never count as a competing source class.
    presentation.surfaces[0].state = DeskDataState::live;
    presentation.surfaces[1].state = DeskDataState::unavailable;
    presentation.surfaces[2].state = DeskDataState::error;
    for (std::size_t i = 3; i < presentation.surfaces.size(); ++i)
        presentation.surfaces[i].state = DeskDataState::unavailable;
    EXPECT_FALSE(research_presentation_has_mixed_sources(presentation));

    // §8.1's concrete example: one live footprint alongside demo siblings.
    presentation.surfaces[static_cast<std::size_t>(ResearchSurface::footprint)].state
        = DeskDataState::live;
    presentation.surfaces[static_cast<std::size_t>(ResearchSurface::dom)].state
        = DeskDataState::demo;
    EXPECT_TRUE(research_presentation_has_mixed_sources(presentation));

    // stale still counts as "real" provenance for mixing purposes (it was a
    // real surface that went stale, not a demo one).
    ResearchPresentation stale_vs_demo;
    stale_vs_demo.surfaces[0].state = DeskDataState::stale;
    stale_vs_demo.surfaces[1].state = DeskDataState::demo;
    EXPECT_TRUE(research_presentation_has_mixed_sources(stale_vs_demo));
}

TEST(ImGuiDeskCapabilities, NeverInfersAvailabilityFromAbsentData)
{
    using namespace truetest::ui::desk;
    const auto no_snapshot = derive_desk_capabilities(
        /*has_snapshot=*/false, /*snap=*/nullptr,
        /*pause=*/false, /*flatten=*/false, /*kill=*/false,
        /*research_present=*/false);
    EXPECT_FALSE(no_snapshot.snapshot_available);
    EXPECT_FALSE(no_snapshot.pause_available);
    EXPECT_FALSE(no_snapshot.flatten_available);
    EXPECT_FALSE(no_snapshot.kill_available);
    EXPECT_FALSE(no_snapshot.debug_telemetry_available);
    EXPECT_FALSE(no_snapshot.questdb_active);
    EXPECT_FALSE(no_snapshot.research_surface_available);
    // Roadmap seams: always false until an actual seam exists — never
    // inferred from any other field.
    EXPECT_FALSE(no_snapshot.research_report_available);
    EXPECT_FALSE(no_snapshot.research_launcher_available);
    EXPECT_FALSE(no_snapshot.research_resolved_config_available);

    truetest::ui::dashboard_snapshot snap;
    snap.debug.has_debug = true;
    snap.health.questdb.active = true;
    const auto with_snapshot = derive_desk_capabilities(
        true, &snap, /*pause=*/true, /*flatten=*/true, /*kill=*/false,
        /*research_present=*/true);
    EXPECT_TRUE(with_snapshot.snapshot_available);
    EXPECT_TRUE(with_snapshot.pause_available);
    EXPECT_TRUE(with_snapshot.flatten_available);
    EXPECT_FALSE(with_snapshot.kill_available);
    EXPECT_TRUE(with_snapshot.debug_telemetry_available);
    EXPECT_TRUE(with_snapshot.questdb_active);
    EXPECT_TRUE(with_snapshot.research_surface_available);
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
