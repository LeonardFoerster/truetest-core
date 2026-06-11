#pragma once
#ifdef HAS_RICH_TUI

#include <ncurses.h>

#include <string>

namespace truetest::ui {

/**
 * TUI Style System (Lightweight version)
 *
 * This file provides semantic colors and helper functions to improve
 * visual hierarchy and danger signaling without a full theming engine.
 *
 * Color philosophy:
 * - Use semantic names (Positive, Danger, Warning, etc.) instead of raw pairs.
 * - Danger states should be loud and obvious.
 * - Muted/dim for secondary information to reduce visual noise.
 */

// ─────────────────────────────────────────────────────────────
// Semantic Color Pairs
// ─────────────────────────────────────────────────────────────

enum class Color {
    Positive,      // Good PnL, healthy state
    Negative,      // Bad PnL
    Warning,       // Caution zone
    Danger,        // Critical / high risk
    Neutral,       // Default text
    Muted,         // Secondary / less important info
    Accent,        // Cyan-style labels (use sparingly)
    White,
    Black,
};

// Initialize color pairs. Call once during TabbedDashboard startup.
void init_colors();

// Apply a semantic color (foreground only)
void set_color(Color c);
void unset_color(Color c);

// Apply color + bold
void set_color_bold(Color c);
void unset_color_bold(Color c);

// Apply color + dim (for secondary information)
void set_color_dim(Color c);
void unset_color_dim(Color c);

// ─────────────────────────────────────────────────────────────
// Helper Drawing Functions
// ─────────────────────────────────────────────────────────────

// Draw a label in accent color
void draw_label(int y, int x, const char* text);

// Convenience wrapper used across all panels (dim accent label)
void label(int y, int x, const char* text);

} // namespace truetest::ui

// Legacy raw color pair constants - declared at GLOBAL scope
// (before the truetest::ui namespace) so every panel can see
// kPairGreen etc. directly.
constexpr int kPairGreen  = 1;
constexpr int kPairRed    = 2;
constexpr int kPairYellow = 3;
constexpr int kPairCyan   = 4;
constexpr int kPairWhite  = 5;

namespace truetest::ui {

// Draw a value with automatic positive/negative coloring
void draw_value(int y, int x, double value, int precision = 2);

// Draw a value with explicit color
void draw_value(int y, int x, double value, Color color, int precision = 2);

// Draw a percentage bar
void draw_bar(int y, int x, int width, double percentage, Color color);

// Draw a simple risk level indicator
void draw_risk_level(int y, int x, const std::string& level, Color color);

// ─────────────────────────────────────────────────────────────
// Layout & Safe Drawing Helpers (for Fix #1 - responsive columns)
// ─────────────────────────────────────────────────────────────

// Returns how many columns are left after x, respecting right margin
int remaining_width(int start_x, int total_width, int right_margin = 1);

// Right-align a piece of content inside the given total width
int right_align(int total_width, int content_len, int right_margin = 1);

// Safe string output that will never write past (x + max_width - 1)
void safe_mvaddstr(int y, int x, int max_width, const char* str);

// Safe printf-style output with width limit (prevents overruns)
void safe_mvprintw(int y, int x, int max_width, const char* fmt, ...);

// Clamp an x coordinate so it stays inside [0, width-1]
int clamp_x(int x, int width);

// ─────────────────────────────────────────────────────────────
// Risk Level Helpers (very important for futures)
// ─────────────────────────────────────────────────────────────

enum class RiskLevel {
    Safe,
    Caution,
    Warning,
    Danger,
    Critical,
};

Color risk_level_to_color(RiskLevel level);
const char* risk_level_to_string(RiskLevel level);

}

#endif // HAS_RICH_TUI
