# Trust Improvement Programme: `ema-rsi-atr-pullback`

Status: execution in progress. Each plan records its own delivered and
`CONFIRMED`/`UNVERIFIED` status; an unmarked proposal must not be treated as
implemented.

The local strategy key is `ema-rsi-atr-pullback`; the literal
`ema_rsi_pullback` is not the registered key in this working copy.

## Objective

Raise the current result from a deterministic software-regression signal to a
backtest suitable for cautious performance inference.

The prior audit established:

- 26 round trips and final equity `7309.42` from `10000`.
- 33 of 52 fill legs outside the corresponding historical OHLC range.
- zero fees in the baseline.
- `periods_per_year=252` despite 1-minute crypto data.
- missing complete signal/order/risk/fill/exit provenance in the JSON report.

## Execution order

```text
01 baseline/reproducibility
02 data provenance
03 strategy math/state
04 execution/fills
05 risk/exit lifecycle
06 accounting/metrics
07 sensitivity/OOS/robustness
08 observability/evidence
09 final Codex verification gate
```

Every plan must state scope, current code evidence, invariants, tests, acceptance
criteria, and `CONFIRMED`/`LIKELY`/`UNVERIFIED`/`RULED OUT`/`NOT ACTIVE` status.

Until all P0 gates pass, the output is exploratory and must not be presented as
evidence of live profitability.

## Evidence status

`CONFIRMED`: baseline facts are measured in the current working copy.
`UNVERIFIED`: remediation gaps explicitly marked by the individual plans remain
open.

## References

- `docs/todos/11-F-forensic-lifecycle-audit.md`
- `docs/internal/trading-logic-audit-2026-08-17.md`
- `docs/architecture/03-realism.md`
- `src/strategy/ema_rsi_atr_pullback/ema_rsi_atr_pullback_strategy.cpp`
