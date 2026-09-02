# Plan 06 — Accounting and metric semantics

## Goal

Make the report numerically reconcilable and semantically correct for 1-minute
crypto data.

## Delivered contract

- The default basis is `525600` periods/year: one-minute bars over a 24/7
  crypto calendar. A daily/equity/second feed must set `periods_per_year`
  explicitly; the resolved value is exported with every report.
- The display equity curve remains bounded and may be decimated. Its final
  `equity_curve_sample_stride` is exported, while `max_drawdown` is calculated
  on every equity mark. `max_drawdown_peak_equity` and
  `max_drawdown_trough_equity` reproduce that full-stream maximum exactly,
  independently of the chart sampling.
- The rolling metrics now export their return count/window and a reason:
  `computed`, `insufficient_return_observations`,
  `zero_return_variance`, or `no_drawdown_in_window`. A displayed zero is
  therefore either mathematically justified or visibly incomplete.
- `trades[]` remains a physical fill-leg blotter for execution auditability;
  `total_trades`, win rate, and profit factor use completed round trips.
  Every leg carries the intended price and fill provenance. JSON exposes
  `trade_rows_kind=physical_fill_legs`; CSV labels every such row
  `physical_fill_leg`.
- The CLI defaults to conservative 10 bps maker and taker fees
  (`--fee tiered --maker-rate 0.001 --taker-rate 0.001`). `--fee zero` is an
  explicit research-only opt-out and is refused in shadow/live mode. Protected
  provider runs require an explicit venue schedule and an explicit positive
  `periods_per_year` sampling basis; venue configurations should
  override the backtest baseline with their documented schedule.

## Required reconciliation

```text
gross = signed(exit_fill - average_entry) × closed_quantity
net   = gross - allocated_entry_fee - allocated_exit_fee
final = initial + Σ realized_net + funding + unrealized_net + residual
```

`residual` is exported at full precision and is expected to be zero within
floating-point rounding. Funding is a separate cash component rather than an
undocumented exception to the identity.

Compare independent values with Portfolio, Analytics, report, and persistence.

## Metric contract

| Metric | Source / units / formula | Missing-data and verification |
|---|---|---|
| Return | Account-currency marked equity; `(final - initial) / initial`. | No market observation produces no return observation. `Analytics.InitialReport`. |
| Annual return | Compounded return, `pow(1 + return, periods_per_year / observed_returns) - 1`. | Uses the exported basis; falls back to cumulative return without observations. `Analytics.AnnualizedReturn_MatchesCompoundingFormula`. |
| Sharpe / Sortino | Per-market-event marked-equity returns; annualized by `sqrt(periods_per_year)`. Funding stays separately reconciled and resets the mark-return baseline, so it cannot create an irregular synthetic period. Sortino uses all-period downside deviation below the per-period MAR. | Need at least two returns; zero variance yields zero Sharpe with an explicit rolling reason. The exported `return_observation_basis` makes the cash-settlement exclusion explicit. `Analytics.Sharpe_BarReturns_Annualized`. |
| Drawdown / Calmar | Full mark stream; max peak-to-trough percentage. Calmar is annualized return divided by drawdown percentage. | The bounded chart is labelled by stride. Peak/trough witness reproduces the exact value. `Analytics.DrawdownWitnessRemainsExactWhenCurveIsDecimated`. |
| Win rate / profit factor | Completed round trips, not fill legs; percentage wins and gross wins / absolute gross losses. | A partial close does not count as a trade. Existing round-trip analytics tests cover the aggregation. |
| Exposure | Fraction of market events with at least one open position. | Market-event cadence, not wall-clock sampling; zero without market data. |
| Slippage | Fill price minus intended price, side-normalized; account-currency price units. | Intended price is zero only when unavailable; adverse/favourable counts make the denominator visible. |
| Benchmark | Buy-and-hold equity from first/last market close using initial cash. | Omitted until a valid first price exists; compared on the same market-event cadence. |

The direct JSON export, web report JSON, and C API report JSON carry the
accounting fields, period basis, return-observation basis, drawdown witness,
and rolling diagnostics.
Numeric assertions use `1e-12` internally; presentation fields may be rounded
for readability but `reconciliation_residual` is exported at full precision.

## Acceptance evidence

- Time basis: default/CLI regression tests assert `525600`.
- Drawdown: a forced-decimation test proves that the exported peak/trough
  witness matches the full-stream drawdown even when neither extremum is in
  the chart curve.
- Rolling metrics: an irregular marked-equity series produces nonzero Sharpe
  and drawdown with `computed` reasons; insufficient inputs are labelled.
- Accounting: a $10 gross close with $3 total commissions and $5 funding
  reports $7 net realized PnL, $1,012 final equity, and zero residual.
- Serialization: the accounting, sampling, rolling, intended-price, and fill
  provenance fields are regression-tested in the JSON export.

## Evidence status

The accounting and metrics contract above is covered by deterministic unit and
CLI regression tests. Performance interpretation still requires a run-specific
fee schedule, data interval, execution model, and complete input manifest; the
conservative default does not turn synthetic execution into venue evidence.

## References

- `src/analytics/analytics.cpp`
- `src/portfolio/`
- `src/report/`
- `src/bin/main.inc`
