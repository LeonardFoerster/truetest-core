#ifdef HAS_RICH_TUI

#include "overlays.h"

#include <ncurses.h>
#include <cstring>
#include <algorithm>

namespace truetest::ui {

void paint_confirm_overlay(int width, int height, ConfirmKind kind)
{
    if (kind == ConfirmKind::none) return;

    const char* prompt = (kind == ConfirmKind::flatten)
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

void paint_help_overlay(int width, int height, bool visible)
{
    if (!visible) return;

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
         {"K",      "kill switch - cancel-all + halt (confirm)"},
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

}

#endif // HAS_RICH_TUI
