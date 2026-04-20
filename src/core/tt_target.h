#pragma once
//
// Compile-time target identification.
//
// Each first-party executable (engine_backtest / engine_shadow / engine_live)
// is compiled with -DTT_TARGET=<one of the TT_TARGET_* ids below>. Code that
// needs to know which binary it's in — for argument-default selection,
// credential gating, or compile-time removal of dead paths — uses these
// macros. Runtime mode switching is kept ONLY at the argument-parsing edge.
//

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
