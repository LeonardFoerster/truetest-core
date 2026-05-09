#pragma once
#ifdef HAS_RICH_TUI

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace truetest::ui {

class ConsoleDashboard;
class IPanel;

struct dashboard_snapshot;

// Render-thread-side snapshot fetcher. The engine fills a
// dashboard_snapshot under its own lock and returns; the rich TUI never
// reaches into engine internals directly. Returning false means "no snapshot
// available yet" (e.g. engine still constructing).
using snapshot_fn = std::function<bool(dashboard_snapshot&)>;

// Optional operator-control hooks. The TUI calls these from its input
// handler in response to hotkeys (`p`, `F`, `K`). Each hook is allowed
// to be null — the TUI shows a "no-op" toast when the action isn't
// available (e.g. backtest mode without a kill switch).
struct operator_actions
{
    std::function<void()>      pause_toggle;       // `p`
    std::function<bool()>      pause_state;        // for the status overlay
    std::function<void()>      flatten;            // `F` (with confirm)
    std::function<bool(std::chrono::milliseconds deadline)> kill;  // `K` (with confirm)
};

// ncurses-backed multi-tab live dashboard. Owns terminal init/teardown
// and the 100 ms render loop. Reads streaming_stats and the recent-event
// ring directly from `data`; reads structured tables (positions, lots,
// open orders, fills, risk) via `snap_fn`.
class TabbedDashboard
{
public:
    TabbedDashboard(std::shared_ptr<ConsoleDashboard> data,
                    snapshot_fn snap_fn,
                    std::chrono::milliseconds tick = std::chrono::milliseconds{100});

    void set_actions(operator_actions a) { actions_ = std::move(a); }
    ~TabbedDashboard();

    TabbedDashboard(const TabbedDashboard&) = delete;
    TabbedDashboard& operator=(const TabbedDashboard&) = delete;

    // Optional theme — chooses the color palette installed at start().
    // Default (`auto`) picks based on whether the terminal advertises
    // dark-on-light vs the standard dark background; `dark` is the
    // existing palette; `light` swaps fg/bg-friendly colors; `hicontrast`
    // boosts saturation for projector/screenshare use.
    enum class theme { dark, light, hicontrast };
    void set_theme(theme t) { theme_ = t; }

    void start();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void render_loop();
    void handle_input();
    void draw_chrome(int width, int height, int active);

    // Persistent one-line summary rendered above every tab. Pulls from
    // the streaming_stats atomics + the snapshot's risk view; nothing
    // tab-specific lives here. Empty snap = limited fallback.
    void draw_status_bar(int width,
                         const dashboard_snapshot* snap);

    // Modal "y/n" prompt rendered on top of the active panel when an
    // operator hotkey (F/K) requests a destructive action.
    void draw_confirm_overlay(int width, int height);

    // Full-screen modal listing every hotkey by category. Toggled by
    // `?`; Esc / `?` / `q` dismisses. Doesn't pause the engine.
    void draw_help_overlay(int width, int height);

    // Bottom-right transient message (2.5s TTL) for non-modal feedback —
    // pause toggles, "kill fired", "no action wired" etc.
    void draw_toast(int width, int height);
    void set_toast(const std::string& msg);

    // Full-width red overlay row painted on top of the chrome row when
    // streaming_stats::halt_flag is set. Drawn last in render_loop so it
    // covers tabs/status — the halt is the only thing that matters once
    // it fires. Rings the terminal bell once on rising edge.
    void draw_halt_banner(int width);

    // Cheap state digest for frame-skip. Captures status-bar atomics +
    // active tab + a few snapshot scalars whose change implies the
    // visible UI changed. Conservative — when in doubt, re-render.
    std::uint64_t compute_render_digest(int active,
                                        const dashboard_snapshot* snap);

    std::shared_ptr<ConsoleDashboard>   data_;
    snapshot_fn                         snap_fn_;
    std::chrono::milliseconds           tick_;

    std::vector<std::unique_ptr<IPanel>> panels_;
    std::vector<std::string>             tab_names_;
    std::vector<std::size_t>             tab_badges_;   // counts shown in tab labels
    std::vector<std::size_t>             tab_badges_prev_;
    std::vector<std::chrono::steady_clock::time_point> tab_flash_until_;

    operator_actions actions_{};

    // Confirm overlay state. When non-zero, render_loop draws a centered
    // "y/n" prompt and blocks tab/quit input until resolved. Set by `F`
    // / `K` hotkeys; cleared by 'y' (run action) or 'n' / Esc / any other.
    enum class confirm_kind { none, flatten, kill };
    std::atomic<int> pending_confirm_{static_cast<int>(confirm_kind::none)};

    // Toast queue: short ring of recent operator-feedback messages.
    // Each entry has a timestamp so the panel can fade old ones. Newest
    // first. Atomic last_toast_ms_ retained as the "most recent" marker
    // for the frame-skip digest.
    struct toast_entry { std::string msg; long long ts_ms = 0; };
    std::atomic<long long>  last_toast_ms_{0};
    std::vector<toast_entry> toasts_;
    std::mutex               toast_mu_;
    static constexpr std::size_t kToastQueueCap = 6;

    std::atomic<int>  active_tab_{0};
    std::atomic<bool> running_{false};
    std::thread       thread_;

    // Rising-edge tracker for the halt-banner bell. Set once when we
    // first observe halt_flag=true; never cleared (halt is terminal in
    // live mode — even if it weren't, the operator only needs to be
    // alerted once per halt event).
    std::atomic<bool> halt_bell_fired_{false};

    // ── Comfort/usability state ─────────────────────────────────────
    // `?`     toggles a full-screen help overlay (paused engine UI but
    //         still receives input to dismiss).
    // Space   freezes UI updates (engine continues; render skipped).
    // theme   swappable color palette installed at start().
    std::atomic<bool> show_help_{false};
    std::atomic<bool> ui_frozen_{false};
    theme             theme_{theme::dark};

    // Process-start time for the status bar's uptime indicator.
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace truetest::ui

#endif // HAS_RICH_TUI
