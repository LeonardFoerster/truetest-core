#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_layout_model.h"
#include "ui/desk/desk_context.h"
#include "ui/desk/footprint_live_source.h"
#include "ui/desk/footprint_panel_state.h"
#include "ui/desk/monitor_model.h"
#include "ui/desk/research_views.h"
#include "ui/operator_actions.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace truetest::ui {
class ConsoleDashboard;
}

namespace truetest::ui::desk {

using snapshot_fn = std::function<bool(dashboard_snapshot&)>;

// In-process ImGui trading desk. Own thread, polls snapshot_fn off hot path.
// Monitor panels + operator controls (pause / flatten / kill with confirm).
class DeskApp
{
public:
    DeskApp(snapshot_fn snap_fn,
            std::shared_ptr<truetest::ui::ConsoleDashboard> data = nullptr,
            std::chrono::milliseconds tick = std::chrono::milliseconds{100});

    ~DeskApp();

    DeskApp(const DeskApp&) = delete;
    DeskApp& operator=(const DeskApp&) = delete;

    void set_actions(operator_actions a);
    void set_title(std::string title);
    void set_research_source(research_snapshot_fn source);
    // Same effect as the "Deterministic demo data" menu toggle - call before
    // start() so headless/CLI-driven QA can reach the research surfaces.
    void set_demo_enabled(bool enabled) { demo_enabled_ = enabled; }
    // Wires a real footprint.md §2.1-§2.2 venue pipeline. When set, the
    // footprint surface's presentation comes from live_footprint_->poll()
    // instead of the demo aggregator - every OTHER research surface (DOM,
    // watchlist, ...) still falls back to demo/unavailable independently,
    // since ResearchSurfaceStatus is tracked per-surface (imgui.md: honest
    // partial wiring, never a blanket LIVE/DEMO label). Call before start().
    void set_live_footprint_source(std::shared_ptr<FootprintLiveSource> source);

    bool start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void render_loop();
    void apply_fonts(float content_scale);

    void draw_frame(const dashboard_snapshot* snap, bool has_snap);
    void draw_menu_bar(const dashboard_snapshot* snap, bool has_snap);
    void draw_page_switcher();
    void draw_top_chrome(const dashboard_snapshot* snap, bool has_snap);
    void draw_halt_banner(const dashboard_snapshot& snap);
    void draw_help_overlay();
    void draw_command_palette();
    void draw_confirm_modal();
    void draw_toast();

    void handle_hotkeys();
    void toggle_focus_mode();
    void execute_command(std::size_t command_index);
    const ResearchPresentation* active_research_view() const;
    // Rebuilds demo_research_->footprint from footprint_demo_.aggregator
    // (footprint.md §2.2/§2.3 pipeline exercised end-to-end by the demo
    // fixture). Call after construction and whenever a toolbar setting
    // changes footprint_demo_.settings.
    void refresh_demo_footprint();
    // Same idea as refresh_demo_footprint(), sourced from
    // live_footprint_->poll() instead - overwrites only the footprint
    // field/status/surface entry, leaving every other surface as whatever
    // make_demo_research_presentation() published. No-op when
    // live_footprint_ is unset. Same safe-call-site discipline as
    // refresh_demo_footprint() (see draw_frame()'s top and render_loop()'s
    // poll block) - never call mid-frame after `research` is captured.
    void refresh_live_footprint();
    void show_toast(std::string msg,
                    std::chrono::milliseconds ttl = std::chrono::seconds{3});
    // Runs body(), logging (and swallowing) any exception it throws in the
    // consistent "[desk] <label> threw..." shape render_loop() wants at each
    // of its guarded call sites; returns false when body threw so the caller
    // can apply its own fallback/toast. Never call across a frame boundary -
    // same discipline as the sites that use it (render_loop()'s poll block
    // and its frame-draw block).
    bool guarded_call(const char* label, const std::function<void()>& body);

    snapshot_fn snap_fn_;
    std::shared_ptr<truetest::ui::ConsoleDashboard> data_;
    std::chrono::milliseconds tick_;
    operator_actions actions_;
    research_snapshot_fn research_fn_;
    research_view_handle external_research_;
    research_view_handle demo_research_;
    FootprintDemoState footprint_demo_;
    std::shared_ptr<FootprintLiveSource> live_footprint_;
    // Set when a toolbar edit changes an aggregation-affecting footprint
    // setting; consumed at the top of the NEXT draw_frame(), never
    // mid-frame - see refresh_demo_footprint()'s call site for why.
    bool footprint_needs_reaggregate_ = false;
    std::string title_ = "TrueTest Desk";

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> start_ok_{false};

    enum class ConfirmKind { none, flatten, kill };
    ConfirmKind confirm_ = ConfirmKind::none;
    std::string toast_;
    std::chrono::steady_clock::time_point toast_until_{};

    bool show_help_ = false;
    bool show_about_ = false;
    bool show_imgui_metrics_ = false;
    bool show_command_palette_ = false;
    bool demo_enabled_ = false;
    bool focus_mode_ = false;
    std::string pre_focus_ini_;
    bool layout_locked_ = true;
    DeskDensity density_ = DeskDensity::compact;
    DeskLinkContext context_;
    std::array<char, 96> command_query_{};
    int command_selection_ = 0;
    DeskPageController page_controller_;
    std::array<bool, static_cast<std::size_t>(DeskPage::count)> page_layout_resolved_{};
    std::array<bool, static_cast<std::size_t>(DeskPage::count)> focus_layout_resolved_{};
    int  fill_filter_ = 0; // 0=all 1=buy 2=sell
    MonitorTelemetry monitor_telemetry_;
    std::vector<std::size_t> visible_fill_rows_;
    // Consecutive per-frame draw exceptions (handle_hotkeys/draw_frame/
    // draw_command_palette). Reset to 0 on any clean frame; render_loop()
    // shuts the desk thread down (never the process) once this crosses
    // kMaxConsecutiveFrameErrors — see render_loop() for why ImGui itself
    // isn't safely recoverable mid-frame, only between frames.
    int consecutive_frame_errors_ = 0;
};

} // namespace truetest::ui::desk

#endif // HAS_IMGUI_DESK
