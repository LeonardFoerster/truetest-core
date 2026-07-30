# S: DMS / Kill-Switch / Bracket Hardening

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. See also src/providers/binance/*_dead_mans_switch.h etc. and prod.md Go-Live row 4.

- **S-01** Extend DMS to also attempt `reduceOnly` MARKET position flattens on heartbeat loss (default behavior or very strong SOP + automation). (Current in `binance_futures_dead_mans_switch.h`: "Critically: this only cancels ORDERS. Open futures positions stay open." Phase 3 `attempt_close` extension in-process via provider fn on persistent fail; pairs with external `tt_watchdog` consideration.)
- **S-02** Address SIGSTOP / process-suspension defeat of DMS (document + consider external `tt_watchdog` binary). (WorkerWatchdog monitors heartbeat thread; 3× promotes to `halt_flag_` so orderly kill-switch runs.)
- **S-03** Improve kill-switch retry/escalation + external monitoring hooks.
- **S-04** Support partial-fraction brackets (`qty_fraction != 1.0`) or make engine-side `ExitManager` fallback robust and fully tested for futures. (Current: `BinanceFuturesBracketAdapter` declines `qty_fraction != 1.0`; engine ExitManager is the only enforcer; logs for missing SL/TP/partials.)
- **S-05** Make futures bracket placement (currently two non-atomic POSTs: `STOP_MARKET` + `TAKE_PROFIT_MARKET` with `closePosition=true` + `reduceOnly`; venue auto-cancels the other leg) more atomic or add robust retry + partial-state reconciliation.
- **S-06** Expand golden regression tests for complex futures multi-leg brackets + reconciliation (`test_engine_brackets.cpp` + golden files).

Additional (Go-Live Gate row 4): DMS position-flattening logic tested (or very strong SOP + automation in place).

**Last updated**: 2026-07-03 (split from governance/03-todo.md per TODOS-SPLIT-SPEC; verbatim extraction of S-* + additional; see 00-OVERVIEW.md).
