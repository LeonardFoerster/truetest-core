#ifdef HAS_RICH_TUI

#include "tabbed_dashboard.h"

#include "console_dashboard.h"
#include "dashboard_snapshot.h"
#include "panels/overview_panel.h"
#include "panels/orders_panel.h"
#include "panels/positions_panel.h"
#include "panels/risk_panel.h"
#include "panels/brackets_panel.h"
#include "panels/strategy_panel.h"
#include "panels/health_panel.h"
#include "panels/debug_panel.h"
#include "panels/l2_panel.h"
#include "tui_style.h"

#include <ncurses.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

namespace truetest::ui {

namespace {

// Resolve ~/.config/truetest/tui.json (or $XDG_CONFIG_HOME equivalent).
std::filesystem::path prefs_path()
{
    namespace fs = std::filesystem;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && *xdg) base = fs::path(xdg);
    else
    {
        const char* home = std::getenv("HOME");
        if (!home || !*home) return {};
        base = fs::path(home) / ".config";
    }
    return base / "truetest" / "tui.json";
}

struct prefs_state {
    int active_tab = -1;
    int theme      = -1;
    int frozen     = -1;
};

void load_prefs(prefs_state& out)
{
    auto p = prefs_path();
    if (p.empty()) return;
    std::ifstream f(p);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line))
    {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.erase(s.begin());
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '"' ||
                                   s.back()  == ',' || s.back()  == '}'))   s.pop_back();
        };
        trim(k); trim(v);
        try {
            if      (k == "active_tab") out.active_tab = std::stoi(v);
            else if (k == "theme")      out.theme      = std::stoi(v);
            else if (k == "frozen")     out.frozen     = std::stoi(v);
        } catch (...) {}
    }
}

void save_prefs(const prefs_state& in)
{
    namespace fs = std::filesystem;
    auto p = prefs_path();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    if (!f.is_open()) return;
    f << "{\n";
    f << "  \"active_tab\": " << in.active_tab << ",\n";
    f << "  \"theme\":      " << in.theme      << ",\n";
    f << "  \"frozen\":     " << in.frozen     << "\n";
    f << "}\n";
}

} // namespace

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

    // Enable mouse — clicks on the tab bar switch tabs. Disabled by
    // setting an empty mask. ALL_MOUSE_EVENTS covers presses, releases,
    // and scroll wheel; we react only to button-down on the header row.
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
    // mouseinterval(0) disables click-vs-doubleclick coalescing so the
    // single-click latency is minimal.
    mouseinterval(0);

    // Terminal-side hints — tiny per-frame savings that compound over
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
        // Theme palette — pair ids 1..5 are referenced everywhere.
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
    prefs_state ps;
    load_prefs(ps);
    if (ps.active_tab >= 0 && ps.active_tab < static_cast<int>(panels_.size()))
        active_tab_.store(ps.active_tab, std::memory_order_release);
    if (ps.theme >= 0 && ps.theme <= static_cast<int>(theme::hicontrast))
        theme_ = static_cast<theme>(ps.theme);

    thread_ = std::thread([this] { render_loop(); });
}

void TabbedDashboard::stop()
{
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    endwin();

    // Persist user-visible state across runs. Best-effort — failure to
    // write (no HOME, read-only fs, etc.) is silent.
    prefs_state ps;
    ps.active_tab = active_tab_.load(std::memory_order_acquire);
    ps.theme      = static_cast<int>(theme_);
    ps.frozen     = ui_frozen_.load(std::memory_order_acquire) ? 1 : 0;
    save_prefs(ps);
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
        if (pc != static_cast<int>(confirm_kind::none))
        {
            if (ch == 'y' || ch == 'Y')
            {
                if (pc == static_cast<int>(confirm_kind::flatten) && actions_.flatten)
                {
                    actions_.flatten();
                    set_toast("flatten requested");
                }
                else if (pc == static_cast<int>(confirm_kind::kill) && actions_.kill)
                {
                    bool ok = actions_.kill(std::chrono::milliseconds(2000));
                    set_toast(ok ? "kill switch fired"
                                 : "kill switch returned false");
                }
                pending_confirm_.store(static_cast<int>(confirm_kind::none),
                                       std::memory_order_release);
            }
            else if (ch == 'n' || ch == 'N' || ch == 27)
            {
                pending_confirm_.store(static_cast<int>(confirm_kind::none),
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
                    for (std::size_t i = 0; i < tab_names_.size(); ++i)
                    {
                        const auto& name = tab_names_[i];
                        const std::size_t count = tab_badges_.size() > i
                            ? tab_badges_[i] : 0;
                        const int label_w = 4 + static_cast<int>(name.size());
                        const int badge_w = (count > 0)
                            ? static_cast<int>(std::snprintf(nullptr, 0,
                                                             "[%zu]", count))
                            : 0;
                        const int span = label_w + badge_w + 2;
                        if (me.x >= x && me.x < x + span)
                        {
                            active_tab_.store(static_cast<int>(i),
                                              std::memory_order_release);
                            break;
                        }
                        x += span;
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
        }
        else if (ch == KEY_BTAB || ch == KEY_LEFT)
        {
            int t = active_tab_.load(std::memory_order_acquire);
            int n = static_cast<int>(panels_.size());
            active_tab_.store((t - 1 + n) % n, std::memory_order_release);
        }
        else if (ch == 'p' || ch == 'P')
        {
            if (actions_.pause_toggle)
            {
                actions_.pause_toggle();
                bool now_paused = actions_.pause_state ? actions_.pause_state() : false;
                set_toast(now_paused ? "PAUSED — no new orders" : "resumed");
            }
            else set_toast("pause not wired");
        }
        else if (ch == 'F')
        {
            if (actions_.flatten)
                pending_confirm_.store(static_cast<int>(confirm_kind::flatten),
                                       std::memory_order_release);
            else set_toast("flatten not wired");
        }
        else if (ch == 'K')
        {
            if (actions_.kill)
                pending_confirm_.store(static_cast<int>(confirm_kind::kill),
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
            set_toast(!was ? "UI frozen — engine still running" : "UI live");
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
            // Enter filter input mode: show a blocking prompt at the
            // bottom of the screen, capture chars until Enter/Esc.
            // Keeps the panel rendered behind so the user sees what
            // they're filtering. Uses synchronous getch (we already
            // own the input loop — no nested loops).
            std::string buf;
            int h = 0, w = 0;
            getmaxyx(stdscr, h, w);
            nodelay(stdscr, FALSE);
            curs_set(1);
            for (;;)
            {
                move(h - 1, 0); clrtoeol();
                attron(COLOR_PAIR(4) | A_BOLD);
                mvprintw(h - 1, 1, " /");
                attroff(COLOR_PAIR(4) | A_BOLD);
                mvaddstr(h - 1, 4, buf.c_str());
                refresh();
                int kc = getch();
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
            nodelay(stdscr, TRUE);
            curs_set(0);
            const int active = active_tab_.load(std::memory_order_acquire);
            if (active >= 0 && active < static_cast<int>(panels_.size()))
                panels_[active]->set_filter(buf);
            set_toast(buf.empty() ? "filter cleared"
                                  : ("filter: " + buf));
        }
        ch = getch();
    }
}

void TabbedDashboard::set_toast(const std::string& msg)
{
    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(toast_mu_);
        toasts_.insert(toasts_.begin(), toast_entry{msg, now_ms});
        if (toasts_.size() > kToastQueueCap)
            toasts_.resize(kToastQueueCap);
    }
    last_toast_ms_.store(now_ms, std::memory_order_release);
}

void TabbedDashboard::draw_confirm_overlay(int width, int height)
{
    const int pc = pending_confirm_.load(std::memory_order_acquire);
    if (pc == static_cast<int>(confirm_kind::none)) return;

    const char* prompt = (pc == static_cast<int>(confirm_kind::flatten))
        ? "  FLATTEN all open positions?  [y/n]  "
        : "  KILL: cancel all + halt?     [y/n]  ";
    const int w = static_cast<int>(std::strlen(prompt)) + 2;
    const int h = 5;
    const int x = (width - w) / 2;
    const int y = (height - h) / 2;

    // Box.
    constexpr int kPairRed = 2;
    attron(COLOR_PAIR(kPairRed) | A_BOLD);
    mvhline(y,         x,           ACS_HLINE,   w);
    mvhline(y + h - 1, x,           ACS_HLINE,   w);
    mvvline(y,         x,           ACS_VLINE,   h);
    mvvline(y,         x + w - 1,   ACS_VLINE,   h);
    mvaddch(y,         x,           ACS_ULCORNER);
    mvaddch(y,         x + w - 1,   ACS_URCORNER);
    mvaddch(y + h - 1, x,           ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1,   ACS_LRCORNER);
    mvaddstr(y + 2, x + 1, prompt);
    attroff(COLOR_PAIR(kPairRed) | A_BOLD);
}

void TabbedDashboard::draw_help_overlay(int width, int height)
{
    if (!show_help_.load(std::memory_order_acquire)) return;

    // Dim the underlying screen by overdrawing a half-width box.
    constexpr int kPairCyan  = 4;
    constexpr int kPairWhite = 5;

    struct row { const char* keys; const char* desc; };
    static constexpr row groups[][8] = {
        // Navigation
        {{"1..9",  "switch to tab N"},
         {"Tab / →","cycle to next tab"},
         {"⇧Tab / ←","cycle to previous tab"},
         {"q / Q",   "quit dashboard"},
         {nullptr,nullptr}, {nullptr,nullptr}, {nullptr,nullptr}, {nullptr,nullptr}},
        // Operator actions
        {{"p",      "pause/resume strategy emission"},
         {"F",      "flatten all open positions (confirm)"},
         {"K",      "kill switch — cancel-all + halt (confirm)"},
         {"Space",  "freeze UI updates (engine continues)"},
         {nullptr,nullptr}, {nullptr,nullptr}, {nullptr,nullptr}, {nullptr,nullptr}},
        // UI / display
        {{"?",      "toggle this help"},
         {"Esc",    "dismiss overlay / cancel confirm"},
         {nullptr,nullptr}, {nullptr,nullptr},
         {nullptr,nullptr}, {nullptr,nullptr}, {nullptr,nullptr}, {nullptr,nullptr}},
    };
    static constexpr const char* group_names[] = {
        "Navigation", "Operator actions", "UI / display"
    };

    const int box_w = std::min(width  - 4, 64);
    const int box_h = std::min(height - 4, 22);
    const int box_x = (width  - box_w) / 2;
    const int box_y = (height - box_h) / 2;

    // Frame.
    attron(COLOR_PAIR(kPairCyan) | A_BOLD);
    mvhline(box_y,             box_x, ACS_HLINE, box_w);
    mvhline(box_y + box_h - 1, box_x, ACS_HLINE, box_w);
    mvvline(box_y,             box_x,             ACS_VLINE, box_h);
    mvvline(box_y,             box_x + box_w - 1, ACS_VLINE, box_h);
    mvaddch(box_y,             box_x,             ACS_ULCORNER);
    mvaddch(box_y,             box_x + box_w - 1, ACS_URCORNER);
    mvaddch(box_y + box_h - 1, box_x,             ACS_LLCORNER);
    mvaddch(box_y + box_h - 1, box_x + box_w - 1, ACS_LRCORNER);

    // Title centered on top border.
    const char* title = " HOTKEYS ";
    mvaddstr(box_y, box_x + (box_w - static_cast<int>(std::strlen(title))) / 2, title);
    attroff(COLOR_PAIR(kPairCyan) | A_BOLD);

    // Body.
    int yy = box_y + 2;
    for (std::size_t g = 0; g < sizeof(groups)/sizeof(groups[0]); ++g)
    {
        if (yy >= box_y + box_h - 2) break;
        attron(COLOR_PAIR(kPairCyan) | A_BOLD);
        mvprintw(yy, box_x + 2, "%s", group_names[g]);
        attroff(COLOR_PAIR(kPairCyan) | A_BOLD);
        ++yy;
        for (const auto& r : groups[g])
        {
            if (!r.keys) break;
            if (yy >= box_y + box_h - 2) break;
            attron(COLOR_PAIR(kPairWhite) | A_BOLD);
            mvprintw(yy, box_x + 4,  "%-12s", r.keys);
            attroff(COLOR_PAIR(kPairWhite) | A_BOLD);
            attron(A_DIM);
            mvprintw(yy, box_x + 18, "%s", r.desc);
            attroff(A_DIM);
            ++yy;
        }
        ++yy;
    }

    // Footer hint.
    attron(A_DIM);
    const char* hint = " press ? or Esc to close ";
    mvaddstr(box_y + box_h - 1,
             box_x + (box_w - static_cast<int>(std::strlen(hint))) / 2, hint);
    attroff(A_DIM);
}

void TabbedDashboard::draw_toast(int width, int height)
{
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<toast_entry> snap;
    {
        std::lock_guard<std::mutex> lk(toast_mu_);
        for (const auto& t : toasts_)
            if ((now - t.ts_ms) <= 6000) snap.push_back(t);   // 6s ttl
    }
    if (snap.empty()) return;

    constexpr int kPairYellow = 3;
    constexpr int kPairWhite  = 5;

    // Stack from bottom-right upward, newest first.
    int yy = height - 2;     // one row above the footer hint
    for (std::size_t i = 0; i < snap.size() && yy >= 0; ++i, --yy)
    {
        const auto& t = snap[i];
        const auto age = now - t.ts_ms;
        const int x = std::max(1, width - static_cast<int>(t.msg.size()) - 4);
        if (i == 0)
        {
            // Newest: full color, reverse video.
            attron(COLOR_PAIR(kPairYellow) | A_REVERSE | A_BOLD);
            mvprintw(yy, x, " %s ", t.msg.c_str());
            attroff(COLOR_PAIR(kPairYellow) | A_REVERSE | A_BOLD);
        }
        else if (age < 4000)
        {
            // Mid-age: dimmer, no reverse.
            attron(COLOR_PAIR(kPairWhite) | A_DIM);
            mvprintw(yy, x, " %s ", t.msg.c_str());
            attroff(COLOR_PAIR(kPairWhite) | A_DIM);
        }
        else
        {
            // Old: very dim text only.
            attron(A_DIM);
            mvprintw(yy, x, " %s ", t.msg.c_str());
            attroff(A_DIM);
        }
    }
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
            char b[16];
            std::snprintf(b, sizeof(b), "[%zu]", count);
            mvaddstr(0, x, b);
            attroff(COLOR_PAIR(badge_pair) | extra);
            x += static_cast<int>(std::strlen(b));
        }
        x += 2;
    }

    // Status bar at row 1 — drawn by render_loop after this returns.
    // Top separator at row 2.
    mvhline(2, 0, ACS_HLINE, width);

    // Footer hint
    move(height - 1, 0); clrtoeol();
    attron(A_DIM);
    mvprintw(height - 1, 1,
             " 1-%zu tabs · Tab cycle · ? help · Space freeze · p pause · F flat · K kill · q quit ",
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
    const std::uint64_t drops_total =
          s.ring_drops_logging.load(std::memory_order_relaxed)
        + s.ring_drops_risk.load(std::memory_order_relaxed)
        + s.ring_drops_stats.load(std::memory_order_relaxed)
        + s.ring_drops_observer.load(std::memory_order_relaxed)
        + s.ring_drops_risk_stats.load(std::memory_order_relaxed)
        + s.ring_drops_mm.load(std::memory_order_relaxed);

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

    // Pause background — subtle but clear visual signal across the whole bar
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
        const double mid = (bid + ask) * 0.5;
        const double bps = mid > 0 ? (ask - bid) / mid * 1e4 : 0.0;
        std::snprintf(buf, sizeof(buf), "%.1fbp", bps);
        put_field("spr:", buf, Color::Muted, false);
    }

    // Position
    std::snprintf(buf, sizeof(buf), "%+.4f", pos);
    put_field("pos:", buf, pos > 0 ? Color::Positive : (pos < 0 ? Color::Negative : Color::Neutral));

    // Equity
    std::snprintf(buf, sizeof(buf), "%.2f", eq);
    put_field("eq:", buf, Color::Neutral);

    // Total PnL — one of the most important numbers on screen
    std::snprintf(buf, sizeof(buf), "%+.2f", pnl + unrl);
    Color pnl_col = (pnl + unrl) > 0 ? Color::Positive : ((pnl + unrl) < 0 ? Color::Negative : Color::Neutral);
    put_field("pnl:", buf, pnl_col, true);

    // Drawdown — critical risk metric
    std::snprintf(buf, sizeof(buf), "%.2f%%", dd);
    Color dd_col = (dd <= -5.0) ? Color::Danger : (dd <= -1.0 ? Color::Warning : Color::Muted);
    put_field("dd:", buf, dd_col, true);

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
        put_pair("drops ", buf, Color::Danger);
        x += 2;
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
        char up_b[24] = {0};
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
    const int rx = std::max(x + 2, width - rlen - 1);
    if (paused) {
        set_color_bold(Color::Warning);
        attron(A_REVERSE);
        mvaddstr(y, rx, right_buf);
        attroff(A_REVERSE);
        unset_color_bold(Color::Warning);
    } else {
        attron(A_DIM);
        mvaddstr(y, rx, right_buf);
        attroff(A_DIM);
    }

    (void)width;  // current bar fits any reasonable terminal
}

void TabbedDashboard::draw_halt_banner(int width)
{
    if (!data_) return;
    if (!data_->stats().halt_flag.load(std::memory_order_acquire)) return;
    if (width <= 0) return;

    constexpr int kPairAlarm = 6;

    // Rising edge: ring the bell once. ncurses beep() is a no-op on
    // terminals that have it disabled — silent failure is fine.
    if (!halt_bell_fired_.exchange(true, std::memory_order_acq_rel))
        ::beep();

    std::string reason = data_->shutdown_reason();
    if (reason.empty()) reason = "halt";

    // Compose " ▶ HALT — <reason> " then pad/truncate to full width so
    // the bg color fills the row edge-to-edge with no gaps.
    std::string body = "  HALT - ";
    body += reason;
    if (static_cast<int>(body.size()) > width)
        body.resize(static_cast<std::size_t>(width));
    body.append(static_cast<std::size_t>(width) - body.size(), ' ');

    attron(COLOR_PAIR(kPairAlarm) | A_BOLD | A_BLINK);
    mvaddstr(0, 0, body.c_str());
    attroff(COLOR_PAIR(kPairAlarm) | A_BOLD | A_BLINK);
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
        mix(s.ring_drops_logging.load(std::memory_order_relaxed));
        mix(s.ring_drops_risk.load(std::memory_order_relaxed));
        mix(s.ring_drops_stats.load(std::memory_order_relaxed));
    }

    if (snap)
    {
        // Sizes only — cheap, and a row count change always implies a
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

        // Resize / first frame: do a full erase. Otherwise let ncurses'
        // diff send only the cells that actually changed (each panel
        // overwrites with field-padded text, so old content is replaced
        // in place — no ghost pixels survive).
        const bool resized = (w != prev_w || h != prev_h);
        if (resized)
        {
            erase();
            prev_w = w;
            prev_h = h;
        }

        int active = active_tab_.load(std::memory_order_acquire);
        const bool tab_switched = (active != prev_active);
        if (tab_switched && !resized)
        {
            // Switching tabs leaves stale rows from the previous panel —
            // wipe just the body region rather than the full screen.
            for (int row = 3; row < h - 1; ++row)
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
        // digest is conservative — covers status-bar atomics + active
        // tab + a few snapshot scalars whose change implies *something*
        // worth re-rendering.
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t digest = compute_render_digest(active, snap_ptr);
        const bool overlay_up =
            pending_confirm_.load(std::memory_order_acquire) !=
                static_cast<int>(confirm_kind::none);
        const auto last_toast = last_toast_ms_.load(std::memory_order_acquire);
        const auto now_ms_count =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
        const bool toast_up = (last_toast > 0)
            && ((now_ms_count - last_toast) < 2500);
        const bool stale = (now - last_render) >= std::chrono::seconds(1);

        // Per-panel min interval: even if the digest changed, throttle
        // slow-moving panels (Health, Debug). Doesn't affect status-bar
        // refresh — that's drawn before the panel and stays at tick rate.
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

        // Tab badges — counts shown next to each tab name. Tabs that
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

} // namespace truetest::ui

#endif // HAS_RICH_TUI
