#pragma once

// Minimal ANSI escape helpers. String literals only — compile-time constants,
// no allocations, no runtime branches. The ConsoleDashboard picks between
// colored and plain output by gating on supports_color() at construction; at
// render time it just concatenates these into a pre-sized buffer.

namespace truetest::ansi {

inline constexpr const char* reset       = "\x1b[0m";
inline constexpr const char* bold        = "\x1b[1m";
inline constexpr const char* dim         = "\x1b[2m";

inline constexpr const char* fg_black    = "\x1b[30m";
inline constexpr const char* fg_red      = "\x1b[31m";
inline constexpr const char* fg_green    = "\x1b[32m";
inline constexpr const char* fg_yellow   = "\x1b[33m";
inline constexpr const char* fg_blue     = "\x1b[34m";
inline constexpr const char* fg_magenta  = "\x1b[35m";
inline constexpr const char* fg_cyan     = "\x1b[36m";
inline constexpr const char* fg_white    = "\x1b[37m";
inline constexpr const char* fg_gray     = "\x1b[90m";

inline constexpr const char* fg_br_red   = "\x1b[91m";
inline constexpr const char* fg_br_green = "\x1b[92m";
inline constexpr const char* fg_br_yel   = "\x1b[93m";

inline constexpr const char* clear_screen   = "\x1b[2J";
inline constexpr const char* cursor_home    = "\x1b[H";
inline constexpr const char* clear_to_eol   = "\x1b[K";
inline constexpr const char* erase_down     = "\x1b[J";
inline constexpr const char* cursor_hide    = "\x1b[?25l";
inline constexpr const char* cursor_show    = "\x1b[?25h";
inline constexpr const char* alt_screen_on  = "\x1b[?1049h";
inline constexpr const char* alt_screen_off = "\x1b[?1049l";

} // namespace truetest::ansi
