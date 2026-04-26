#pragma once
// Compile-time target id. Each engine_{backtest,shadow,live} binary is
// built with -DTT_TARGET=<one of these>. Used for arg defaults, credential
// gating, and dead-path removal. Runtime mode switching is kept only at
// the argument-parsing edge.

#define TT_TARGET_BACKTEST 1
#define TT_TARGET_SHADOW   2
#define TT_TARGET_LIVE     3

#ifndef TT_TARGET
#  error "TT_TARGET must be defined by the build system (see CMakeLists.txt)."
#endif

#if TT_TARGET == TT_TARGET_BACKTEST
#  define TT_DEFAULT_MODE "backtest"
#elif TT_TARGET == TT_TARGET_SHADOW
#  define TT_DEFAULT_MODE "shadow"
#elif TT_TARGET == TT_TARGET_LIVE
#  define TT_DEFAULT_MODE "live"
#else
#  error "Invalid TT_TARGET value."
#endif

namespace truetest {

constexpr bool target_allows_live_orders() noexcept {
#if TT_TARGET == TT_TARGET_LIVE
    return true;
#else
    return false;
#endif
}

// True for binaries that ship the rich (ncurses) tabbed dashboard.
// Backtest finishes faster than human reading speed; the existing ANSI
// dashboard is enough there. Shadow and live get the extended panes.
constexpr bool target_uses_rich_tui() noexcept {
#if TT_TARGET == TT_TARGET_BACKTEST
    return false;
#else
    return true;
#endif
}

constexpr const char* target_name() noexcept {
#if TT_TARGET == TT_TARGET_BACKTEST
    return "engine_backtest";
#elif TT_TARGET == TT_TARGET_SHADOW
    return "engine_shadow";
#elif TT_TARGET == TT_TARGET_LIVE
    return "engine_live";
#else
    return "unknown";
#endif
}

}  // namespace truetest
