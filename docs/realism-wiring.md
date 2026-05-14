# Realism wiring plan

> **Status (2026):** This plan has been largely executed. All original
> tasks plus the later **maker queue model** (`--maker-queue-model`) have
> been completed and are documented in `docs/realism.md`.

`docs/realism.md` describes the execution-realism toolkit. At the time
this plan was written, most of the knobs existed only as isolated classes.
The work described below (plus the subsequent maker queue model) closed
the gap.

## Current status (updated)

| Knob                    | Status      | Notes |
|-------------------------|-------------|-------|
| `--aggression`          | ✅ Done     | |
| `--wire-latency-us`     | ✅ Done     | |
| `--order-latency-us`    | ✅ Done     | Stochastic + fixed models |
| `--impact-k-bps`        | ✅ Done     | SquareRoot + walked-book |
| `--realistic-fills`     | ✅ Done     | Passive-side pricing |
| `--bar-spread-bps`      | ✅ Done     | |
| `--walked-book-impact`  | ✅ Done     | |
| `--queue-model`         | ✅ Done     | Shadow L2 position model |
| `--maker-queue-model`   | ✅ Done     | Paper/backtest `QueueAwareBookAdapter` + Front/Uniform/Back cancel models |

All realism flags are ignored in live mode and require `--depth-stream`
when they depend on L2 data.

Without these knobs, every backtest result and every shadow-mode P&L
is biased — fills happen too cheaply, market orders take no impact,
and orders execute with zero queue delay. Closing the gap is mostly
plumbing; the hard work (the model implementations) is already done.

## Status snapshot

| Knob | Class exists | Engine wires it | CLI exposes it |
|------|--------------|-----------------|----------------|
| `--aggression` | n/a (constant) | ✅ | ✅ |
| `--wire-latency-us` | `FixedLatencyModel` | ✅ | ✅ |
| `--fill-rng-seed` | n/a | ✅ | ✅ |
| **Order latency** | `StochasticLatencyModel` ✅ | ⚠ consumes `config_.latency_model` but nothing sets it | ❌ |
| **Impact model** | `SquareRootImpactModel` ✅ | ❌ `LocalBookAdapter` ctor accepts it; engine never passes one | ❌ |
| **Realistic fills** | ❌ logic missing | ❌ | ❌ |
| **Bar spread** | ❌ field missing from `engine_config` | ❌ | ❌ |

Source of truth for the gap analysis is in the audit summary; key file
references are inlined per task below.

## Task 1 — Wire order-latency CLI

**Goal:** expose `--order-latency-us` and `--order-latency-stddev-us`
to instantiate a `StochasticLatencyModel` and assign it to
`engine_config.latency_model`. The engine already routes orders
through `pending_orders_` with a bumped `earliest_eligible_ts` —
no engine changes needed.

**Files to touch:**

- `src/execution/latency_model.h` — verify `StochasticLatencyModel`
  constructor signature; no changes expected.
- `src/engine/engine_config.h` — confirm the `latency_model` field
  exists (it does; see line ~41 region with `wire_latency_model`).
- `src/bin/main.inc` — add the two flags to the CLI parser, build
  the model, assign it to `cfg.latency_model`. Mirror the existing
  `--wire-latency-us` plumbing as a template (~line 658 + ~line 1571).
- `tests/` — extend an existing engine integration test to assert that
  setting `--order-latency-us 5000` shifts fills past the eligible ts.

**Acceptance:**

- `engine_backtest --order-latency-us 5000 --order-latency-stddev-us 1500 ...`
  observably delays fills relative to the same run with `0`.
- Bar-mode caveat documented in `realism.md` line 37 still applies.
- `engine_live` ignores both flags (real exchange supplies real
  latency).

**Estimated diff size:** ~30 lines + one test.

## Task 2 — Wire impact model CLI

**Goal:** expose `--impact-k-bps` and `--impact-adv` to instantiate a
`SquareRootImpactModel` and pass it through to `LocalBookAdapter`.
The adapter constructor already accepts the model and the math is
already implemented and unit-tested (`tests/test_impact_model.cpp`).

**Files to touch:**

- `src/execution/impact_model.h` — no changes; `SquareRootImpactModel`
  ctor takes `k_bps` and `adv` (lines 41-44).
- `src/engine/engine_config.h` — add `impact_model` shared_ptr field
  alongside `latency_model` and `wire_latency_model`.
- `src/engine/engine.cpp` — at the `LocalBookAdapter` instantiation
  site (around line 203-206), pass `config_.impact_model` if set.
- `src/bin/main.inc` — add both flags, build the model, assign.
- `tests/test_engine_impact_wiring.cpp` — already exists per audit;
  may need extension to cover the CLI surface.

**Acceptance:**

- `engine_backtest --impact-k-bps 10 --impact-adv 1000000 ...` raises
  effective fill price for buys, lowers it for sells, with magnitude
  proportional to `sqrt(qty / adv)`.
- Default (no flags) behaves identically to today — `ZeroImpactModel`
  is the silent default.
- `engine_live` ignores both flags.

**Estimated diff size:** ~40 lines + test extension.

## Task 3 — Implement `--realistic-fills`

**Goal:** opt into the correct fill-price semantics described in
`realism.md` lines 13-28. Today every fill records at the aggressor's
limit price (mid × `market_aggression`); under the flag, fills should
print at the **passive** (resting) side's price, with one
`fill_event` per book level walked, and the bar-spread shift in
`LocalBookAdapter::submit_order` should be auto-skipped to avoid
double-counting.

This is the **highest-leverage single change** for backtest realism.
Without it, the simulated mark-out on every taker fill is wrong by up
to `market_aggression - 1` (default 10%).

**Files to touch:**

- `src/engine/engine_config.h` — add `bool realistic_fills = false;`.
- `src/execution/execution_adapter.h` — modify
  `LocalBookAdapter::submit_order` to:
  1. Branch on `realistic_fills`.
  2. When on, walk the resting book level by level, emit a
     `fill_event` per level with `price = level.price`.
  3. Skip the bar-spread shift (currently unconditional).
  4. Keep `market_aggression` as the *crossing* multiplier (it
     determines how far through the book the order will eat), but
     stop using it as the fill price.
- `src/bin/main.inc` — add `--realistic-fills` flag.
- `tests/test_local_book_adapter.cpp` (or create) — assert one fill
  per level under the flag, and that resting-side prices are used.

**Acceptance:**

- A market buy that eats two levels of a seeded MM book emits two
  `fill_event`s, each at the corresponding ask-level price.
- With `--realistic-fills` and `--bar-spread-bps 0`, total fill
  notional matches the sum of `level.price * level.qty` walked.
- Default off — existing backtests don't shift unannounced.
- `engine_live` ignores the flag.

**Estimated diff size:** ~80 lines + dedicated tests.

## Task 4 — Implement `--bar-spread-bps`

**Goal:** apply a calibrated full bid-ask charged to bar-mode market
orders. The MM seeds at deterministic half-spread; `LocalBookAdapter`
should shift the market-order reference price by ±half-spread.
Suppress for symbols carrying real L2 (already tracked in
`l2_seeded_symbols_` — see `engine.cpp:1721`).

**Files to touch:**

- `src/engine/engine_config.h` — add `double bar_spread_bps = 0.0;`.
- `src/execution/execution_adapter.h` — in `LocalBookAdapter`, before
  applying impact + aggression, shift reference price by
  `±(bar_spread_bps / 2 / 10000) * mid`. Skip when symbol ∈
  `l2_seeded_symbols_` and skip when `realistic_fills` is on (the
  resting-side walk already incorporates the seeded spread).
- `src/bin/main.inc` — add `--bar-spread-bps` flag.
- `tests/` — bar-mode market order with `--bar-spread-bps 5` lands
  half a basis point worse than mid relative to baseline.

**Acceptance:**

- `--bar-spread-bps 0` (default) is a no-op.
- Suppressed under `--realistic-fills`.
- Suppressed for symbols with real L2 depth subscribed.
- `engine_live` ignores the flag.

**Estimated diff size:** ~25 lines + test.

## Task 5 — Doc cleanup

After Tasks 1-4 ship, update `docs/realism.md` to match reality:

- Remove the implication that `--realistic-fills` and
  `--bar-spread-bps` exist today (they don't, until Tasks 3-4 land).
- Add concrete CLI examples for each knob.
- Cross-link this file (`realism-wiring.md`) once the work is
  complete and merged so future contributors see the design rationale.

Also update `CLAUDE.md`:

- Fix the line that says HybridExecutor is the default for
  shadow mode — it's backtest-only. Shadow mode swaps in
  `TradeTapeShadowAdapter`.

## Suggested merge order

1. **Task 3 (`--realistic-fills`)** first — biggest correctness win
   per line of diff. Every other realism knob's calibration is
   meaningful only once fill prices are honest.
2. **Task 1 (order latency)** next — smallest diff, immediately useful
   for any latency-sensitive strategy.
3. **Task 2 (impact)** — needed once you start sizing positions
   non-trivially.
4. **Task 4 (bar spread)** — only matters for bar-mode workflows that
   don't have L2 depth subscribed.
5. **Task 5 (docs)** — once everything ships.

Bundle 1+3 in a single PR if convenient — they touch overlapping code
in `LocalBookAdapter::submit_order`.

## Validation methodology

For each task, the regression check is the same:

1. Run a fixed CSV backtest (`market_data.csv`, SMA strategy, fixed
   seed) before the change. Capture the fill log.
2. Apply the change. Re-run with the new flag set to its **default**
   (off / zero). Diff the fill log — must be byte-identical.
3. Re-run with the new flag enabled. Diff manually; expected
   differences should match the model.
4. Add the calibrated-flag run as a permanent integration test.

This guarantees no silent backtest shift and that the new code path
is exercised in CI.

## See also

- `docs/realism.md` — current (mostly aspirational) toolkit overview;
  becomes accurate once Tasks 1-4 land.
- `docs/demo-trading-workflow.md` — these knobs are most useful
  applied to replay of recorded mainnet tape.
- `docs/strategy-validation.md` — once realism is honest, the A/B
  harness can fairly compare strategy variants.
- `tests/test_impact_model.cpp` — reference test pattern for adding
  the missing wiring tests.
