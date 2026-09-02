#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_app.h"

#include "ui/console_dashboard.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_view_model.h"
#include "ui/desk/panels/account_risk_panel.h"
#include "ui/desk/panels/account_strip.h"
#include "ui/desk/panels/fills_table.h"
#include "ui/desk/panels/health_strip.h"
#include "ui/desk/panels/instrument_panel.h"
#include "ui/desk/panels/market_watch.h"
#include "ui/desk/panels/orders_table.h"
#include "ui/desk/panels/positions_table.h"
#include "ui/desk/panels/protection_table.h"
#include "ui/desk/panels/top_bar.h"
#include "ui/desk/widgets/confirm_modal.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <utility>

namespace truetest::ui::desk {
namespace {

void glfw_error_callback(int code, const char* description)
{
    std::fprintf(stderr, "[desk] GLFW error %d: %s\n", code, description ? description : "");
}

// Prefer X11 under Wayland desktops where libdecor support can be incomplete.
// Every call in this function is made synchronously by DeskApp::start() on the
// application-main thread.
bool glfw_init_for_desk()
{
    const char* preference = std::getenv("TRUETEST_DESK_PLATFORM");
    if (!preference) preference = "x11";
    const auto try_platform = [](int platform) {
        glfwInitHint(GLFW_PLATFORM, platform);
        if (glfwInit()) return true;
        glfwTerminate();
        return false;
    };
    const auto try_any_platform = [] {
        // Init hints persist across attempts. Explicitly undo the failed
        // platform preference before falling back to GLFW auto-selection.
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
        return glfwInit() == GLFW_TRUE;
    };
    if (std::strcmp(preference, "wayland") == 0)
        return try_platform(GLFW_PLATFORM_WAYLAND) || try_any_platform();
    if (std::strcmp(preference, "any") == 0) return try_any_platform();
    return try_platform(GLFW_PLATFORM_X11) || try_any_platform();
}

std::filesystem::path executable_asset_path(const char* filename)
{
#if defined(__linux__)
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return executable.parent_path() / "desk_assets" / "fonts" / filename;
#endif
    return std::filesystem::path{"desk_assets"} / "fonts" / filename;
}

void draw_help_overlay(bool& open)
{
    if (!open) return;
    ImGui::OpenPopup("Command Center help");
    if (ImGui::BeginPopupModal("Command Center help", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("TrueTest Trading Command Center");
        ImGui::Separator();
        ImGui::BulletText(
            "Select a Market Watch, position, or order row to set the global instrument.");
        ImGui::BulletText("P toggles Pause when the existing operator hook is available.");
        ImGui::BulletText("F requests confirmed Flatten; K requests confirmed Kill.");
        ImGui::BulletText(
            "Per-position and per-order controls are visible but intentionally not wired.");
        ImGui::BulletText("Snapshot update age is not labelled as venue market-data age.");
        if (ImGui::Button("Close")) {
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace

DeskApp::DeskApp(DeskAppConfig config)
    : snapshot_fn_(std::move(config.snapshot))
    , console_(std::move(config.console))
    , tick_(config.tick > std::chrono::milliseconds::zero()
                ? config.tick
                : std::chrono::milliseconds{1})
    , actions_(std::move(config.actions))
    , trade_actions_(std::move(config.trade_actions))
    , title_(config.title.empty() ? "TrueTest Trading Command Center"
                                  : std::move(config.title))
{}

DeskApp::~DeskApp() noexcept
{
    request_stop();
    const bool owns_platform_resources =
        glfw_initialized_ || window_ || context_created_ || glfw_backend_initialized_ ||
        opengl_backend_initialized_;
    if (owns_platform_resources && owner_thread_ != std::this_thread::get_id()) {
        std::fprintf(stderr,
                     "[desk] fatal: DeskApp destroyed outside its platform owner thread\n");
        std::terminate();
    }
    shutdown();
}

void DeskApp::show_toast(std::string message, std::chrono::milliseconds ttl)
{
    toast_ = std::move(message);
    toast_until_ = std::chrono::steady_clock::now() + ttl;
}

void DeskApp::draw_toast()
{
    if (toast_.empty() || std::chrono::steady_clock::now() >= toast_until_) return;
    ImGui::SameLine();
    ImGui::TextColored(theme::warning(), "%s", toast_.c_str());
}

bool DeskApp::start()
{
    if (running_.load(std::memory_order_acquire) || window_ || context_created_)
        return false;

    owner_thread_ = std::this_thread::get_id();
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfw_init_for_desk()) {
        std::fprintf(stderr, "[desk] GLFW initialization failed; desk disabled\n");
        owner_thread_ = {};
        return false;
    }
    glfw_initialized_ = true;

    try {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        window_ = glfwCreateWindow(1760, 1020, title_.c_str(), nullptr, nullptr);
        if (!window_) {
            std::fprintf(stderr, "[desk] GLFW window creation failed; desk disabled\n");
            shutdown();
            return false;
        }
        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);

        IMGUI_CHECKVERSION();
        if (!ImGui::CreateContext()) {
            std::fprintf(stderr, "[desk] ImGui context creation failed; desk disabled\n");
            shutdown();
            return false;
        }
        context_created_ = true;
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        glfwGetWindowContentScale(window_, &scale_x, &scale_y);
        apply_fonts(std::max(scale_x, scale_y));
        theme::apply();

        if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
            std::fprintf(stderr, "[desk] ImGui GLFW backend initialization failed\n");
            shutdown();
            return false;
        }
        glfw_backend_initialized_ = true;
        if (!ImGui_ImplOpenGL3_Init("#version 130")) {
            std::fprintf(stderr, "[desk] ImGui OpenGL backend initialization failed\n");
            shutdown();
            return false;
        }
        opengl_backend_initialized_ = true;
        running_.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[desk] initialization failed: %s\n", exception.what());
    } catch (...) {
        std::fprintf(stderr, "[desk] initialization failed with an unknown exception\n");
    }
    shutdown();
    return false;
}

void DeskApp::shutdown() noexcept
{
    running_.store(false, std::memory_order_release);
    if (opengl_backend_initialized_) {
        ImGui_ImplOpenGL3_Shutdown();
        opengl_backend_initialized_ = false;
    }
    if (glfw_backend_initialized_) {
        ImGui_ImplGlfw_Shutdown();
        glfw_backend_initialized_ = false;
    }
    if (context_created_) {
        ImGui::DestroyContext();
        context_created_ = false;
    }
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    if (glfw_initialized_) {
        glfwTerminate();
        glfw_initialized_ = false;
    }
    owner_thread_ = {};
}

void DeskApp::apply_fonts(float content_scale)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    const float body_size = 13.5f * std::max(content_scale, 1.0f);
    const float mono_size = 12.5f * std::max(content_scale, 1.0f);
    ImFont* body = io.Fonts->AddFontDefault(&config);

    const std::array<std::string, 4> body_paths = {
        executable_asset_path("IBMPlexSans-Regular.ttf").string(),
        "src/ui/desk/assets/fonts/IBMPlexSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (const auto& path : body_paths) {
        if (FILE* file = std::fopen(path.c_str(), "rb")) {
            std::fclose(file);
            if (ImFont* loaded = io.Fonts->AddFontFromFileTTF(path.c_str(), body_size, &config))
                body = loaded;
            break;
        }
    }

    ImFont* mono = nullptr;
    const std::array<std::string, 4> mono_paths = {
        executable_asset_path("IBMPlexMono-Regular.ttf").string(),
        "src/ui/desk/assets/fonts/IBMPlexMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    };
    for (const auto& path : mono_paths) {
        if (FILE* file = std::fopen(path.c_str(), "rb")) {
            std::fclose(file);
            mono = io.Fonts->AddFontFromFileTTF(path.c_str(), mono_size, &config);
            break;
        }
    }
    io.FontDefault = body;
    theme::set_mono_font(mono);
    io.Fonts->Build();
}

void DeskApp::invoke_pause()
{
    if (!actions_.pause_toggle) {
        show_toast("Pause unavailable");
        return;
    }
    try {
        actions_.pause_toggle();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[desk] pause callback failed: %s\n", exception.what());
        show_toast("Pause request failed");
        return;
    } catch (...) {
        std::fprintf(stderr, "[desk] pause callback failed with an unknown exception\n");
        show_toast("Pause request failed");
        return;
    }
    const auto paused = read_pause_state();
    if (!paused) {
        show_toast("Pause toggled; state unavailable");
        return;
    }
    show_toast(*paused ? "Paused" : "Resumed");
}

void DeskApp::invoke_flatten()
{
    if (!actions_.flatten) {
        show_toast("Flatten unavailable");
        return;
    }
    try {
        actions_.flatten();
        show_toast("Flatten requested");
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[desk] flatten callback failed: %s\n", exception.what());
        show_toast("Flatten request failed");
    } catch (...) {
        std::fprintf(stderr, "[desk] flatten callback failed with an unknown exception\n");
        show_toast("Flatten request failed");
    }
}

void DeskApp::invoke_kill()
{
    if (!actions_.kill) {
        show_toast("Kill unavailable");
        return;
    }
    try {
        const bool acknowledged = actions_.kill(std::chrono::seconds(5));
        show_toast(acknowledged ? "Kill acknowledged"
                                : "Kill did not acknowledge before deadline");
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[desk] kill callback failed: %s\n", exception.what());
        show_toast("Kill request failed");
    } catch (...) {
        std::fprintf(stderr, "[desk] kill callback failed with an unknown exception\n");
        show_toast("Kill request failed");
    }
}

std::optional<bool> DeskApp::read_pause_state() noexcept
{
    if (!actions_.pause_state) return std::nullopt;
    try {
        const bool paused = actions_.pause_state();
        pause_state_failure_reported_ = false;
        return paused;
    } catch (const std::exception& exception) {
        if (!pause_state_failure_reported_)
            std::fprintf(stderr, "[desk] pause-state callback failed: %s\n", exception.what());
    } catch (...) {
        if (!pause_state_failure_reported_)
            std::fprintf(stderr,
                         "[desk] pause-state callback failed with an unknown exception\n");
    }
    pause_state_failure_reported_ = true;
    return std::nullopt;
}

void DeskApp::handle_hotkeys()
{
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) show_help_ = !show_help_;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || confirm_ != widgets::ConfirmKind::none) return;
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) invoke_pause();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false) && actions_.flatten)
        confirm_ = widgets::ConfirmKind::flatten;
    if (ImGui::IsKeyPressed(ImGuiKey_K, false) && actions_.kill)
        confirm_ = widgets::ConfirmKind::kill;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) show_help_ = false;
}

void DeskApp::draw_frame(const dashboard_snapshot* snapshot, bool has_snapshot)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr ImGuiWindowFlags root_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("TrueTest Trading Command Center", nullptr, root_flags);

    CommandCenterViewModel* view_ptr = has_snapshot && snapshot ? &view_cache_ : nullptr;
    const auto paused_state = read_pause_state();
    const auto action =
        panels::draw_top_bar(snapshot, view_ptr, paused_state.value_or(false),
                             static_cast<bool>(actions_.pause_toggle), paused_state.has_value(),
                             static_cast<bool>(actions_.flatten), static_cast<bool>(actions_.kill));
    if (action == panels::TopBarAction::pause_toggle) invoke_pause();
    if (action == panels::TopBarAction::flatten) confirm_ = widgets::ConfirmKind::flatten;
    if (action == panels::TopBarAction::kill) confirm_ = widgets::ConfirmKind::kill;
    draw_toast();

    if (!snapshot || !view_ptr) {
        ImGui::Spacing();
        ImGui::TextColored(theme::text(), "WAITING FOR COHERENT ENGINE SNAPSHOT");
        ImGui::TextColored(theme::text_faint(),
                           "Unknown values are not presented as zero or safe.");
        panels::draw_health_strip(nullptr, nullptr, false, false);
        ImGui::End();
        ImGui::PopStyleVar(2);
        draw_help_overlay(show_help_);
        return;
    }

    CommandCenterViewModel& view = *view_ptr;
    panels::draw_account_strip(*snapshot, view.account);
    const float health_height = 33.0f;
    const float available = ImGui::GetContentRegionAvail().y;
    const float blotter_height = std::clamp(available * 0.36f, 185.0f, 330.0f);
    const float main_height = std::max(180.0f, available - blotter_height - health_height - 9.0f);

    ImGui::BeginChild("command_center_main", {0.0f, main_height}, false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    constexpr ImGuiTableFlags layout_flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("command_center_columns", 3, layout_flags, {0.0f, main_height})) {
        ImGui::TableSetupColumn("watch", ImGuiTableColumnFlags_WidthFixed, 325.0f);
        ImGui::TableSetupColumn("instrument", ImGuiTableColumnFlags_WidthStretch, 570.0f);
        ImGui::TableSetupColumn("risk", ImGuiTableColumnFlags_WidthFixed, 325.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("market_watch_region", {-1.0f, main_height - 6.0f}, false);
        panels::draw_market_watch(view, state_);
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("instrument_region", {-1.0f, main_height - 6.0f}, false);
        panels::draw_instrument_panel(view, *snapshot, state_,
                                      derive_trade_action_capabilities(trade_actions_));
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginChild("account_risk_region", {-1.0f, main_height - 6.0f}, false);
        panels::draw_account_risk_panel(*snapshot, view.account);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::BeginChild("command_center_blotter", {0.0f, blotter_height}, true);
    if (ImGui::BeginTabBar("operation_blotter_tabs")) {
        if (ImGui::BeginTabItem("Positions")) {
            state_.active_blotter = DeskBlotterTab::positions;
            panels::draw_positions_table(view, state_);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Open Orders")) {
            state_.active_blotter = DeskBlotterTab::orders;
            panels::draw_orders_table(view, state_,
                                      derive_trade_action_capabilities(trade_actions_));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Protection / TP-SL")) {
            state_.active_blotter = DeskBlotterTab::protection;
            panels::draw_protection_table(view);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Recent Fills")) {
            state_.active_blotter = DeskBlotterTab::fills;
            panels::draw_fills_table(view);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    panels::draw_health_strip(snapshot, &view, monitor_telemetry_.available(),
                              monitor_telemetry_.rate_available());

    ImGui::End();
    ImGui::PopStyleVar(2);

    const auto confirmation = widgets::draw_confirm_modal(confirm_);
    if (confirmation == widgets::ConfirmResult::confirmed) {
        if (confirm_ == widgets::ConfirmKind::flatten)
            invoke_flatten();
        else if (confirm_ == widgets::ConfirmKind::kill)
            invoke_kill();
        confirm_ = widgets::ConfirmKind::none;
    } else if (confirmation == widgets::ConfirmResult::cancelled)
        confirm_ = widgets::ConfirmKind::none;
    draw_help_overlay(show_help_);
}

void DeskApp::run() noexcept
{
    if (!window_ || !opengl_backend_initialized_) return;
    if (owner_thread_ != std::this_thread::get_id()) {
        std::fprintf(stderr, "[desk] run() refused outside its platform owner thread\n");
        request_stop();
        return;
    }
    if (!running_.load(std::memory_order_acquire)) {
        shutdown();
        return;
    }

    try {
        dashboard_snapshot snapshot;
        bool has_snapshot = false;
        auto next_snapshot = std::chrono::steady_clock::now();
        while (running_.load(std::memory_order_acquire) && !glfwWindowShouldClose(window_)) {
            glfwPollEvents();
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_snapshot) {
                dashboard_snapshot candidate;
                try {
                    if (snapshot_fn_ && snapshot_fn_(candidate)) {
                        monitor_telemetry_.merge(candidate, console_.get(), now);
                        auto candidate_view = build_command_center_view_model(candidate, state_);
                        if (state_.selected_symbol.empty() &&
                            !candidate_view.market_watch.empty()) {
                            state_.selected_symbol = candidate_view.market_watch.front().symbol;
                            candidate_view.selected_symbol = state_.selected_symbol;
                        }
                        snapshot = std::move(candidate);
                        view_cache_ = std::move(candidate_view);
                        has_snapshot = true;
                    }
                } catch (const std::exception& exception) {
                    std::fprintf(stderr, "[desk] snapshot callback threw: %s\n", exception.what());
                    show_toast("Snapshot callback failed");
                } catch (...) {
                    std::fprintf(stderr, "[desk] snapshot callback threw\n");
                    show_toast("Snapshot callback failed");
                }
                next_snapshot = now + tick_;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            handle_hotkeys();
            draw_frame(has_snapshot ? &snapshot : nullptr, has_snapshot);
            ImGui::Render();
            int width = 0, height = 0;
            glfwGetFramebufferSize(window_, &width, &height);
            glViewport(0, 0, width, height);
            const ImVec4 clear = theme::background();
            glClearColor(clear.x, clear.y, clear.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window_);
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[desk] render loop failed: %s\n", exception.what());
    } catch (...) {
        std::fprintf(stderr, "[desk] render loop failed with an unknown exception\n");
    }
    shutdown();
}

}  // namespace truetest::ui::desk

#endif  // HAS_IMGUI_DESK
