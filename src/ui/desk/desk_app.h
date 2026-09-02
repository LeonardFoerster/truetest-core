#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_state.h"
#include "ui/desk/desk_trade_actions.h"
#include "ui/desk/desk_view_model.h"
#include "ui/desk/monitor_model.h"
#include "ui/desk/widgets/confirm_modal.h"
#include "ui/operator_actions.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

struct GLFWwindow;

namespace truetest::ui {
class ConsoleDashboard;
}

namespace truetest::ui::desk {

using snapshot_fn = std::function<bool(dashboard_snapshot&)>;

struct DeskAppConfig
{
    snapshot_fn snapshot;
    std::shared_ptr<truetest::ui::ConsoleDashboard> console;
    std::chrono::milliseconds tick{100};
    operator_actions actions;
    DeskTradeActions trade_actions;
    std::string title{"TrueTest Trading Command Center"};
};

// The ImGui desk is a cold-path, attended command center. It owns platform
// lifecycle, coherent snapshot polling, and UI-only selection state; panels
// receive only the immutable snapshot projection below that boundary.
class DeskApp
{
public:
    explicit DeskApp(DeskAppConfig config);
    ~DeskApp() noexcept;

    DeskApp(const DeskApp&) = delete;
    DeskApp& operator=(const DeskApp&) = delete;

    // Platform setup is synchronous. start() and run() must be called by the
    // same (application-main) thread; request_stop() is the only cross-thread
    // lifecycle operation.
    bool start();
    void run() noexcept;
    void request_stop() noexcept { running_.store(false, std::memory_order_release); }
    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void shutdown() noexcept;
    void apply_fonts(float content_scale);
    void draw_frame(const dashboard_snapshot* snapshot, bool has_snapshot);
    void handle_hotkeys();
    void show_toast(std::string message, std::chrono::milliseconds ttl = std::chrono::seconds{3});
    void draw_toast();
    void invoke_pause();
    void invoke_flatten();
    void invoke_kill();
    std::optional<bool> read_pause_state() noexcept;

    snapshot_fn snapshot_fn_;
    std::shared_ptr<truetest::ui::ConsoleDashboard> console_;
    std::chrono::milliseconds tick_;
    operator_actions actions_;
    DeskTradeActions trade_actions_;
    std::string title_;

    std::atomic<bool> running_{false};
    std::thread::id owner_thread_{};
    GLFWwindow* window_ = nullptr;
    bool glfw_initialized_ = false;
    bool context_created_ = false;
    bool glfw_backend_initialized_ = false;
    bool opengl_backend_initialized_ = false;
    DeskState state_;
    MonitorTelemetry monitor_telemetry_;

    CommandCenterViewModel view_cache_;
    bool pause_state_failure_reported_ = false;
    bool show_help_ = false;
    widgets::ConfirmKind confirm_ = widgets::ConfirmKind::none;
    std::string toast_;
    std::chrono::steady_clock::time_point toast_until_{};
};

}  // namespace truetest::ui::desk

#endif  // HAS_IMGUI_DESK
