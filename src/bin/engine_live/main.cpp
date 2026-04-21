// Entry point for engine_live — the only binary that may ever submit real
// orders (gated by core/tt_target.h's target_allows_live_orders()).
//
// See src/bin/engine_backtest/main.cpp for the rationale behind the thin
// wrapper pattern. Step 10's credential-isolation directive will shift
// REST-credential and signing code into this directory (out of
// src/providers/binance/) as the deepdive phases land — at which point
// this wrapper diverges from engine_backtest/engine_shadow.
#include "../main.inc"
