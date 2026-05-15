#ifdef HAS_RICH_TUI

#include "tui_style.h"

#include <ncurses.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace truetest::ui {

namespace {

// Internal color pair indices — start at 10 to avoid collision
// with the existing pairs (1-6) used in tabbed_dashboard.cpp
constexpr int PAIR_POSITIVE   = 10;
constexpr int PAIR_NEGATIVE   = 11;
constexpr int PAIR_WARNING    = 12;
constexpr int PAIR_DANGER     = 13;
constexpr int PAIR_NEUTRAL    = 14;
constexpr int PAIR_MUTED      = 15;
constexpr int PAIR_ACCENT     = 16;
constexpr int PAIR_WHITE      = 17;
constexpr int PAIR_BLACK      = 18;

} // anonymous namespace

void init_colors()
{
    // Assume start_color() and use_default_colors() have already been called
    // in TabbedDashboard::start()

    // Positive (green)
    init_pair(PAIR_POSITIVE, COLOR_GREEN,  -1);
    // Negative (red)
    init_pair(PAIR_NEGATIVE, COLOR_RED,    -1);
    // Warning (yellow)
    init_pair(PAIR_WARNING,  COLOR_YELLOW, -1);
    // Danger (bright red / bold red)
    init_pair(PAIR_DANGER,   COLOR_RED,    -1);
    // Neutral (white/default)
    init_pair(PAIR_NEUTRAL,  COLOR_WHITE,  -1);
    // Muted (dim gray-ish)
    init_pair(PAIR_MUTED,    COLOR_WHITE,  -1);   // We use A_DIM on top
    // Accent (cyan)
    init_pair(PAIR_ACCENT,   COLOR_CYAN,   -1);
    // White / Black
    init_pair(PAIR_WHITE,    COLOR_WHITE,  -1);
    init_pair(PAIR_BLACK,    COLOR_BLACK,  -1);
}

// ─────────────────────────────────────────────────────────────
// Color Application
// ─────────────────────────────────────────────────────────────

void set_color(Color c)
{
    switch (c)
    {
        case Color::Positive:   attron(COLOR_PAIR(PAIR_POSITIVE));   break;
        case Color::Negative:   attron(COLOR_PAIR(PAIR_NEGATIVE));   break;
        case Color::Warning:    attron(COLOR_PAIR(PAIR_WARNING));    break;
        case Color::Danger:     attron(COLOR_PAIR(PAIR_DANGER));     break;
        case Color::Neutral:    attron(COLOR_PAIR(PAIR_NEUTRAL));    break;
        case Color::Muted:      attron(COLOR_PAIR(PAIR_MUTED) | A_DIM); break;
        case Color::Accent:     attron(COLOR_PAIR(PAIR_ACCENT));     break;
        case Color::White:      attron(COLOR_PAIR(PAIR_WHITE));      break;
        case Color::Black:      attron(COLOR_PAIR(PAIR_BLACK));      break;
    }
}

void unset_color(Color c)
{
    switch (c)
    {
        case Color::Positive:   attroff(COLOR_PAIR(PAIR_POSITIVE));   break;
        case Color::Negative:   attroff(COLOR_PAIR(PAIR_NEGATIVE));   break;
        case Color::Warning:    attroff(COLOR_PAIR(PAIR_WARNING));    break;
        case Color::Danger:     attroff(COLOR_PAIR(PAIR_DANGER));     break;
        case Color::Neutral:    attroff(COLOR_PAIR(PAIR_NEUTRAL));    break;
        case Color::Muted:      attroff(COLOR_PAIR(PAIR_MUTED) | A_DIM); break;
        case Color::Accent:     attroff(COLOR_PAIR(PAIR_ACCENT));     break;
        case Color::White:      attroff(COLOR_PAIR(PAIR_WHITE));      break;
        case Color::Black:      attroff(COLOR_PAIR(PAIR_BLACK));      break;
    }
}

void set_color_bold(Color c)
{
    set_color(c);
    attron(A_BOLD);
}

void unset_color_bold(Color c)
{
    unset_color(c);
    attroff(A_BOLD);
}

void set_color_dim(Color c)
{
    set_color(c);
    attron(A_DIM);
}

void unset_color_dim(Color c)
{
    unset_color(c);
    attroff(A_DIM);
}

// ─────────────────────────────────────────────────────────────
// Drawing Helpers
// ─────────────────────────────────────────────────────────────

void draw_label(int y, int x, const char* text)
{
    set_color(Color::Accent);
    mvaddstr(y, x, text);
    unset_color(Color::Accent);
}

void draw_value(int y, int x, double value, int precision)
{
    Color c = (value > 0) ? Color::Positive :
              (value < 0) ? Color::Negative : Color::Neutral;

    set_color(c);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.*f", precision, value);
    mvaddstr(y, x, buf);
    unset_color(c);
}

void draw_value(int y, int x, double value, Color color, int precision)
{
    set_color(color);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.*f", precision, value);
    mvaddstr(y, x, buf);
    unset_color(color);
}

void draw_bar(int y, int x, int width, double percentage, Color color)
{
    int filled = static_cast<int>(std::round(percentage * width));
    filled = std::clamp(filled, 0, width);

    set_color(color);

    for (int i = 0; i < filled; ++i)
        mvaddch(y, x + i, ACS_CKBOARD);

    unset_color(color);

    // Fill the rest with dim background
    set_color_dim(Color::Muted);
    for (int i = filled; i < width; ++i)
        mvaddch(y, x + i, ACS_CKBOARD);
    unset_color_dim(Color::Muted);
}

void draw_risk_level(int y, int x, const std::string& level, Color color)
{
    set_color_bold(color);
    mvaddstr(y, x, level.c_str());
    unset_color_bold(color);
}

// ─────────────────────────────────────────────────────────────
// Risk Level Mapping
// ─────────────────────────────────────────────────────────────

Color risk_level_to_color(RiskLevel level)
{
    switch (level)
    {
        case RiskLevel::Safe:     return Color::Positive;
        case RiskLevel::Caution:  return Color::Warning;
        case RiskLevel::Warning:  return Color::Warning;
        case RiskLevel::Danger:   return Color::Danger;
        case RiskLevel::Critical: return Color::Danger;
    }
    return Color::Neutral;
}

const char* risk_level_to_string(RiskLevel level)
{
    switch (level)
    {
        case RiskLevel::Safe:     return "SAFE";
        case RiskLevel::Caution:  return "CAUTION";
        case RiskLevel::Warning:  return "WARNING";
        case RiskLevel::Danger:   return "DANGER";
        case RiskLevel::Critical: return "CRITICAL";
    }
    return "UNKNOWN";
}

} // namespace truetest::ui

#endif // HAS_RICH_TUI
