#ifdef HAS_RICH_TUI

#include "tabbed_dashboard.h"

#include "console_dashboard.h"
#include "console_format.h"
#include "dashboard_snapshot.h"
#include "overlays.h"
#include "panels/overview_panel.h"
#include "panels/orders_panel.h"
#include "panels/positions_panel.h"
#include "panels/risk_panel.h"
#include "panels/brackets_panel.h"
#include "panels/strategy_panel.h"
#include "panels/health_panel.h"
#include "panels/debug_panel.h"
#include "panels/l2_panel.h"
#include "toast.h"
#include "tui_prefs.h"
#include "tui_style.h"

#include <ncurses.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace truetest::ui {

TabbedDashboard::TabbedDashboard(std::shared_ptr<ConsoleDashboard> data,
                                 snapshot_fn snap_fn,
                                 std::chrono::milliseconds tick)
    : data_(std::move(data))
    , snap_fn_(std::move(snap_fn))
    , tick_(tick)
{
    panels_.emplace_back(std::make_unique<OverviewPanel>());
    panels_.emplace_back(std::make_unique<PositionsPanel>());
    panels_.emplace_back(std::make_unique<OrdersPanel>());
    panels_.emplace_back(std::make_unique<BracketsPanel>());
    panels_.emplace_back(std::make_unique<L2Panel>());
    panels_.emplace_back(std::make_unique<StrategyPanel>());
    panels_.emplace_back(std::make_unique<RiskPanel>());
    panels_.emplace_back(std::make_unique<HealthPanel>());
    panels_.emplace_back(std::make_unique<DebugPanel>());
    for (auto& p : panels_) tab_names_.emplace_back(p->title());
}

TabbedDashboard::~TabbedDashboard()
{
    stop();
}

void TabbedDashboard::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    // Required for ncursesw to render the UTF-8 block chars used by
    // ascii::sparkline (▁▂▃▄▅▆▇█) and the warning glyph in RiskPanel.
    // Safe to call repeatedly; no-op if already set by the user's env.
    std::setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    // Enable mouse - clicks on the tab bar switch tabs. Disabled by
    // setting an empty mask. ALL_MOUSE_EVENTS covers presses, releases,
    // and scroll wheel; we react only to button-down on the header row.
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
    // mouseinterval(0) disables click-vs-doubleclick coalescing so the
    // single-click latency is minimal.
    mouseinterval(0);

    // Terminal-side hints - tiny per-frame savings that compound over
    // 10 Hz × N hours. leaveok stops ncurses from emitting a cursor-move
    // escape on every refresh; clearok(false) suppresses any forced
    // full-screen redraw; intrflush stops Ctrl-C from flushing the
    // input queue so signals don't disrupt our render loop.
    leaveok(stdscr, TRUE);
    clearok(stdscr, FALSE);
    intrflush(stdscr, FALSE);
    if (has_colors())
    {
        start_color();
        use_default_colors();
        // Theme palette - pair ids 1..5 are referenced everywhere.
        // dark (default): bright fg on transparent (-1) bg.
        // light: same fg colors but bg=COLOR_WHITE for readability on
        // light terminals; ncurses swaps interpretations.
        // hicontrast: BOLD fg + saturated bg for projector use.
        switch (theme_)
        {
        case theme::light:
            init_pair(1, COLOR_GREEN,  COLOR_WHITE);
            init_pair(2, COLOR_RED,    COLOR_WHITE);
            init_pair(3, COLOR_YELLOW, COLOR_WHITE);
            init_pair(4, COLOR_BLUE,   COLOR_WHITE);
            init_pair(5, COLOR_BLACK,  COLOR_WHITE);
            break;
        case theme::hicontrast:
            init_pair(1, COLOR_BLACK, COLOR_GREEN);
            init_pair(2, COLOR_WHITE, COLOR_RED);
            init_pair(3, COLOR_BLACK, COLOR_YELLOW);
            init_pair(4, COLOR_BLACK, COLOR_CYAN);
            init_pair(5, COLOR_BLACK, COLOR_WHITE);
            break;
        case theme::dark:
        default:
            init_pair(1, COLOR_GREEN,  -1);
            init_pair(2, COLOR_RED,    -1);
            init_pair(3, COLOR_YELLOW, -1);
            init_pair(4, COLOR_CYAN,   -1);
            init_pair(5, COLOR_WHITE,  -1);
            break;
        }
        // Pair 6 is the halt-banner alarm: white-on-red regardless of
        // theme. The banner is drawn over chrome and overrides whatever
        // palette is in use, so consistency wins over theme adherence.
        init_pair(6, COLOR_WHITE, COLOR_RED);
    }

    // Initialize the new semantic style system (Phase 2)
    init_colors();

    start_time_ = std::chrono::steady_clock::now();

    // Load saved preferences. Active tab survives across runs; theme
    // is sticky if it was set before. Anything missing stays at default.
    TuiPrefs ps = load_tui_prefs();
    if (ps.active_tab >= 0 && ps.active_tab < static_cast<int>(panels_.size()))
        active_tab_.store(ps.active_tab, std::memory_order_release);
    if (ps.theme >= 0 && ps.theme <= static_cast<int>(theme::hicontrast))
        theme_ = static_cast<theme>(ps.theme);

    thread_ = std::thread([this] { render_loop(); });
}

void TabbedDashboard::stop()
{
    if (!running_.exchange(false)) return;

    // Give the render thread a chance to exit cleanly
    if (thread_.joinable())
    {
        // Wait up to 1 second; if it's blocked in the filter prompt or sleep,
        // we still want to restore the terminal.
        auto start = std::chrono::steady_clock::now();
        while (thread_.joinable() &&
               (std::chrono::steady_clock::now() - start) < std::chrono::seconds(1))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (thread_.joinable())
            thread_.detach();   // last resort - don't hang the caller
    }

    // Fix #4: Always attempt to restore the terminal, even in bad cases
    endwin();

    // Persist user-visible state across runs.
    TuiPrefs ps;
    ps.active_tab = active_tab_.load(std::memory_order_acquire);
    ps.theme      = static_cast<int>(theme_);
    ps.frozen     = ui_frozen_.load(std::memory_order_acquire) ? 1 : 0;
    save_tui_prefs(ps);
}

void TabbedDashboard::handle_input()
{
    int ch = getch();
    while (ch != ERR)
    {
        // Help overlay swallows everything except its own dismiss keys.
        if (show_help_.load(std::memory_order_acquire))
        {
            if (ch == '?' || ch == 27 || ch == 'q' || ch == 'Q')
                show_help_.store(false, std::memory_order_release);
            ch = getch();
            continue;
        }

        // If a confirm overlay is up, only y/n/Esc/q matter.
        const int pc = pending_confirm_.load(std::memory_order_acquire);
        if (pc != static_cast<int>(ConfirmKind::none))
        {
            if (ch == 'y' || ch == 'Y')
            {
                if (pc == static_cast<int>(ConfirmKind::flatten) && actions_.flatten)
                {
                    actions_.flatten();
                    set_toast("flatten requested");
                }
                else if (pc == static_cast<int>(ConfirmKind::kill) && actions_.kill)
                {
                    bool ok = actions_.kill(std::chrono::milliseconds(2000));
                    set_toast(ok ? "kill switch fired"
                                 : "kill switch returned false");
                }
                pending_confirm_.store(static_cast<int>(ConfirmKind::none),
                                       std::memory_order_release);
            }
            else if (ch == 'n' || ch == 'N' || ch == 27)
            {
                pending_confirm_.store(static_cast<int>(ConfirmKind::none),
                                       std::memory_order_release);
                set_toast("cancelled");
            }
            ch = getch();
            continue;
        }

        if (ch == KEY_MOUSE)
        {
            MEVENT me;
            if (getmouse(&me) == OK)
            {
                // Tab clicks: row 0 is the chrome. Compute which tab
                // label the click landed on by walking the same name+
                // badge layout draw_chrome uses. Only react on press
                // (BUTTON1_PRESSED or BUTTON1_CLICKED).
                if (me.y == 0 &&
                    (me.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)))
                {
                    int x = 1;
                    // Use recorded geometry if available (Fix #8), otherwise fall back
                    if (!last_tab_rects_.empty() && last_tab_rects_.size() == tab_names_.size())
                    {
                        for (std::size_t i = 0; i < last_tab_rects_.size(); ++i)
                        {
                            const auto& r = last_tab_rects_[i];
                            if (me.x >= r.left && me.x < r.right)
                            {
                                active_tab_.store(static_cast<int>(i), std::memory_order_release);
                                break;
                            }
                        }
                    }
                    else
                    {
                        // Fallback to old calculation
                        for (std::size_t i = 0; i < tab_names_.size(); ++i)
                        {
                            const auto& name = tab_names_[i];
                            const std::size_t count = tab_badges_.size() > i ? tab_badges_[i] : 0;
                            const int label_w = 4 + static_cast<int>(name.size());
                            const int badge_w = (count > 0) ? static_cast<int>(std::snprintf(nullptr, 0, "[%zu]", count)) : 0;
                            const int span = label_w + badge_w + 3;
                            if (me.x >= x && me.x < x + span)
                            {
                                active_tab_.store(static_cast<int>(i), std::memory_order_release);
                                break;
                            }
                            x += span;
                        }
                    }
                }
            }
            ch = getch();
            continue;
        }
        if (ch >= '1' && ch <= '0' + static_cast<int>(panels_.size()))
        {
            active_tab_.store(ch - '1', std::memory_order_release);
        }
        else if (ch == 'q' || ch == 'Q')
        {
            running_.store(false, std::memory_order_release);
            return;
        }
        else if (ch == '\t' || ch == KEY_RIGHT)
        {
            int t = active_tab_.load(std::memory_order_acquire);
            active_tab_.store((t + 1) % static_cast<int>(panels_.size()),
                              std::memory_order_release);
            // Fix #3: ask engine for fresh data after tab change
            if (snap_fn_) { truetest::ui::dashboard_snapshot tmp; snap_fn_(tmp); }
        }
        else if (ch == KEY_BTAB || ch == KEY_LEFT)
        {
            int t = active_tab_.load(std::memory_order_acquire);
            int n = static_cast<int>(panels_.size());
            active_tab_.store((t - 1 + n) % n, std::memory_order_release);
            if (snap_fn_) { truetest::ui::dashboard_snapshot tmp; snap_fn_(tmp); }
        }
        else if (ch == 'p' || ch == 'P')
        {
            if (actions_.pause_toggle)
            {
                actions_.pause_toggle();
                bool now_paused = actions_.pause_state ? actions_.pause_state() : false;
                set_toast(now_paused ? "PAUSED - no new orders" : "resumed");
                // Fix #3: operator action -> want fresh data
                if (snap_fn_) { truetest::ui::dashboard_snapshot tmp; snap_fn_(tmp); }
            }
            else set_toast("pause not wired");
        }
        else if (ch == 'F')
        {
            if (actions_.flatten)
                pending_confirm_.store(static_cast<int>(ConfirmKind::flatten),
                                       std::memory_order_release);
            else set_toast("flatten not wired");
        }
        else if (ch == 'K')
        {
            if (actions_.kill)
                pending_confirm_.store(static_cast<int>(ConfirmKind::kill),
                                       std::memory_order_release);
            else set_toast("kill not wired");
        }
        else if (ch == '?')
        {
            show_help_.store(true, std::memory_order_release);
        }
        else if (ch == ' ')
        {
            const bool was = ui_frozen_.load(std::memory_order_acquire);
            ui_frozen_.store(!was, std::memory_order_release);
            set_toast(!was ? "UI frozen - engine still running" : "UI live");
            // Fix #3: force fresh snapshot after unfreeze
            if (snap_fn_) { truetest::ui::dashboard_snapshot tmp; snap_fn_(tmp); }
        }
        else if (ch == 'j' || ch == KEY_DOWN)
        {
            // Move row cursor down on the active panel (no-op if panel
            // doesn't support cursor).
            const int active = active_tab_.load(std::memory_order_acquire);
            if (active >= 0 && active < static_cast<int>(panels_.size()) &&
                panels_[active]->supports_cursor())
            {
                const int max = panels_[active]->cursor_max();
                const int cur = panels_[active]->cursor_row();
                if (max > 0)
                    panels_[active]->set_cursor_row(std::min(max - 1, cur + 1));
            }
        }
        else if (ch == 'k' || ch == KEY_UP)
        {
            const int active = active_tab_.load(std::memory_order_acquire);
            if (active >= 0 && active < static_cast<int>(panels_.size()) &&
                panels_[active]->supports_cursor())
            {
                const int cur = panels_[active]->cursor_row();
                panels_[active]->set_cursor_row(std::max(0, cur - 1));
            }
        }
        else if (ch == '/')
        {
            run_filter_prompt();
        }
        ch = getch();
    }
}

void TabbedDashboard::run_filter_prompt()
{
    // Enter filter input mode: show a blocking prompt at the
    // bottom of the screen, capture chars until Enter/Esc.
    // Keeps the panel rendered behind so the user sees what
    // they're filtering. Uses synchronous getch (we already
    // own the input loop - no nested loops).
    std::string buf;
    int h = 0, w = 0;
    getmaxyx(stdscr, h, w);
    // Fix #5: RAII guard so we always restore nodelay + cursor even on early return / signal
    struct PromptGuard {
        ~PromptGuard() {
            nodelay(stdscr, TRUE);
            curs_set(0);
        }
    };
    PromptGuard _guard;

    // Fix #4: Make the filter prompt responsive to shutdown (no more stuck getch on exit)
    nodelay(stdscr, TRUE);
    curs_set(1);
    for (;;)
    {
        if (!running_.load(std::memory_order_acquire))
        {
            buf.clear();
            break;   // shutdown requested - exit prompt cleanly
        }

        move(h - 1, 0); clrtoeol();
        attron(COLOR_PAIR(4) | A_BOLD);
        mvprintw(h - 1, 1, " /");
        attroff(COLOR_PAIR(4) | A_BOLD);
        mvaddstr(h - 1, 4, buf.c_str());
        refresh();

        int kc = getch();
        if (kc == ERR)
        {
            // no key - yield a bit so shutdown can be observed quickly
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (kc == 27) { buf.clear(); break; }     // Esc cancels
        if (kc == '\n' || kc == KEY_ENTER) break; // accept
        if (kc == KEY_BACKSPACE || kc == 127 || kc == 8)
        {
            if (!buf.empty()) buf.pop_back();
        }
        else if (kc >= 32 && kc < 127 &&
                 buf.size() < 64)
            buf.push_back(static_cast<char>(kc));
    }
    nodelay(stdscr, TRUE);   // ensure we leave in the expected state for the guard
    const int active = active_tab_.load(std::memory_order_acquire);
    if (active >= 0 && active < static_cast<int>(panels_.size()))
        panels_[active]->set_filter(buf);
    set_toast(buf.empty() ? "filter cleared"
                          : ("filter: " + buf));
}

void TabbedDashboard::set_toast(const std::string& msg)
{
    toasts_.set_toast(msg);
}

void TabbedDashboard::draw_confirm_overlay(int width, int height)
{
    const int pc = pending_confirm_.load(std::memory_order_acquire);
    ConfirmKind k = static_cast<ConfirmKind>(pc);
    paint_confirm_overlay(width, height, k);  // free function from overlays module
}

void TabbedDashboard::draw_help_overlay(int width, int height)
{
    paint_help_overlay(width, height,
        show_help_.load(std::memory_order_acquire));  // free function from overlays module
}

void TabbedDashboard::draw_toast(int width, int height)
{
    toasts_.draw(width, height);
}

void TabbedDashboard::draw_chrome(int width, int height, int active)
{
    // Header row: tab names + badge counts where the snapshot has them.
    // The badge gives at-a-glance "this tab has activity" cue without
    // requiring you to switch into it.
    move(0, 0); clrtoeol();
    int x = 1;
    for (std::size_t i = 0; i < tab_names_.size(); ++i)
    {
        const bool is_active = (static_cast<int>(i) == active);
        const auto& name = tab_names_[i];
        const std::size_t count = tab_badges_.size() > i ? tab_badges_[i] : 0;

        // Fix #1: stop drawing tabs if we would overflow the terminal
        int needed = 4 + static_cast<int>(name.size()) + 2;
        if (count > 0) needed += 4; // rough "[99]" estimate
        if (x + needed > width - 2)
            break;

        int tab_start = x;

        if (is_active) attron(A_REVERSE | A_BOLD);
        mvprintw(0, x, " %zu·%s ", i + 1, name.c_str());
        if (is_active) attroff(A_REVERSE | A_BOLD);
        x += 4 + static_cast<int>(name.size());

        if (count > 0)
        {
            // Flash: green-reverse for 600 ms after a count change,
            // then settle back to cyan.
            const bool flashing = (tab_flash_until_.size() > i)
                && (std::chrono::steady_clock::now() < tab_flash_until_[i]);
            const int badge_pair = flashing ? 1 : 4;   // green : cyan
            const auto extra = flashing ? (A_REVERSE | A_BOLD) : A_BOLD;
            attron(COLOR_PAIR(badge_pair) | extra);
            char b[32];
            std::snprintf(b, sizeof(b), "[%zu]", count);
            mvaddstr(0, x, b);
            attroff(COLOR_PAIR(badge_pair) | extra);
            x += static_cast<int>(std::strlen(b));
        }

        // Record the rectangle for reliable hit testing (Fix #8)
        last_tab_rects_.push_back({tab_start, x});

        x += 2;
    }

    // Status bar at row 1 - drawn by render_loop after this returns.
    // Top separator at row 2.
    mvhline(2, 0, ACS_HLINE, width);

    // Footer hint (Fix #1 + #6)
    move(height - 1, 0); clrtoeol();
    attron(A_DIM);
    safe_mvprintw(height - 1, 1, width - 2,
             " 1-%zu tabs · Tab/←→ · ? help · Space · p pause · F/K · q ",
             tab_names_.size());
    attroff(A_DIM);
}

void TabbedDashboard::draw_status_bar(int width,
                                      const dashboard_snapshot* snap)
{
    if (!data_) return;
    const auto& s = data_->stats();

    const auto state    = static_cast<connection_state>(
        s.state.load(std::memory_order_acquire));
    const std::int64_t  last_fp8 = s.last_price_fp8.load(std::memory_order_relaxed);
    const std::int64_t  bid_fp8  = s.best_bid_fp8.load(std::memory_order_relaxed);
    const std::int64_t  ask_fp8  = s.best_ask_fp8.load(std::memory_order_relaxed);
    const std::int64_t  pos_fp8  = s.position_qty_fp8.load(std::memory_order_relaxed);
    const std::int64_t  pnl_fp4  = s.realized_pnl_fp4.load(std::memory_order_relaxed);
    const std::int64_t  unrl_fp4 = s.unrealized_pnl_fp4.load(std::memory_order_relaxed);
    const std::int64_t  dd_fp4   = s.drawdown_fp4.load(std::memory_order_relaxed);
    const bool halted = s.halt_flag.load(std::memory_order_acquire);
    const std::uint64_t drops_total = total_ring_drops(s);

    const double last = (last_fp8 < 0) ? 0.0 : static_cast<double>(last_fp8) / 1e8;
    const double bid  = (bid_fp8  < 0) ? 0.0 : static_cast<double>(bid_fp8)  / 1e8;
    const double ask  = (ask_fp8  < 0) ? 0.0 : static_cast<double>(ask_fp8)  / 1e8;
    const double pos  = static_cast<double>(pos_fp8)  / 1e8;
    const double pnl  = static_cast<double>(pnl_fp4)  / 1e4;
    const double unrl = static_cast<double>(unrl_fp4) / 1e4;
    const double dd   = static_cast<double>(dd_fp4)   / 1e2;
    const double eq   = snap ? snap->equity : 0.0;

    // Using new semantic style system (Phase 2)
    using Color = truetest::ui::Color;

    // Deliberately NOT console_format.h's state_label()/state_color(): this
    // status bar needs terser labels ("reconn"/"HALT") to fit its tight
    // fields, vs. state_label()'s full words ("RECONNECT"/"HALTED") sized
    // for console_dashboard's wider box layout. Two label sets by design,
    // not an accidental duplicate - see AGENTS.md's dashboard_snapshot note
    // for the general pattern, but this specific case is an intentional
    // presentation difference, not drift.
    auto state_text = [](connection_state st) {
        switch (st) {
            case connection_state::idle:         return "idle";
            case connection_state::backfill:     return "backfill";
            case connection_state::waiting:      return "waiting";
            case connection_state::live:         return "LIVE";
            case connection_state::reconnecting: return "reconn";
            case connection_state::halted:       return "HALT";
            case connection_state::closed:       return "closed";
        }
        return "?";
    };
    auto get_state_color = [](connection_state st) -> Color {
        switch (st) {
            case connection_state::live:         return Color::Positive;
            case connection_state::halted:       return Color::Danger;
            case connection_state::reconnecting:
            case connection_state::backfill:
            case connection_state::waiting:      return Color::Warning;
            default:                             return Color::Neutral;
        }
    };

    int y = 1;
    move(y, 0); clrtoeol();

    // Pause background - subtle but clear visual signal across the whole bar
    const bool paused = actions_.pause_state && actions_.pause_state();
    if (paused)
    {
        set_color(Color::Warning);
        attron(A_REVERSE);
        for (int xi = 0; xi < width; ++xi) mvaddch(y, xi, ' ');
        attroff(A_REVERSE);
        unset_color(Color::Warning);
    }

    int x = 1;

    // Helper for dim label + bold value (common pattern in good trading UIs)
    auto put_field = [&](const char* lbl, const char* val, Color c, bool bold_value = true) {
        attron(A_DIM); mvaddstr(y, x, lbl); attroff(A_DIM);
        x += static_cast<int>(std::strlen(lbl));
        if (bold_value) set_color_bold(c); else set_color(c);
        mvaddstr(y, x, val);
        if (bold_value) unset_color_bold(c); else unset_color(c);
        x += static_cast<int>(std::strlen(val)) + 2;
    };

    char buf[48];

    // State (most important leftmost item)
    Color state_col = get_state_color(state);
    set_color_bold(state_col);
    mvaddstr(y, x, state_text(state));
    unset_color_bold(state_col);
    x += static_cast<int>(std::strlen(state_text(state))) + 3;

    // Market data
    if (last_fp8 >= 0) {
        std::snprintf(buf, sizeof(buf), "%.2f", last);
        put_field("last:", buf, Color::Accent, false);
    }
    if (bid_fp8 > 0 && ask_fp8 > 0) {
        std::snprintf(buf, sizeof(buf), "%.1fbp", spread_bps(bid, ask));
        put_field("spr:", buf, Color::Muted, false);
    }

    // Position
    std::snprintf(buf, sizeof(buf), "%+.4f", pos);
    put_field("pos:", buf, pos > 0 ? Color::Positive : (pos < 0 ? Color::Negative : Color::Neutral));

    // Equity
    std::snprintf(buf, sizeof(buf), "%.2f", eq);
    put_field("eq:", buf, Color::Neutral);

    // Total PnL - one of the most important numbers on screen
    std::snprintf(buf, sizeof(buf), "%+.2f", pnl + unrl);
    Color pnl_col = (pnl + unrl) > 0 ? Color::Positive : ((pnl + unrl) < 0 ? Color::Negative : Color::Neutral);
    put_field("pnl:", buf, pnl_col, true);

    // Drawdown - critical risk metric
    std::snprintf(buf, sizeof(buf), "%.2f%%", dd);
    Color dd_col = (dd <= -5.0) ? Color::Danger : (dd <= -1.0 ? Color::Warning : Color::Muted);
    put_field("dd:", buf, dd_col, true);

    // Fix #3: proper staleness tracking + indicator
    static auto last_good_snapshot = std::chrono::steady_clock::now();
    if (snap)
        last_good_snapshot = std::chrono::steady_clock::now();

    auto snap_age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_good_snapshot).count();

    if (snap_age > 1500)
    {
        set_color_bold(Color::Warning);
        mvaddstr(y, x, " stale");
        unset_color_bold(Color::Warning);
        x += 7;
    }

    if (halted) {
        set_color_bold(Color::Danger);
        attron(A_REVERSE);
        mvaddstr(y, x, " HALT ");
        attroff(A_REVERSE);
        unset_color_bold(Color::Danger);
        x += 7;
    }

    if (drops_total > 0) {
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(drops_total));
        // Inline replacement for the missing put_pair helper
        attron(A_DIM);
        mvaddstr(y, x, "drops ");
        attroff(A_DIM);
        x += 6;
        set_color_bold(Color::Danger);
        mvaddstr(y, x, buf);
        unset_color_bold(Color::Danger);
        x += static_cast<int>(std::strlen(buf)) + 2;
    }

    // ── Right side: PAUSED + Clock + Uptime ────────────────────────
    char right_buf[64] = {0};
    {
        // Wall clock (HH:MM:SS).
        const auto now_t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now_t);
#else
        localtime_r(&now_t, &tm);
#endif
        char clock_b[16];
        std::snprintf(clock_b, sizeof(clock_b), "%02d:%02d:%02d",
                      tm.tm_hour, tm.tm_min, tm.tm_sec);

        // Uptime (since dashboard started).
        char up_b[40] = {0};
        if (start_time_.time_since_epoch().count() > 0)
        {
            const auto secs =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time_).count();
            if (secs >= 3600)
                std::snprintf(up_b, sizeof(up_b), " · up %lldh%02lldm",
                              (long long)(secs/3600),
                              (long long)((secs%3600)/60));
            else if (secs >= 60)
                std::snprintf(up_b, sizeof(up_b), " · up %lldm%02llds",
                              (long long)(secs/60),
                              (long long)(secs%60));
            else
                std::snprintf(up_b, sizeof(up_b), " · up %llds",
                              (long long)secs);
        }
        const char* paused_lbl = paused ? "[PAUSED] " : "";
        std::snprintf(right_buf, sizeof(right_buf),
                      "%s%s%s", paused_lbl, clock_b, up_b);
    }
    const int rlen = static_cast<int>(std::strlen(right_buf));
    // Use the new safe right-align helper (Fix #1)
    const int rx = right_align(width, rlen, 1);
    // Never let the right side overlap the left content
    const int safe_rx = std::max(rx, x + 3);

    if (paused) {
        set_color_bold(Color::Warning);
        attron(A_REVERSE);
        safe_mvaddstr(y, safe_rx, width - safe_rx - 1, right_buf);
        attroff(A_REVERSE);
        unset_color_bold(Color::Warning);
    } else {
        attron(A_DIM);
        safe_mvaddstr(y, safe_rx, width - safe_rx - 1, right_buf);
        attroff(A_DIM);
    }
}

void TabbedDashboard::draw_halt_banner(int width)
{
    if (!data_) return;
    if (!data_->stats().halt_flag.load(std::memory_order_acquire)) return;
    if (width <= 0) return;

    constexpr int kPairAlarm = 6;

    // Rising edge: ring the bell once.
    if (!halt_bell_fired_.exchange(true, std::memory_order_acq_rel))
        ::beep();

    std::string reason = data_->shutdown_reason();
    if (reason.empty()) reason = "halt";

    // Full-width red alarm bar (Fix #2)
    std::string body = "  HALT - ";
    body += reason;
    if (static_cast<int>(body.size()) > width)
        body.resize(static_cast<std::size_t>(width));
    body.append(static_cast<std::size_t>(width) - body.size(), ' ');

    attron(COLOR_PAIR(kPairAlarm) | A_BOLD | A_BLINK);
    mvaddstr(0, 0, body.c_str());
    attroff(COLOR_PAIR(kPairAlarm) | A_BOLD | A_BLINK);

    // Re-draw the active tab number in high contrast on the red bar
    // so the operator still knows which tab they are looking at (Fix #2)
    int active = active_tab_.load(std::memory_order_acquire);
    if (active >= 0 && active < static_cast<int>(tab_names_.size()))
    {
        const auto& name = tab_names_[active];
        char tab_label[64];
        std::snprintf(tab_label, sizeof(tab_label), " %d·%s ", active + 1, name.c_str());

        attron(COLOR_PAIR(kPairAlarm) | A_BOLD | A_REVERSE);
        mvaddstr(0, 1, tab_label);
        attroff(COLOR_PAIR(kPairAlarm) | A_BOLD | A_REVERSE);
    }
}

std::uint64_t TabbedDashboard::compute_render_digest(
    int active, const dashboard_snapshot* snap)
{
    std::uint64_t h = 0;
    auto mix = [&h](std::uint64_t v) {
        // FNV-style cheap mix; we don't need cryptographic strength,
        // just sensitivity to "did anything visible change".
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };

    mix(static_cast<std::uint64_t>(active));

    if (data_)
    {
        const auto& s = data_->stats();
        mix(s.events_total.load(std::memory_order_relaxed));
        mix(s.fills_total.load(std::memory_order_relaxed));
        mix(s.trades_total.load(std::memory_order_relaxed));
        mix(static_cast<std::uint64_t>(s.last_price_fp8.load(std::memory_order_relaxed)));
        mix(static_cast<std::uint64_t>(s.best_bid_fp8.load(std::memory_order_relaxed)));
        mix(static_cast<std::uint64_t>(s.best_ask_fp8.load(std::memory_order_relaxed)));
        mix(static_cast<std::uint64_t>(s.position_qty_fp8.load(std::memory_order_relaxed)));
        mix(static_cast<std::uint64_t>(s.realized_pnl_fp4.load(std::memory_order_relaxed)));
        mix(static_cast<std::uint64_t>(s.unrealized_pnl_fp4.load(std::memory_order_relaxed)));
        mix(static_cast<std::uint64_t>(s.drawdown_fp4.load(std::memory_order_relaxed)));
        mix(s.halt_flag.load(std::memory_order_acquire) ? 1ULL : 0ULL);
        // Was previously 3 individual mix() calls covering only
        // logging/risk/stats - observer/risk_stats/mm drops could climb
        // without ever tripping a redraw of the status bar's drops
        // indicator. total_ring_drops() covers all 6 in one call.
        mix(total_ring_drops(s));
    }

    if (snap)
    {
        // Sizes only - cheap, and a row count change always implies a
        // visible diff. Doesn't catch "same row count but different
        // values"; the status-bar atomics above usually move when that
        // happens (any fill bumps fills_total, etc).
        mix(snap->positions.size());
        mix(snap->lots.size());
        mix(snap->open_orders.size());
        mix(snap->recent_fills.size());
        mix(snap->brackets.size());
        mix(snap->strategies.size());
        // L2 changes constantly when a venue feed is on; include the
        // best bid/ask price so the L2 panel re-renders on any tick.
        const auto fbid = static_cast<std::uint64_t>(snap->l2.best_bid * 1e8);
        const auto fask = static_cast<std::uint64_t>(snap->l2.best_ask * 1e8);
        mix(fbid);
        mix(fask);
        // Memory cache refresh moves heap_bytes about once per second;
        // include so Debug/Memory section refreshes when it should.
        mix(snap->memory.rss_bytes);
    }

    return h;
}

void TabbedDashboard::render_loop()
{
    dashboard_snapshot snap;
    int prev_w = -1, prev_h = -1;
    int prev_active = -1;
    std::uint64_t last_digest = 0;
    auto last_render = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire))
    {
        handle_input();
        if (!running_.load(std::memory_order_acquire)) break;

        // Space-to-freeze: skip the whole render path. Engine continues
        // to run; we just stop redrawing so the user can read static
        // values without flicker. Toast announcing the freeze stays
        // visible because it predates the freeze.
        if (ui_frozen_.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(tick_);
            continue;
        }

        int h = 0, w = 0;
        getmaxyx(stdscr, h, w);

        // Fix #6: Basic minimum viable terminal size guard
        if (h < 8 || w < 40)
        {
            erase();
            attron(A_BOLD | COLOR_PAIR(2));
            mvprintw(0, 0, "Terminal too small (need at least 40x8). Resize to continue.");
            attroff(A_BOLD | COLOR_PAIR(2));
            refresh();
            std::this_thread::sleep_for(tick_);
            continue;
        }

        // Resize / first frame: do a full erase. Otherwise let ncurses'
        // diff send only the cells that actually changed (each panel
        // overwrites with field-padded text, so old content is replaced
        // in place - no ghost pixels survive).
        const bool resized = (w != prev_w || h != prev_h);
        if (resized)
        {
            erase();
            prev_w = w;
            prev_h = h;
        }

        // Fix #7: Reset all attributes at the start of every frame.
        // This prevents ghost bold/reverse/dim/color from leaking across
        // panels, overlays, or after early returns in draw().
        attrset(A_NORMAL);
        standend();

        int active = active_tab_.load(std::memory_order_acquire);
        const bool tab_switched = (active != prev_active);
        if (tab_switched && !resized)
        {
            // Switching tabs leaves stale rows from the previous panel -
            // wipe just the body region rather than the full screen.
            // Fix #6: be more thorough on resize
            int wipe_start = resized ? 0 : 3;
            for (int row = wipe_start; row < h - 1; ++row)
            {
                move(row, 0);
                clrtoeol();
            }
            prev_active = active;
        }

        const dashboard_snapshot* snap_ptr = nullptr;
        if (snap_fn_ && snap_fn_(snap)) snap_ptr = &snap;

        // ── Frame-skip via state digest ─────────────────────────────
        // Skip render if nothing visible changed AND no overlay is up
        // AND it's been < 1 s since the last paint (sanity bound). The
        // digest is conservative - covers status-bar atomics + active
        // tab + a few snapshot scalars whose change implies *something*
        // worth re-rendering.
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t digest = compute_render_digest(active, snap_ptr);
        const bool overlay_up =
            pending_confirm_.load(std::memory_order_acquire) !=
                static_cast<int>(ConfirmKind::none);
        const auto last_toast = toasts_.last_toast_ms();
        const auto now_ms_count =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
        const bool toast_up = (last_toast > 0)
            && ((now_ms_count - last_toast) < 2500);
        const bool stale = (now - last_render) >= std::chrono::seconds(1);

        // Per-panel min interval: even if the digest changed, throttle
        // slow-moving panels (Health, Debug). Doesn't affect status-bar
        // refresh - that's drawn before the panel and stays at tick rate.
        bool below_panel_interval = false;
        if (active >= 0 && active < static_cast<int>(panels_.size()))
        {
            const auto interval = panels_[active]->min_refresh_interval();
            below_panel_interval = (now - last_render) < interval;
        }

        const bool help_up = show_help_.load(std::memory_order_acquire);
        if (!resized && !tab_switched && !overlay_up && !toast_up &&
            !help_up && !stale && (digest == last_digest || below_panel_interval))
        {
            std::this_thread::sleep_for(tick_);
            continue;
        }

        // Tab badges - counts shown next to each tab name. Tabs that
        // don't have a meaningful count (Overview, Health, Debug, L2)
        // stay 0. Updated each render frame from the snapshot.
        tab_badges_.assign(panels_.size(), 0);
        if (tab_badges_prev_.size() != panels_.size())
            tab_badges_prev_.assign(panels_.size(), 0);
        if (tab_flash_until_.size() != panels_.size())
            tab_flash_until_.assign(panels_.size(), std::chrono::steady_clock::time_point{});

        if (snap_ptr)
        {
            for (std::size_t i = 0; i < tab_names_.size(); ++i)
            {
                const auto& n = tab_names_[i];
                if      (n == "Positions") tab_badges_[i] = snap_ptr->positions.size();
                else if (n == "Orders")    tab_badges_[i] = snap_ptr->open_orders.size();
                else if (n == "Brackets")  tab_badges_[i] = snap_ptr->brackets.size();
                else if (n == "Strategy")  tab_badges_[i] = snap_ptr->strategies.size();

                // Cell-flash: a count change is the cheapest signal
                // that "something happened" in this tab. Flash the
                // badge for ~600 ms so the eye registers the event
                // without the user having to switch tabs.
                if (tab_badges_[i] != tab_badges_prev_[i])
                    tab_flash_until_[i] = now + std::chrono::milliseconds(600);
                tab_badges_prev_[i] = tab_badges_[i];
            }
        }

        draw_chrome(w, h, active);
        draw_status_bar(w, snap_ptr);

        if (active >= 0 && active < static_cast<int>(panels_.size()))
        {
            // body region: rows [3, h-2]
            int body_y0 = 3;
            int body_h  = h - 4;
            if (body_h < 1) body_h = 1;
            panels_[active]->draw(body_y0, w, body_h, *data_, snap_ptr);
        }

        // Overlays last so they paint on top of panel content.
        draw_confirm_overlay(w, h);
        draw_help_overlay(w, h);
        draw_toast(w, h);
        // Halt banner is drawn after every other overlay so nothing else
        // (toast, confirm) can hide it. Once halt fires, it's the only
        // signal that matters.
        draw_halt_banner(w);

        refresh();
        last_digest = digest;
        last_render = now;
        std::this_thread::sleep_for(tick_);
    }
}

}

#endif // HAS_RICH_TUI
