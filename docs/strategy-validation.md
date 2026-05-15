# Strategy validation tooling

The capture and shadow infrastructure (`docs/demo-trading-workflow.md`)
gives you raw material — tapes, event logs, QuestDB rows. This
document is the work plan for the **tools that turn that raw material
into verdicts**: is strategy A better than strategy B, is the fill
model lying, where is the slippage coming from.

Four deliverables, ranked by leverage. Each is independent — you can
ship them in any order, or stop after the first one if it's enough.

## Deliverable 1 — Dual-portfolio shadow mode

**The killer feature.** Today `ShadowTracker` reports per-order
divergence between simulated and exchange fills (counts, slippage,
latency) but doesn't run a second `Portfolio` to convert that
divergence into a P&L number. Without it, you can see "1.7 bps median
slippage" but you can't see "that costs you $X / day on this volume."

**Design:**

- Add a second `Portfolio` instance to `engine_shadow` (gate on shadow
  mode only).
- The existing `Portfolio` continues to consume `sim_filled` events —
  this is "what your strategy thinks happened."
- The new `exchange_portfolio` consumes `exchange_filled` events from
  `TradeTapeShadowAdapter`. This is "what would have happened with the
  real trade tape."
- At engine shutdown, emit both equity curves + a divergence summary:
  - Final P&L: sim vs exchange (absolute + %).
  - Sharpe / Sortino / max drawdown for each.
  - Per-symbol attribution of the divergence.
- Persist both to QuestDB under suffixed run tags
  (`my_run_sim`, `my_run_exch`) so they show up as separate rows in
  `runs_meta` and existing comparison queries Just Work.

**Files to touch:**

- `src/analytics/shadow_tracker.h` — add fields for the
  exchange-side equity series, or hand fills off to a separate
  worker.
- `src/engine/engine.cpp` — at shadow-mode wiring, instantiate the
  second portfolio. The fill split point is already there
  (line ~2343 routes `on_exchange_fill` to `shadow_tracker_`).
- `src/engine/engine.cpp` shutdown reporter — extend the printed
  summary.
- `src/data/questdb/store.h/.cpp` — emit the second `runs_meta` row.

**Acceptance:**

- `engine_shadow --persist --run-tag my_run` produces two rows in
  `runs_meta` with shared lineage metadata, plus the standard
  `ShadowTracker` report extended with a P&L delta line.
- Sim and exchange fills are independently auditable in QuestDB.
- Disabled when not in shadow mode (no-op for backtest / live).

**Estimated effort:** 1 day, ~150 lines + tests.

## Deliverable 2 — A/B comparison CLI

**Use case:** you ran the same strategy with two parameter sets, two
seeds, two builds. You want a side-by-side: which is better, by how
much, with how much confidence.

**Design:**

- Pure consumer of the existing QuestDB schema — no engine changes.
- Standalone Python script (`scripts/compare_runs.py` or similar),
  one external dep (`questdb`-compatible HTTP query, or `psycopg2`
  via QuestDB's PG wire).
- Inputs: two `run_tag` values.
- Outputs (stdout + optional `--out` HTML/PNG):
  - Side-by-side metrics table: equity, Sharpe, Sortino, max DD,
    win rate, profit factor, total fills, total fees.
  - P&L curve overlay (matplotlib).
  - Slippage histogram (sim vs exchange, or run A vs run B).
  - Fill latency p50/p99 comparison.
  - Per-strategy / per-symbol decomposition where applicable.
  - Statistical significance (bootstrap CI on equity diff).

**Files to add:**

- `scripts/compare_runs.py` — main entry point.
- `scripts/lib/questdb_client.py` — thin query wrapper.
- `scripts/lib/metrics.py` — Sharpe / Sortino / DD computed from
  fills + equity points.
- `tests/scripts/test_compare_runs.py` — at least a smoke test
  against a fixture QuestDB or canned JSON.

**Acceptance:**

```bash
python scripts/compare_runs.py strat_v1_default strat_v1_aggressive
# → tabulated metrics, divergence summary, exit 0
```

- Works against any two runs in `runs_meta`.
- Handles missing/partial runs gracefully.
- Optional `--baseline strat_v1_default --candidates strat_v1_*` form
  for parameter sweeps.

**Estimated effort:** 1-2 days, contained in `scripts/`.

## Deliverable 3 — Per-trade analyzer

**Use case:** after a run, you want every trade as a row, with entry,
exit, duration, P&L, slippage, MAE/MFE — the spreadsheet view that no
aggregate metric can replace.

**Design:**

- Same Python harness as Deliverable 2.
- For one `run_tag`: join `{tag}_orders` ↔ `{tag}_fills` ↔
  `{tag}_order_status` and stitch fills back into round-trip trades
  (entry + exit, possibly via `opener_order_id` field on the fills
  table).
- Compute per-trade:
  - Entry/exit timestamp, price, qty.
  - Holding period (s).
  - Realized P&L (gross + net of fees).
  - Slippage at entry and exit (vs reference price; needs a price
    series — pull from `market_event` log or a sidecar OHLCV file).
  - MAE / MFE — requires a price series indexed by timestamp.
- Output: CSV / Parquet for downstream tools (Excel, pandas, R).

**Files to add:**

- `scripts/extract_trades.py`.
- `scripts/lib/trade_stitcher.py` — the join + round-trip logic.

**Acceptance:**

```bash
python scripts/extract_trades.py strat_v1_replay --out trades.parquet
# → one row per round-trip, schema documented in --help
```

- Handles partial fills (multiple fills per order).
- Handles strategies that don't pair entries with exits (open
  position at end → row with NaN exit fields).
- Documented schema in the script's `--help`.

**Estimated effort:** 1-2 days. The MAE/MFE computation is the only
non-obvious part — needs careful timestamp alignment with the price
series.

## Deliverable 4 — L2-aware queue position modeling

**The biggest realism upgrade for limit-heavy strategies, and the
largest change.** Today `TradeTapeShadowAdapter` ignores depth: a
simulated limit fills the moment a real trade crosses it, regardless
of how much volume was queued ahead in the real book. For a passive
maker, this is far too generous — in reality the strategy would have
been cued behind 30 BTC of older orders and missed the print.

**Design:**

- Subscribe to L2 depth alongside trade tape (already supported via
  `--stream depth`).
- In `TradeTapeShadowAdapter`, on each L2 update, snapshot the
  cumulative volume ahead of each simulated limit at its price level.
- On each real trade print at price P, decrement the "volume ahead"
  for every simulated limit at price P by the trade size.
- A simulated limit fills only when its volume-ahead reaches zero.
- Add a per-order `queue_position` field to the QuestDB orders table
  for forensics.

**Files to touch:**

- `src/execution/trade_tape_shadow_adapter.h` — most of the new logic.
- `src/data/questdb/schema.cpp` — extend the orders table schema
  (`queue_position` column).
- `src/orderbook/orderbook.h` — may need a "snapshot cumulative volume
  at price level" helper if not already present.
- New tests covering: fill blocked by queue, fill released as volume
  drains, partial fills.

**Acceptance:**

- Run identical strategy through replay with and without queue
  modeling. Without modeling, fill rate is e.g. 95%; with modeling,
  fill rate is e.g. 60% — the difference is calibrated against
  recorded prod data.
- Queue position is queryable in QuestDB per order.
- Disabled by default behind `--shadow-queue-modeling` to keep the
  default behaviour stable.
- No-op for backtest and live modes.

**Estimated effort:** 1 week. Most of that is correctness testing —
the model is conceptually simple but has a lot of edge cases (queue
priority loss on amend, cancel-replace, order re-pricing).

## Suggested order

1. **Deliverable 1** first. Without dual-portfolio P&L you're flying
   blind on the actual cost of fill divergence — every other
   validation result is hard to act on.
2. **Deliverable 2** second. As soon as you have anything to compare,
   you'll want to compare it. Pure tooling, no engine risk.
3. **Deliverable 3** third. Spreadsheet view; pays off whenever you
   want to debug a specific bad trade.
4. **Deliverable 4** when limit-heavy strategies become your focus,
   or sooner if shadow runs are showing implausibly high fill rates
   for passive orders.

## Validation methodology

For each deliverable:

- Tooling deliverables (2, 3): pin a fixture run in CI; assert the
  output schema and a few invariants (totals add up, equity curve
  monotonic in cumulative P&L, etc.).
- Engine-touching deliverables (1, 4): the regression check from
  `docs/realism-wiring.md` applies — confirm the default code path
  produces byte-identical fills before / after.

## What this set does *not* cover

- **Multi-venue arbitrage validation.** Single-provider only today;
  out of scope for the validation track until a second venue lands.
- **Walk-forward / cross-validation.** No tooling here for splitting
  a tape into train/test windows or rolling-origin re-fits. Add when
  needed; Deliverable 2 gives you the building blocks.
- **Capacity analysis.** "How big can this strategy run before its
  own impact eats the edge?" requires the impact model from
  `docs/realism-wiring.md` Task 2 plus a sweep harness on top.

## See also

- `docs/demo-trading-workflow.md` — produces the runs that this
  tooling consumes.
- `docs/realism-wiring.md` — close the realism gap so the validation
  numbers mean something.
- `docs/db.md` — QuestDB schema reference; all queries here use it.
- `src/analytics/shadow_tracker.h` — current state of the comparison
  surface.
