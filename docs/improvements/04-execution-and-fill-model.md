# Plan 04 — Execution and fill validity

## Problem

The local backtest uses a synthetic MarketMaker/LocalBookAdapter. The audited
baseline had 33/52 fills outside their historical execution bars. This is an
assumption, not evidence supplied by the CSV.

## Select one explicit model

1. **Bar-constrained:** fills obey a documented conservative OHLC and gap rule.
2. **Synthetic liquidity:** outside-OHLC fills are allowed, but spread, depth,
   impact, and size are calibrated from external evidence and results are labelled
   exploratory.
3. **Historical L2:** fills use recorded book/trade data with queue and latency.

Codex must not silently mix these models.

## Selected model

**Synthetic liquidity** is the current local-backtest model. `LocalBookAdapter`
matches against a synthetic MarketMaker ladder; fills may therefore lie outside
the source bar's OHLC range. Those fills are explicitly marked exploratory and
must not be presented as historical execution evidence. The current ladder,
spread, depth, impact, and size defaults have not been externally calibrated.

## Required tests

- open/high/low/close, gap-through, outside-range, and same-bar SL/TP cases;
- signal close cannot fill before the permitted timestamp;
- exits cannot use pre-arm prices as gap fills;
- depth, precision, quantity exhaustion, and fill IDs;
- spread, impact, fees, probability, latency, and model name in every fill.

## Acceptance criteria

Every fill contains intended price, execution price, model, reference bar, and
reason. Outside-OHLC fills are either impossible by contract or explicitly
excluded from historical execution claims.

## Evidence status

The audited outside-OHLC fills are `CONFIRMED`. The selected synthetic model is
now carried on each local fill with intended/reference prices, reference time,
reason, spread/impact, probability, modeled latency, fee, and stable fill ID;
the JSON, CSV, and console report label such runs exploratory. Per-level
remaining quantity now reflects the residual after that fill rather than the
final walked-book residual. Real-market validity remains `UNVERIFIED` pending
external calibration and is explicitly excluded from historical execution
claims.

## References

- `src/execution/`
- `src/providers/local/`
- `src/engine/engine_orders.cpp`
- `src/engine/fill_processor.cpp`
- `docs/architecture/03-realism.md`
