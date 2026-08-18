#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/research_workspace_panel.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_window_names.h"

#include "imgui.h"

namespace truetest::ui::desk::panels {

namespace {

void not_wired(const char* what, const char* seam)
{
    theme::status_badge("NOT WIRED", theme::StatusTone::neutral);
    ImGui::SameLine();
    ImGui::TextColored(theme::tx_mid(), "%s", what);
    ImGui::TextColored(theme::tx_faint(), "Seam needed: %s", seam);
}

void draw_setup_tab()
{
    ImGui::TextColored(theme::tx_lo(),
        "Backtest/replay/Monte Carlo configuration is driven entirely by CLI "
        "flags and JSON (--config, --preset, --dry-run, --dump-config) — see "
        "docs/reference/04-flags.md and docs/reference/01-instructions.md.");
    ImGui::Separator();

    static constexpr const char* kSections[] = {
        "Data (provider, path, symbol, format, backfill, seed)",
        "Strategy (name(s), --param, registry entries)",
        "Execution realism — backtest/shadow only (latency, impact, queue, fees)",
        "Risk / exits (balance, risk-fraction, SL/TP, exit policy)",
        "Runtime (thread preset, pinning, spin policy)",
        "Output (path/format, event logging, persistence)",
    };
    for (const char* section : kSections)
        ImGui::BulletText("%s", section);

    ImGui::Separator();
    not_wired("Resolved configuration preview (preset + config + overrides)",
              "a cold-path call into the existing dry-run/resolved-config "
              "resolver, invoked without starting an engine run");
    ImGui::Spacing();
    not_wired("In-desk backtest launch",
              "an isolated engine_backtest child-process launch API — running "
              "a heavy backtest on the attached shadow/live engine loop would "
              "violate the research/live process boundary");
}

void draw_report_tab()
{
    theme::status_badge("UNAVAILABLE", theme::StatusTone::neutral);
    ImGui::SameLine();
    ImGui::TextColored(theme::tx_mid(), "No AnalyticsReport source wired to the desk");
    ImGui::TextColored(theme::tx_faint(),
        "The desk's research callback only ever publishes market-structure "
        "ResearchPresentation data (footprint/DOM/heatmap/profile/funding/"
        "correlation), never an AnalyticsReport. Reports are produced by "
        "--output/--dump-config on a completed CLI run today.");
}

void draw_monte_carlo_tab()
{
    theme::status_badge("NOT WIRED", theme::StatusTone::neutral);
    ImGui::SameLine();
    ImGui::TextColored(theme::tx_mid(), "Monte Carlo campaigns run out-of-process only");
    ImGui::TextColored(theme::tx_faint(),
        "No campaign setup, trial count, generator/model, seed, aggregate "
        "metrics, distribution summary, or trial drill-down is surfaced to "
        "the desk. --monte-carlo/--mc-trials/--mc-parallel remain CLI-only.");
}

void draw_replay_tab()
{
    theme::status_badge("NOT WIRED", theme::StatusTone::neutral);
    ImGui::SameLine();
    ImGui::TextColored(theme::tx_mid(), "No in-desk replay control");
    ImGui::TextColored(theme::tx_faint(),
        "--replay reads an authoritative finalized event log at process "
        "start; the desk has no control to trigger or browse a replay run.");
}

} // namespace

void draw_research_workspace_panel(const DeskCapabilities& caps)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::research_setup)))
    {
        ImGui::End();
        return;
    }

    theme::section_header("RESEARCH", "Develop → Backtest/Replay/MC → Review",
                          theme::secondary());
    ImGui::TextColored(theme::tx_faint(),
        "Capabilities: launcher %s · report %s · resolved-config %s",
        caps.research_launcher_available ? "wired" : "not wired",
        caps.research_report_available ? "wired" : "not wired",
        caps.research_resolved_config_available ? "wired" : "not wired");
    ImGui::Separator();

    if (ImGui::BeginTabBar("research_workspace_tabs"))
    {
        if (ImGui::BeginTabItem("Setup"))
        {
            draw_setup_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Report"))
        {
            draw_report_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Monte Carlo"))
        {
            draw_monte_carlo_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Replay"))
        {
            draw_replay_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
