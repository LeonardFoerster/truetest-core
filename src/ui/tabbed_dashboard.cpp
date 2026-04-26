#ifdef HAS_RICH_TUI

#include "tabbed_dashboard.h"

#include "console_dashboard.h"
#include "dashboard_snapshot.h"
#include "panels/overview_panel.h"
#include "panels/orders_panel.h"
#include "panels/positions_panel.h"
#include "panels/risk_panel.h"

#include <ncurses.h>

#include <clocale>
#include <utility>

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
    panels_.emplace_back(std::make_unique<RiskPanel>());
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
    if (has_colors())
    {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN,  -1);
        init_pair(2, COLOR_RED,    -1);
        init_pair(3, COLOR_YELLOW, -1);
        init_pair(4, COLOR_CYAN,   -1);
        init_pair(5, COLOR_WHITE,  -1);
    }

    thread_ = std::thread([this] { render_loop(); });
}

void TabbedDashboard::stop()
{
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    endwin();
}

void TabbedDashboard::handle_input()
{
    int ch = getch();
    while (ch != ERR)
    {
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
        ch = getch();
    }
}

void TabbedDashboard::draw_chrome(int width, int height, int active)
{
    // Header row: tab names. Active highlighted.
    int x = 1;
    for (std::size_t i = 0; i < tab_names_.size(); ++i)
    {
        bool is_active = (static_cast<int>(i) == active);
        if (is_active) attron(A_REVERSE);
        mvprintw(0, x, " %zu. %s ", i + 1, tab_names_[i].c_str());
        if (is_active) attroff(A_REVERSE);
        x += static_cast<int>(tab_names_[i].size()) + 6;
    }

    // Top separator
    mvhline(1, 0, ACS_HLINE, width);

    // Footer hint
    attron(A_DIM);
    mvprintw(height - 1, 1,
             " 1-%zu select  Tab/Shift+Tab cycle  q quit ",
             tab_names_.size());
    attroff(A_DIM);
}

void TabbedDashboard::render_loop()
{
    dashboard_snapshot snap;
    while (running_.load(std::memory_order_acquire))
    {
        handle_input();
        if (!running_.load(std::memory_order_acquire)) break;

        int h = 0, w = 0;
        getmaxyx(stdscr, h, w);
        erase();

        int active = active_tab_.load(std::memory_order_acquire);
        draw_chrome(w, h, active);

        const dashboard_snapshot* snap_ptr = nullptr;
        if (snap_fn_ && snap_fn_(snap)) snap_ptr = &snap;

        if (active >= 0 && active < static_cast<int>(panels_.size()))
        {
            // body region: rows [2, h-2]
            int body_y0 = 2;
            int body_h  = h - 3;
            if (body_h < 1) body_h = 1;
            panels_[active]->draw(body_y0, w, body_h, *data_, snap_ptr);
        }

        refresh();
        std::this_thread::sleep_for(tick_);
    }
}

} // namespace truetest::ui

#endif // HAS_RICH_TUI
