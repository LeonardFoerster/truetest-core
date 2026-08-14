#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_app.h"
#include "ui/desk/desk_layout.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_command_model.h"
#include "ui/desk/panels/about_dialog.h"
#include "ui/desk/panels/activity_panel.h"
#include "ui/desk/panels/market_panels.h"
#include "ui/desk/panels/research_panels.h"
#include "ui/desk/panels/status_panels.h"
#include "ui/desk/footprint_presentation_bridge.h"
#include "ui/console_dashboard.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include "implot.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>

namespace truetest::ui::desk {

namespace {

void glfw_error_callback(int code, const char* desc)
{
    std::fprintf(stderr, "[desk] GLFW error %d: %s\n", code, desc ? desc : "");
}

const char* glfw_platform_name(int platform)
{
    switch (platform)
    {
    case GLFW_PLATFORM_WAYLAND:
        return "wayland";
    case GLFW_PLATFORM_X11:
        return "x11";
    case GLFW_PLATFORM_WIN32:
        return "win32";
    case GLFW_PLATFORM_COCOA:
        return "cocoa";
    case GLFW_PLATFORM_NULL:
        return "null";
    default:
        return "unknown";
    }
}

// Desk render_loop runs on a worker thread (engine owns main). GLFW's Wayland
// path loads libdecor for CSD; libdecor-gtk fails to init off the main thread
// ("Failed to load plugin 'libdecor-gtk.so': failed to init") and KWin often
// never maps a usable toplevel — window "launches" but nothing is visible.
// Prefer X11 (XWayland) when available; override with TRUETEST_DESK_PLATFORM=
// x11|wayland|any.
bool glfw_init_for_desk()
{
    const char* pref = std::getenv("TRUETEST_DESK_PLATFORM");
    if (!pref)
        pref = "x11";

    auto try_platform = [](int platform) -> bool {
        glfwInitHint(GLFW_PLATFORM, platform);
        if (glfwInit())
            return true;
        // GLFW requires terminate before a second init attempt.
        glfwTerminate();
        return false;
    };

    if (std::strcmp(pref, "wayland") == 0)
    {
        if (try_platform(GLFW_PLATFORM_WAYLAND))
            return true;
        std::fprintf(stderr,
                     "[desk] Wayland init failed (TRUETEST_DESK_PLATFORM=wayland); "
                     "trying default\n");
        return glfwInit();
    }
    if (std::strcmp(pref, "any") == 0)
        return glfwInit();

    // Default and explicit "x11": prefer X11 to dodge libdecor-gtk.
    if (try_platform(GLFW_PLATFORM_X11))
        return true;
    std::fprintf(stderr,
                 "[desk] X11/XWayland init failed; falling back to GLFW default "
                 "(Wayland may show libdecor-gtk warnings and a blank window)\n");
    return glfwInit();
}

bool danger_btn(const char* label)
{
    theme::danger_button_style(true);
    const bool hit = ImGui::Button(label);
    theme::danger_button_style_pop(true);
    return hit;
}

std::filesystem::path executable_asset_path(const char* filename)
{
#if defined(__linux__)
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error)
        return executable.parent_path() / "desk_assets" / "fonts" / filename;
#endif
    return std::filesystem::path{"desk_assets"} / "fonts" / filename;
}

void draw_waiting_window(DeskPanel panel, const char* title)
{
    if (ImGui::Begin(desk_window_name(panel)))
    {
        theme::panel_meta(title, nullptr, DeskDataState::unavailable, 0);
        ImGui::TextColored(theme::tx_mid(), "Waiting for a coherent engine snapshot.");
        ImGui::TextColored(theme::tx_faint(),
                           "Unknown values are never interpreted as a safe or zero state.");
    }
    ImGui::End();
}

} // namespace

DeskApp::DeskApp(snapshot_fn snap_fn,
                 std::shared_ptr<truetest::ui::ConsoleDashboard> data,
                 std::chrono::milliseconds tick)
    : snap_fn_(std::move(snap_fn))
    , data_(std::move(data))
    , tick_(tick)
{
}

DeskApp::~DeskApp() { stop(); }

void DeskApp::set_actions(operator_actions a) { actions_ = std::move(a); }

void DeskApp::set_research_source(research_snapshot_fn source)
{
    research_fn_ = std::move(source);
}

void DeskApp::set_title(std::string title)
{
    if (!title.empty())
        title_ = std::move(title);
}

void DeskApp::show_toast(std::string msg, std::chrono::milliseconds ttl)
{
    toast_ = std::move(msg);
    toast_until_ = std::chrono::steady_clock::now() + ttl;
}

bool DeskApp::guarded_call(const char* label, const std::function<void()>& body)
{
    try
    {
        body();
        return true;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[desk] %s threw: %s\n", label, e.what());
        return false;
    }
    catch (...)
    {
        std::fprintf(stderr, "[desk] %s threw a non-std exception\n", label);
        return false;
    }
}

bool DeskApp::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return is_running();

    start_ok_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { render_loop(); });

    for (int i = 0; i < 150 && running_.load(std::memory_order_acquire); ++i)
    {
        if (start_ok_.load(std::memory_order_acquire))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return running_.load(std::memory_order_acquire);
}

void DeskApp::stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
    {
        if (thread_.joinable())
            thread_.join();
        return;
    }
    if (thread_.joinable())
        thread_.join();
}

void DeskApp::apply_fonts(float content_scale)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static ImVector<ImWchar> glyph_ranges;
    glyph_ranges.clear();
    ImFontGlyphRangesBuilder glyph_builder;
    glyph_builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    glyph_builder.AddText("—–…→←·×ρμ−≥≤│");
    glyph_builder.BuildRanges(&glyph_ranges);
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = true;
    cfg.GlyphRanges = glyph_ranges.Data;
    const float body_px = 13.5f * std::max(content_scale, 1.0f);
    const float mono_px = 12.5f * std::max(content_scale, 1.0f);
    // Bundled IBM Plex is deterministic across workstations. System faces are
    // retained only as a recovery path for incomplete installations.
    ImFont* ui_font = io.Fonts->AddFontDefault(&cfg);
    const std::array<std::string, 5> ui_candidates = {
        executable_asset_path("IBMPlexSans-Regular.ttf").string(),
        "src/ui/desk/assets/fonts/IBMPlexSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    for (const auto& path : ui_candidates)
    {
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            std::fclose(f);
            if (ImFont* loaded = io.Fonts->AddFontFromFileTTF(path.c_str(), body_px, &cfg))
                ui_font = loaded;
            break;
        }
    }

    ImFont* mono_font = nullptr;
    ImFontConfig mono_cfg = cfg;
    const std::array<std::string, 5> mono_candidates = {
        executable_asset_path("IBMPlexMono-Regular.ttf").string(),
        "src/ui/desk/assets/fonts/IBMPlexMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    };
    for (const auto& path : mono_candidates)
    {
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            std::fclose(f);
            mono_font = io.Fonts->AddFontFromFileTTF(path.c_str(), mono_px, &mono_cfg);
            break;
        }
    }
    io.FontDefault = ui_font;
    theme::set_mono_font(mono_font);
    io.Fonts->Build();
}

void DeskApp::render_loop()
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfw_init_for_desk())
    {
        std::fprintf(stderr, "[desk] glfwInit failed — desk disabled\n");
        running_.store(false, std::memory_order_release);
        return;
    }

    const int platform = glfwGetPlatform();
    std::fprintf(stderr, "[desk] GLFW platform: %s\n", glfw_platform_name(platform));
    if (platform == GLFW_PLATFORM_WAYLAND)
    {
        std::fprintf(stderr,
                     "[desk] note: native Wayland may print libdecor-gtk warnings "
                     "and hide the window when the UI runs off-main-thread. "
                     "Unset TRUETEST_DESK_PLATFORM or set it to x11 if the desk "
                     "is blank.\n");
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // Maximize after first map — GLFW_MAXIMIZED at create is flaky on some
    // Wayland compositors (zero-size configure / never shown).
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
#if defined(GLFW_X11_CLASS_NAME)
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "truetest_desk");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "truetest_desk");
#endif
#if defined(GLFW_WAYLAND_APP_ID)
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "truetest.desk");
#endif

    GLFWwindow* window = glfwCreateWindow(1760, 1020, title_.c_str(), nullptr, nullptr);
    if (!window)
    {
        std::fprintf(stderr, "[desk] glfwCreateWindow failed — desk disabled\n");
        glfwTerminate();
        running_.store(false, std::memory_order_release);
        return;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwShowWindow(window);
    glfwFocusWindow(window);
    glfwMaximizeWindow(window);

    {
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::fprintf(stderr,
                     "[desk] OpenGL %s / %s  framebuffer %dx%d  (close window to exit)\n",
                     vendor ? vendor : "?",
                     renderer ? renderer : "?",
                     fb_w,
                     fb_h);
        if (fb_w <= 0 || fb_h <= 0)
        {
            std::fprintf(stderr,
                         "[desk] warning: zero framebuffer — compositor did not "
                         "map the window; try TRUETEST_DESK_PLATFORM=x11\n");
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // Defends the render-loop catch block below: EndFrame() already calls
    // ImGui's internal ErrorRecoveryTryToRecoverState() when
    // ConfigErrorRecovery is on (default true, set explicitly here for
    // clarity), which rebalances any Begin/PushID/PushStyleColor stacks
    // left open by a mid-frame exception. Left at its own default
    // (ConfigErrorRecoveryEnableAssert=true), that recovery still calls
    // IM_ASSERT on the way out - an abort in assert-enabled builds, exactly
    // the crash the catch block exists to prevent. Disabling just the
    // assert keeps the stack-rebalancing recovery and the debug log line,
    // without defeating render_loop()'s "one dropped frame, not a crash"
    // contract for this specific class of fault.
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.IniFilename = desk_layout_ini_filename;

    theme::apply();
    float content_scale_x = 1.0f;
    float content_scale_y = 1.0f;
    glfwGetWindowContentScale(window, &content_scale_x, &content_scale_y);
    const float content_scale = std::clamp(std::max(content_scale_x, content_scale_y),
                                           1.0f, 2.5f);
    theme::set_ui_scale(content_scale);
    if (content_scale > 1.0f)
        ImGui::GetStyle().ScaleAllSizes(content_scale);
    apply_fonts(content_scale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    start_ok_.store(true, std::memory_order_release);

    dashboard_snapshot snap{};
    demo_research_ = make_demo_research_presentation();
    refresh_demo_footprint();
    refresh_live_footprint();
    bool has_snap = false;
    auto next_poll = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire) && !glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_poll)
        {
            // snap_fn_/research_fn_ are the one place engine-adjacent code
            // runs on this thread. They execute outside ImGui's frame scope,
            // so a thrown exception here is cleanly recoverable: log, fall
            // back to "no data this tick", toast, and keep the desk running.
            // Separate try/catch per call - not one block wrapping both -
            // so a research_fn_ fault doesn't discard a snap_fn_ result
            // that already succeeded moments earlier in the same tick.
            if (!guarded_call("snapshot callback", [&] {
                    if (snap_fn_)
                    {
                        has_snap = snap_fn_(snap);
                        if (has_snap)
                            monitor_telemetry_.merge(snap, data_.get(), now);
                    }
                }))
            {
                has_snap = false;
                show_toast("Snapshot callback error — see stderr");
            }

            if (!guarded_call("research callback", [&] {
                    external_research_ = research_fn_ ? research_fn_() : research_view_handle{};
                }))
            {
                external_research_ = research_view_handle{};
                show_toast("Research callback error — see stderr");
            }
            refresh_live_footprint(); // safe here - before this iteration's draw_frame() captures `research`
            next_poll = now + tick_;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Dear ImGui is not exception-safe mid-frame (per its own FAQ): a
        // throw here can leave Begin/End or ID-stack state unbalanced. We
        // can't safely recover *within* the corrupted frame, so on catch we
        // skip presenting this frame entirely and let EndFrame() below do
        // the real recovery: with ConfigErrorRecovery on (set at init,
        // above) it calls ImGui's own ErrorRecoveryTryToRecoverState()
        // internally, rebalancing any stack a mid-frame throw left open, so
        // the next NewFrame() starts clean rather than merely not-asserting.
        // Escalates to a clean thread shutdown if frame exceptions keep
        // happening rather than risk compounding corruption forever anyway.
        // A single transient throw still just costs one dropped frame; the
        // engine itself is never touched by this thread.
        const bool frame_ok = guarded_call("frame draw", [&] {
            handle_hotkeys();
            draw_frame(has_snap ? &snap : nullptr, has_snap);
            draw_command_palette();
        });

        if (!frame_ok)
        {
            ++consecutive_frame_errors_;
            show_toast("Frame draw error — see stderr");
            try { ImGui::EndFrame(); } catch (...) {}

            static constexpr int kMaxConsecutiveFrameErrors = 5;
            if (consecutive_frame_errors_ >= kMaxConsecutiveFrameErrors)
            {
                std::fprintf(stderr,
                             "[desk] %d consecutive frame errors — shutting down desk "
                             "thread (engine keeps running headless)\n",
                             consecutive_frame_errors_);
                running_.store(false, std::memory_order_release);
                break;
            }
            continue; // don't present a possibly-corrupted frame
        }
        consecutive_frame_errors_ = 0;

        ImGui::Render();
        int dw = 0, dh = 0;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        const ImVec4 clear = theme::bg0();
        glClearColor(clear.x, clear.y, clear.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    running_.store(false, std::memory_order_release);
}

void DeskApp::handle_hotkeys()
{
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
        show_help_ = !show_help_;

    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.KeyAlt && !io.KeySuper
        && ImGui::IsKeyPressed(ImGuiKey_K, false))
    {
        show_command_palette_ = true;
        command_query_.fill('\0');
        command_selection_ = 0;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false))
        toggle_focus_mode();

    // Don't steal keys while typing in a field.
    if (io.WantTextInput || show_command_palette_ || confirm_ != ConfirmKind::none)
        return;

    const bool bare_key = operator_shortcut_allowed(
        io.KeyCtrl, io.KeyAlt, io.KeySuper, io.KeyShift, io.WantTextInput,
        show_command_palette_, confirm_ != ConfirmKind::none);

    if (bare_key && ImGui::IsKeyPressed(ImGuiKey_P, false) && actions_.pause_toggle)
    {
        actions_.pause_toggle();
        show_toast(actions_.pause_state && actions_.pause_state() ? "Paused" : "Resumed");
    }
    if (bare_key && ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        if (actions_.flatten)
            confirm_ = ConfirmKind::flatten;
        else
            show_toast("Flatten unavailable");
    }
    if (bare_key && ImGui::IsKeyPressed(ImGuiKey_K, false))
    {
        if (actions_.kill)
            confirm_ = ConfirmKind::kill;
        else
            show_toast("Kill switch unavailable");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        if (confirm_ != ConfirmKind::none)
            confirm_ = ConfirmKind::none;
        else if (show_help_)
            show_help_ = false;
    }
}

void DeskApp::toggle_focus_mode()
{
    if (!focus_mode_)
    {
        std::size_t ini_size = 0;
        const char* ini = ImGui::SaveIniSettingsToMemory(&ini_size);
        pre_focus_ini_.assign(ini, ini_size);
        focus_mode_ = true;
        page_controller_.request_layout_reset();
        return;
    }

    const auto page = page_controller_.active_page();
    ImGui::DockBuilderRemoveNode(ImGui::GetID(desk_focus_dockspace_name(page)));
    if (!pre_focus_ini_.empty())
        ImGui::LoadIniSettingsFromMemory(pre_focus_ini_.data(), pre_focus_ini_.size());
    focus_mode_ = false;
    focus_layout_resolved_[static_cast<std::size_t>(page)] = false;
    pre_focus_ini_.clear();
}

const ResearchPresentation* DeskApp::active_research_view() const
{
    if (external_research_)
        return external_research_.get();
    if (demo_enabled_ && demo_research_)
        return demo_research_.get();
    return nullptr;
}

void DeskApp::refresh_demo_footprint()
{
    if (!demo_research_)
        return;
    // demo_research_ is an immutable published snapshot (research_view_handle
    // = shared_ptr<const ResearchPresentation>) - copy, mutate the copy,
    // then swap the pointer, same "cold worker builds, atomic swap publishes"
    // pattern footprint.md §2.2 describes for the real service.
    auto view = std::make_shared<ResearchPresentation>(*demo_research_);
    FootprintPresentationOptions opts;
    opts.quote_units = footprint_demo_.settings.quote_units;
    view->footprint = to_footprint_bar_views(footprint_demo_.aggregator, opts);
    // footprint.md §2.2's own definition of raw-frame replay - "use the same
    // parser and aggregator ... perform no REST requests" - is exactly what
    // the demo fixture does, so REPLAY is the honest status here (not LIVE).
    view->footprint_status = truetest::footprint::data_status::replay;
    auto& surface = view->surfaces[static_cast<std::size_t>(ResearchSurface::footprint)];
    surface.state = DeskDataState::demo;
    surface.source = "demo aggregator replay";
    ++surface.version;
    demo_research_ = view;
}

void DeskApp::set_live_footprint_source(std::shared_ptr<FootprintLiveSource> source)
{
    live_footprint_ = std::move(source);
}

void DeskApp::refresh_live_footprint()
{
    if (!live_footprint_ || !demo_research_)
        return;
    // Overwrite only the footprint field/status/surface entry - every
    // other surface keeps whatever make_demo_research_presentation()
    // published, so DOM/watchlist/etc. stay honestly labeled DEMO DATA
    // rather than going dark just because footprint went live.
    auto view = std::make_shared<ResearchPresentation>(*demo_research_);
    auto live_view = live_footprint_->poll();
    view->footprint = live_view->footprint;
    view->footprint_status = live_view->footprint_status;
    view->surfaces[static_cast<std::size_t>(ResearchSurface::footprint)] =
        live_view->surfaces[static_cast<std::size_t>(ResearchSurface::footprint)];
    demo_research_ = view;
}

void DeskApp::execute_command(std::size_t command_index)
{
    if (command_index >= desk_commands.size())
        return;
    const auto& command = desk_commands[command_index];
    switch (command.kind)
    {
    case DeskCommandKind::select_page:
        page_controller_.select(command.page);
        break;
    case DeskCommandKind::reset_layout:
        page_controller_.request_layout_reset();
        break;
    case DeskCommandKind::toggle_demo:
        demo_enabled_ = !demo_enabled_;
        show_toast(demo_enabled_ ? "Deterministic DEMO DATA enabled" : "Demo data disabled");
        break;
    case DeskCommandKind::toggle_focus:
        toggle_focus_mode();
        break;
    case DeskCommandKind::toggle_layout_lock:
        layout_locked_ = !layout_locked_;
        show_toast(layout_locked_ ? "Layout locked" : "Layout unlocked");
        break;
    case DeskCommandKind::toggle_density:
        density_ = density_ == DeskDensity::compact
            ? DeskDensity::comfortable : DeskDensity::compact;
        show_toast(density_ == DeskDensity::compact
            ? "Compact density" : "Comfortable density");
        break;
    }
}

void DeskApp::draw_command_palette()
{
    if (!show_command_palette_)
        return;
    ImGui::OpenPopup("Desk command palette");
    ImGui::SetNextWindowSize(ImVec2(theme::dp(620), theme::dp(360)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.35f));
    if (ImGui::BeginPopupModal("Desk command palette", &show_command_palette_,
                               ImGuiWindowFlags_NoResize))
    {
        ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##desk_command", "Type a workspace or command…",
                                 command_query_.data(), command_query_.size());
        std::array<std::size_t, desk_commands.size()> matches{};
        std::size_t match_count = 0;
        const std::string_view query{command_query_.data()};
        for (std::size_t i = 0; i < desk_commands.size(); ++i)
            if (desk_command_matches(desk_commands[i], query))
                matches[match_count++] = i;
        if (match_count == 0)
            command_selection_ = 0;
        else
        {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
                command_selection_ = (command_selection_ + 1) % static_cast<int>(match_count);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
                command_selection_ = (command_selection_ + static_cast<int>(match_count) - 1)
                    % static_cast<int>(match_count);
            command_selection_ = std::clamp(command_selection_, 0,
                                             static_cast<int>(match_count) - 1);
        }
        ImGui::Separator();
        for (std::size_t row = 0; row < match_count; ++row)
        {
            const auto& command = desk_commands[matches[row]];
            ImGui::PushID(static_cast<int>(matches[row]));
            const bool selected = static_cast<int>(row) == command_selection_;
            if (ImGui::Selectable(command.label, selected, ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0, theme::dp(32))))
            {
                execute_command(matches[row]);
                show_command_palette_ = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(theme::dp(255.0f));
            ImGui::TextColored(theme::tx_lo(), "%s", command.hint);
            ImGui::PopID();
        }
        if (match_count > 0 && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
        {
            execute_command(matches[static_cast<std::size_t>(command_selection_)]);
            show_command_palette_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            show_command_palette_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DeskApp::draw_menu_bar(const dashboard_snapshot* snap, bool has_snap)
{
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("Desk"))
    {
        if (ImGui::MenuItem("Help", "F1", show_help_))
            show_help_ = !show_help_;
        if (ImGui::MenuItem("About TrueTest", nullptr, show_about_))
            show_about_ = !show_about_;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Layout"))
    {
        if (ImGui::MenuItem("Reset current page"))
            page_controller_.request_layout_reset();
        if (ImGui::MenuItem("Focus primary", "F11", focus_mode_))
            toggle_focus_mode();
        ImGui::MenuItem("Lock layout", nullptr, &layout_locked_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::MenuItem("Deterministic demo data", nullptr, demo_enabled_))
            demo_enabled_ = !demo_enabled_;
        const bool comfortable = density_ == DeskDensity::comfortable;
        if (ImGui::MenuItem("Comfortable density", nullptr, comfortable))
            density_ = comfortable ? DeskDensity::compact : DeskDensity::comfortable;
        ImGui::MenuItem("ImGui metrics", nullptr, &show_imgui_metrics_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Ops"))
    {
        const bool can_pause = static_cast<bool>(actions_.pause_toggle);
        const bool paused = actions_.pause_state ? actions_.pause_state() : false;
        if (ImGui::MenuItem(paused ? "Resume" : "Pause", "P", false, can_pause) && can_pause)
            actions_.pause_toggle();
        if (ImGui::MenuItem("Flatten…", "F", false, static_cast<bool>(actions_.flatten)))
            confirm_ = ConfirmKind::flatten;
        if (ImGui::MenuItem("Kill switch…", "K", false, static_cast<bool>(actions_.kill)))
            confirm_ = ConfirmKind::kill;
        ImGui::EndMenu();
    }

    char menu_status[96];
    if (has_snap && monitor_telemetry_.available()
        && monitor_telemetry_.rate_available())
        std::snprintf(menu_status, sizeof(menu_status), "%.0f FPS  ·  %.1f ev/s",
                      ImGui::GetIO().Framerate, snap->health.rate_ev_per_sec);
    else if (has_snap)
        std::snprintf(menu_status, sizeof(menu_status), "%.0f FPS  ·  rate N/A",
                      ImGui::GetIO().Framerate);
    else
        std::snprintf(menu_status, sizeof(menu_status), "%.0f FPS  ·  waiting",
                      ImGui::GetIO().Framerate);
    const float status_x = ImGui::GetWindowWidth()
        - ImGui::CalcTextSize(menu_status).x - ImGui::GetStyle().WindowPadding.x;
    if (status_x > ImGui::GetCursorPosX() + 24.0f)
    {
        ImGui::SameLine(status_x);
        ImGui::TextColored(theme::tx_lo(), "%s", menu_status);
    }

    ImGui::EndMenuBar();
}

void DeskApp::draw_page_switcher()
{
    // Single-page desk for now: no point showing a one-item tab strip.
    // Re-adding pages to desk_pages makes this switcher reappear for free.
    if (desk_pages.size() <= 1)
        return;

    const bool narrow = ImGui::GetContentRegionAvail().x < theme::dp(900.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg1());
    ImGui::BeginChild("page_switcher", ImVec2(0, theme::dp(theme::kStripHeight)), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!narrow)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::tx_faint(), "WORKSPACE");
        ImGui::SameLine(0, 16);
    }

    for (const auto page : desk_pages)
    {
        const bool active = page_controller_.active_page() == page;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              active ? theme::accent_dim() : theme::bg2());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              active ? theme::accent_dim() : theme::bg3());
        ImGui::PushStyleColor(ImGuiCol_Text,
                              active ? theme::accent() : theme::tx_mid());
        if (ImGui::Button(desk_page_label(page), ImVec2(theme::dp(112.0f), 0)))
            page_controller_.select(page);
        ImGui::PopStyleColor(3);
        if (page != desk_pages.back())
            ImGui::SameLine(0, 6);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DeskApp::draw_top_chrome(const dashboard_snapshot* snap, bool has_snap)
{
    const char* target = (has_snap && snap && !snap->debug.target.empty())
                             ? snap->debug.target.c_str()
                             : "—";
    const char* mode = (has_snap && snap && !snap->debug.mode.empty())
                           ? snap->debug.mode.c_str()
                           : "—";
    const bool halted = has_snap && snap && snap->risk.halted;
    const bool paused = actions_.pause_state ? actions_.pause_state() : false;

    const bool narrow = ImGui::GetContentRegionAvail().x < theme::dp(760.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg0());
    ImGui::BeginChild("operator_chrome", ImVec2(0, theme::dp(narrow ? 70.0f : 38.0f)), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const auto draw_state = [&] {
        const bool live = std::strcmp(target, "engine_live") == 0
            || std::strcmp(target, "live") == 0;
        const bool shadow = std::strcmp(target, "engine_shadow") == 0
            || std::strcmp(target, "shadow") == 0;
        theme::badge(target, theme::mode_color(target),
                     live ? theme::danger_dim()
                          : (shadow ? theme::info_dim() : theme::accent_dim()));
        ImGui::SameLine();
        theme::status_badge(mode, theme::StatusTone::neutral);
        if (halted)
        {
            ImGui::SameLine();
            theme::status_badge("HALTED", theme::StatusTone::halted);
        }
        if (paused)
        {
            ImGui::SameLine();
            theme::status_badge("PAUSED", theme::StatusTone::warning);
        }
        ImGui::SameLine();
        theme::badge(context_.symbol.c_str(), theme::data_link(), theme::info_dim());
        if (demo_enabled_ && !external_research_)
        {
            ImGui::SameLine();
            theme::status_badge("DEMO DATA", theme::StatusTone::warning);
        }
        draw_toast();
    };

    const auto draw_controls = [&] {
        if (ImGui::Button("Command  Ctrl+K"))
        {
            show_command_palette_ = true;
            command_query_.fill('\0');
        }
        ImGui::SameLine();
        const bool paused_now = actions_.pause_state ? actions_.pause_state() : false;
        if (ImGui::Button(paused_now ? "Resume  P" : "Pause  P"))
        {
            if (actions_.pause_toggle)
                actions_.pause_toggle();
            else
                show_toast("Pause unavailable");
        }
        ImGui::SameLine();
        ImGui::TextColored(theme::tx_faint(), "|");
        ImGui::SameLine();
        if (danger_btn("Flatten  F"))
        {
            if (actions_.flatten)
                confirm_ = ConfirmKind::flatten;
            else
                show_toast("Flatten unavailable");
        }
        ImGui::SameLine();
        if (danger_btn("Kill  K"))
        {
            if (actions_.kill)
                confirm_ = ConfirmKind::kill;
            else
                show_toast("Kill switch unavailable");
        }
    };

    if (narrow)
    {
        draw_state();
        draw_controls();
    }
    else if (ImGui::BeginTable("operator_chrome_columns", 2,
                               ImGuiTableFlags_SizingFixedFit
                                   | ImGuiTableFlags_NoSavedSettings))
    {
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, theme::dp(430.0f));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_state();
        ImGui::TableNextColumn();
        draw_controls();
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DeskApp::draw_toast()
{
    if (toast_.empty())
        return;
    if (std::chrono::steady_clock::now() >= toast_until_)
    {
        toast_.clear();
        return;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::warn());
    ImGui::Text("  │  %s", toast_.c_str());
    ImGui::PopStyleColor();
}

void DeskApp::draw_halt_banner(const dashboard_snapshot& snap)
{
    if (!snap.risk.halted)
        return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::danger_dim());
    ImGui::BeginChild("halt_banner", ImVec2(0, theme::dp(40)), true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
    ImGui::TextUnformatted("  RISK HALT  ·  new orders blocked  ·  halt is terminal — process restart required");
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DeskApp::draw_help_overlay()
{
    if (!show_help_)
        return;
    ImGui::SetNextWindowSize(ImVec2(theme::dp(420), 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Keyboard shortcuts", &show_help_,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Operator");
        ImGui::BulletText("P — pause / resume strategies");
        ImGui::BulletText("F — flatten (confirm)");
        ImGui::BulletText("K — kill switch (confirm)");
        ImGui::BulletText("Esc — cancel confirm / close help");
        ImGui::Separator();
        ImGui::TextUnformatted("Desk");
        ImGui::BulletText("F1 — this help");
        ImGui::BulletText("Ctrl+K — command palette");
        ImGui::BulletText("F11 — focus/restore primary surface");
        ImGui::BulletText("Use the top switch to move between workspaces");
        ImGui::BulletText("Unlock Layout > Lock layout, then drag titles to re-dock");
        ImGui::BulletText("Layout saved to %s", desk_layout_ini_filename);
        ImGui::Separator();
        ImGui::TextColored(theme::tx_lo(),
                           "Halt is terminal. Flatten/kill never auto-retry.");
        if (ImGui::Button("Close", ImVec2(theme::dp(120), 0)))
            show_help_ = false;
    }
    ImGui::End();
}

void DeskApp::draw_confirm_modal()
{
    if (confirm_ == ConfirmKind::none)
        return;

    ImGui::OpenPopup("Confirm destructive action");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Confirm destructive action", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (confirm_ == ConfirmKind::flatten)
        {
            ImGui::TextColored(theme::warn(), "FLATTEN");
            ImGui::TextWrapped(
                "Request flatten on all open positions. Confirm only if you intend "
                "to reduce risk now.");
        }
        else if (confirm_ == ConfirmKind::kill)
        {
            ImGui::TextColored(theme::down(), "KILL SWITCH");
            ImGui::TextWrapped(
                "Cancel-all + flatten with a 5s deadline via the provider kill switch. "
                "Loud, non-retrying, fail-closed.");
        }

        ImGui::Spacing();
        if (danger_btn("Yes — execute") || ImGui::IsKeyPressed(ImGuiKey_Y, false))
        {
            if (confirm_ == ConfirmKind::flatten && actions_.flatten)
            {
                actions_.flatten();
                show_toast("Flatten requested");
            }
            else if (confirm_ == ConfirmKind::kill && actions_.kill)
            {
                const bool ok = actions_.kill(std::chrono::milliseconds{5000});
                show_toast(ok ? "Kill switch completed" : "Kill switch failed / timed out");
            }
            confirm_ = ConfirmKind::none;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            confirm_ = ConfirmKind::none;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DeskApp::draw_frame(const dashboard_snapshot* snap, bool has_snap)
{
    // Safe point to republish demo_research_: nothing from a prior frame
    // still holds the old raw research pointer (draw_frame doesn't persist
    // it across calls), and this frame hasn't captured `research` yet.
    if (footprint_needs_reaggregate_)
    {
        footprint_demo_.reaggregate();
        refresh_demo_footprint();
        footprint_needs_reaggregate_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags root =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar
        | ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(theme::dp(8.0f), theme::dp(6.0f)));
    ImGui::Begin("##TrueTestDeskRoot", nullptr, root);
    draw_menu_bar(snap, has_snap);
    draw_page_switcher();
    draw_top_chrome(snap, has_snap);

    if (has_snap && snap)
        draw_halt_banner(*snap);

    // Always-visible P&L/risk header — deliberately rendered outside the
    // dockspace (not a dockable ImGui::Begin window) so it can never be
    // dragged away, closed, or buried under another tab.
    if (page_controller_.active_page() == DeskPage::monitor)
    {
        static const dashboard_snapshot empty_monitor_snapshot{};
        panels::draw_account_strip(snap ? *snap : empty_monitor_snapshot, monitor_telemetry_);
    }

    const DeskPage active_page = page_controller_.active_page();
    const ImGuiID normal_dock_id = ImGui::GetID(desk_page_dockspace_name(active_page));
    const ImGuiID focus_dock_id = ImGui::GetID(desk_focus_dockspace_name(active_page));
    const ImGuiID dock_id = focus_mode_ ? focus_dock_id : normal_dock_id;
    const auto page_index = static_cast<std::size_t>(active_page);
    const bool layout_existed_before_submission =
        ImGui::DockBuilderGetNode(dock_id) != nullptr;
    const ImVec2 dock_size = ImGui::GetContentRegionAvail();
    if (!focus_mode_)
    {
        // A process can close while focus mode owns the primary window. That
        // transient dockspace is never a valid startup layout; recover the
        // normal page deterministically instead of restoring an empty shell.
        if (ImGuiDockNode* focus_node = ImGui::DockBuilderGetNode(focus_dock_id);
            focus_node && focus_node->Windows.Size > 0)
        {
            ImGui::DockBuilderRemoveNode(focus_dock_id);
            apply_desk_page_layout(normal_dock_id, dock_size.x, dock_size.y,
                                   active_page, false);
            page_layout_resolved_[page_index] = true;
        }
    }
    const auto reset_request = page_controller_.consume_layout_request();
    const bool reset_active_page = reset_request && *reset_request == active_page;
    bool& active_layout_resolved = focus_mode_
        ? focus_layout_resolved_[page_index] : page_layout_resolved_[page_index];
    if (!active_layout_resolved)
    {
        active_layout_resolved = true;
        if (should_seed_default_layout(layout_existed_before_submission,
                                       reset_active_page))
            apply_desk_page_layout(dock_id, dock_size.x, dock_size.y,
                                   active_page, focus_mode_);
    }
    if (reset_request)
    {
        const auto reset_index = static_cast<std::size_t>(*reset_request);
        const ImGuiID reset_dock_id = ImGui::GetID(
            focus_mode_ ? desk_focus_dockspace_name(*reset_request)
                        : desk_page_dockspace_name(*reset_request));
        (focus_mode_ ? focus_layout_resolved_[reset_index]
                     : page_layout_resolved_[reset_index]) = true;
        apply_desk_page_layout(reset_dock_id, dock_size.x, dock_size.y,
                               *reset_request, focus_mode_);
        show_toast(focus_mode_ ? "Primary surface focused"
                               : std::string{desk_page_label(*reset_request)} + " layout reset");
    }
    // Preserve already-created inactive layouts without constructing or
    // rendering their panels. Expensive research surfaces only draw when active.
    for (const auto page : desk_pages)
    {
        if (page == active_page)
            continue;
        const ImGuiID inactive_id = ImGui::GetID(desk_page_dockspace_name(page));
        if (should_keep_inactive_dockspace(ImGui::DockBuilderGetNode(inactive_id) != nullptr))
            ImGui::DockSpace(inactive_id, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
        const ImGuiID inactive_focus_id = ImGui::GetID(desk_focus_dockspace_name(page));
        if (should_keep_inactive_dockspace(
                ImGui::DockBuilderGetNode(inactive_focus_id) != nullptr))
            ImGui::DockSpace(inactive_focus_id, ImVec2(0, 0),
                             ImGuiDockNodeFlags_KeepAliveOnly);
    }
    if (focus_mode_
        && should_keep_inactive_dockspace(ImGui::DockBuilderGetNode(normal_dock_id) != nullptr))
        ImGui::DockSpace(normal_dock_id, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
    ImGui::DockSpace(dock_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    set_desk_layout_locked(dock_id, layout_locked_);

    const ResearchPresentation* research = active_research_view();
    static const dashboard_snapshot empty_snapshot{};
    const dashboard_snapshot& current = snap ? *snap : empty_snapshot;

    switch (active_page)
    {
    case DeskPage::orderflow:
        // Do NOT reaggregate/republish synchronously here: `research` above
        // is a raw pointer into the CURRENT demo_research_ object, still
        // used by the sibling panel calls below within this same frame.
        // Reassigning demo_research_ now would dangle it mid-frame. Defer
        // to the top of the next draw_frame() instead (see there).
        if (panels::draw_orderflow_canvas_panel(research, context_,
                                                footprint_demo_.camera, footprint_demo_.settings,
                                                footprint_demo_.bounds_cache,
                                                footprint_demo_.viewport_cache))
        {
            footprint_needs_reaggregate_ = true;
        }
        if (!focus_mode_)
        {
            panels::draw_watchlist_panel(snap, research, context_, density_);
            panels::draw_dom_panel(DeskPanel::orderflow_dom, snap, research, context_, density_);
            panels::draw_selected_context_panel(snap, research, context_);
            panels::draw_activity_panel(DeskPanel::activity_blotter, snap, density_,
                                        context_.symbol.c_str());
        }
        break;
    case DeskPage::liquidity:
        panels::draw_liquidity_panel(research, context_);
        if (!focus_mode_)
        {
            panels::draw_dom_panel(DeskPanel::liquidity_dom, snap, research, context_, density_);
            panels::draw_liquidations_panel(research, context_);
            panels::draw_liquidity_tape_panel(research, context_, density_);
        }
        break;
    case DeskPage::structure:
        panels::draw_tpo_panel(research, context_);
        if (!focus_mode_)
        {
            panels::draw_volume_profile_panel(research, context_);
            panels::draw_session_context_panel(research, context_);
        }
        break;
    case DeskPage::markets:
        panels::draw_correlation_panel(research, context_);
        if (!focus_mode_)
            panels::draw_funding_panel(research, context_, density_);
        break;
    case DeskPage::operations:
        if (!snap)
        {
            draw_waiting_window(DeskPanel::equity, "ACCOUNT / EQUITY");
            if (!focus_mode_)
            {
                draw_waiting_window(DeskPanel::operations_activity, "OPERATIONS ACTIVITY");
                draw_waiting_window(DeskPanel::strategies, "STRATEGIES");
                draw_waiting_window(DeskPanel::risk, "RISK");
                draw_waiting_window(DeskPanel::health, "SYSTEM HEALTH");
            }
        }
        else
        {
            panels::draw_equity_panel(current);
            if (!focus_mode_)
            {
                panels::draw_activity_panel(DeskPanel::operations_activity, snap, density_);
                panels::draw_strategies_panel(current);
                panels::draw_risk_panel(current);
                panels::draw_health_panel(current, monitor_telemetry_);
            }
        }
        break;
    case DeskPage::monitor:
        if (!snap)
        {
            draw_waiting_window(DeskPanel::activity_blotter, "POSITIONS & ACTIVITY");
            if (!focus_mode_)
            {
                draw_waiting_window(DeskPanel::health, "SYSTEM HEALTH");
                draw_waiting_window(DeskPanel::risk, "RISK");
            }
        }
        else
        {
            panels::draw_activity_panel(DeskPanel::activity_blotter, snap, density_);
            if (!focus_mode_)
            {
                panels::draw_health_panel(current, monitor_telemetry_);
                panels::draw_risk_panel(current);
            }
        }
        break;
    case DeskPage::count:
        break;
    }

    if (show_imgui_metrics_)
        ImGui::ShowMetricsWindow(&show_imgui_metrics_);

    ImGui::End();
    ImGui::PopStyleVar();

    draw_confirm_modal();
    draw_help_overlay();
    draw_about_dialog(show_about_);
}

} // namespace truetest::ui::desk

#endif // HAS_IMGUI_DESK
