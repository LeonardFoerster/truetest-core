# S: DMS / Kill-Switch / Bracket Hardening

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. See also src/providers/binance/*_dead_mans_switch.h etc. and prod.md Go-Live row 4.

- **S-01** Validate the centralized DMS failure path in shadow and venue testnet:
  the first failed heartbeat latches terminal halt and invokes the shared
  exact-once kill session. DMS itself must never flatten or retry.
- **S-02** Address SIGSTOP / process-suspension defeat of DMS (document +
  consider an external `tt_watchdog` binary). WorkerWatchdog promotes a stale
  heartbeat to terminal halt; an external watchdog remains necessary when the
  whole process is suspended.
- **S-03** Add external alerting/escalation hooks for ambiguous kill results.
  Safety HTTP operations remain single-attempt; escalation must never become
  an automatic retry.
- **S-04** Support partial-fraction brackets (`qty_fraction != 1.0`) or make engine-side `ExitManager` fallback robust and fully tested for futures. (Current: `BinanceFuturesBracketAdapter` declines `qty_fraction != 1.0`; engine ExitManager is the only enforcer; logs for missing SL/TP/partials.)
- **S-05** Make futures bracket placement (currently two non-atomic conditional
  algo POSTs with `closePosition=true`, without quantity/`reduceOnly`) more
  atomic or strengthen non-retrying partial-state reconciliation. Do not assume
  the venue cancels the sibling leg automatically.
- **S-06** Expand golden regression tests for complex futures multi-leg brackets + reconciliation (`test_engine_brackets.cpp` + golden files).

Additional (Go-Live Gate row 4): DMS-to-central-kill handoff and the external
process-suspension SOP must be exercised with evidence.

**Last updated**: 2026-08-14 (terminal first-failure and centralized-kill contract).
