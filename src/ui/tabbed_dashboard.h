#pragma once
#ifdef HAS_RICH_TUI

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
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
    ~TabbedDashboard();

    TabbedDashboard(const TabbedDashboard&) = delete;
    TabbedDashboard& operator=(const TabbedDashboard&) = delete;

    void start();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void render_loop();
    void handle_input();
    void draw_chrome(int width, int height, int active);

    std::shared_ptr<ConsoleDashboard>   data_;
    snapshot_fn                         snap_fn_;
    std::chrono::milliseconds           tick_;

    std::vector<std::unique_ptr<IPanel>> panels_;
    std::vector<std::string>             tab_names_;

    std::atomic<int>  active_tab_{0};
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace truetest::ui

#endif // HAS_RICH_TUI
