# Realistic demo trading workflow

End-to-end recipe for validating a strategy against real Binance market
microstructure **without risking real money and without using the
testnet** (whose synthetic liquidity makes slippage / impact / fill
realism meaningless — see `docs/testnet.md`).

The approach: capture real mainnet tape, replay it deterministically,
and run shadow mode on a live mainnet feed in parallel. Every component
needed for this already ships and is wired to the CLI today — no
engine changes required.

**This document uses spot (`--provider binance`) in the examples.**  
If you are trading **USDT-M futures only** (recommended path for most new
users of this engine), replace:
- `--provider binance` → `--provider binance-futures`
- `stream.testnet.binance.vision` / `stream.binancefuture.com` (testnet)
- Use `kline_1m`, `trade`, or `depth` streams on the futures WebSocket
- Expect position-based reconciliation (`/fapi/v2/positionRisk`) instead of
  spot wallet balances, and `reduceOnly` MARKET closes on shutdown.
- The dead-man's switch (`--dead-man-countdown-ms`) is **on by default** for
  futures and should usually stay armed.

See [`docs/futures-testnet.md`](futures-testnet.md) for the full futures-specific
startup checklist, account setup (one-way mode), and refusal reasons.

## TL;DR

Three CLI invocations and a QuestDB instance get you:

- A reproducible binary tape of real mainnet behaviour.
- A deterministic replay path that re-executes your strategy against
  that tape with bit-identical results across runs.
- A live shadow run that compares simulated fills to what a real
  exchange would have given you, in realtime, without sending an order.
- A full per-event audit trail in QuestDB queryable per `run_tag`.

## Prerequisites

- Build with the optional backends needed for live data + persistence:
  ```bash
  cmake -B build -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON
  cmake --build build
  ```
- A running QuestDB instance reachable on `127.0.0.1:9000` (HTTP, DDL)
  and `127.0.0.1:9009` (ILP, ingest). Soft-fail if absent: the run
  continues but persistence is skipped (warning to stderr). For hard
  guarantees prefer to start QuestDB before launching.
- Disk budget: ~250 MB / hour / symbol for combined `trade` +
  `depth20@100ms` recording, uncompressed. Plan for 2-8 GB per
  overnight session.

## Step 1 — Record real mainnet tape

Capture an interesting window of real mainnet for your symbol and stream
mix. The recorder writes timestamped raw WS JSON, one message per line,
and is tab-delimited (`<ms_ts>\t<json>\n`).

**Spot example:**
```bash
./build/engine_shadow \
    --provider binance --symbol btcusdt \
    --stream trade \
    --record tapes/btc_trade_$(date +%F_%H%M).txt \
    --log-events tapes/btc_$(date +%F_%H%M).bin \
    --persist --run-tag baseline_capture_$(date +%F)
```

**USDT-M Futures example (recommended for most users):**
```bash
./build/engine_shadow \
    --provider binance-futures --symbol btcusdt \
    --stream kline_1m \
    --depth-stream depth20@100ms \
    --record tapes/btcusdt_futures_$(date +%F_%H%M).txt \
    --log-events tapes/btcusdt_futures_$(date +%F_%H%M).bin \
    --persist --run-tag futures_capture_$(date +%F)
```

> **Note for futures:** You can record `trade`, `kline_*`, or depth streams.
> The shadow fill logic (`TradeTapeShadowAdapter`) works on trade prints for
> both spot and futures. Mark price and funding are available in the data
> stream but are not yet used for shadow fill simulation.

Useful capture windows:

- **Quiet hours** (Asia overnight) — calibrate baseline spreads.
- **US/EU open overlap** — peak liquidity, tightest spreads, highest flow.
- **Around scheduled events** (FOMC, CPI release) — stress test against
  real microstructure shocks. Dates in plain trading calendars.

Two artefacts are produced:

| File | Content | Use |
|------|---------|-----|
| `*.txt` | Raw WS JSON, timestamped | Replay via `--replay-data` |
| `*.bin` | Engine event log (zstd, indexed) | Replay via `--replay`, time-windowed via `--replay-from/--to` |

The raw tape is the source of truth for "what did the market do." The
event log additionally captures every order, fill, and risk decision
your engine emitted.

### Multi-stream caveat

`BinanceRecorder` wraps a single transport. To capture trade + L2 depth
together today, run two recorder processes in parallel writing to
separate files, then replay them with the corresponding stream
selection. Combined-stream capture is a future improvement —
`BinanceCombinedTransport` exists but the recorder doesn't decompose
the interleaved message stream back into per-channel files.

## Step 2 — Deterministic replay

Replay a tape against your strategy. With the same RNG seed and same
config the engine produces a bit-identical fill log on every run —
verified by `tests/test_engine_integration.cpp`.

```bash
./build/engine_shadow \
    --replay-data tapes/btc_trade_2026-05-09_1200.txt \
    --strategy your-strat \
    --persist --run-tag strat_v1_replay_$(date +%s)
```

Pacing options:

- **Default (fast-forward)** — ingest as fast as the parser drains.
  Use for iteration speed; a one-hour tape replays in seconds.
- **Paced** — `BinanceReplayTransport` can sleep between messages to
  reproduce original wall-clock timing. Set via `pace=true` in the
  transport ctor. Use this when you want the engine's own latency
  model + scheduling jitter to behave realistically (e.g. validating a
  rate-limited strategy).

Each replay run writes to a fresh `run_tag` in QuestDB. Keep tags
descriptive (`strat_v1_default`, `strat_v1_aggressive_stop`) — they're
the join key for A/B comparisons.

### Time-windowed replay

The event log (`.bin`) is indexed every 1000 events. To replay only a
slice (e.g. one hour of an overnight capture):

```bash
./build/engine_shadow \
    --replay tapes/btc_2026-05-09_1200.bin \
    --replay-from 1746792000000000 \
    --replay-to   1746795600000000 \
    --persist --run-tag strat_v1_window_test
```

Timestamps are microseconds since epoch.

## Step 3 — Live shadow on mainnet

Shadow mode connects to **mainnet WebSocket**, runs your strategy in
real time, and uses `TradeTapeShadowAdapter` to match simulated limit
orders against actual mainnet trade prints. No order is ever sent to
the exchange.

```bash
./build/engine_shadow \
    --provider binance --symbol btcusdt --stream trade \
    --strategy your-strat \
    --wire-latency-us 5000 \
    --persist --run-tag strat_v1_live_shadow_$(date +%s)
```

What the shadow adapter does:

- Holds your simulated limit orders in memory.
- On every real trade print, checks: would my BUY @ P fill if a real
  trade printed at ≤ P? Would my SELL @ P fill if printed at ≥ P?
- For market orders, fills at the next real trade price.
- Records `sim_*` (what your local model thought) and `exchange_*`
  (what the real tape would have given you) per order via
  `ShadowTracker`.

A summary report prints at shutdown: total orders, fill counts,
slippage (absolute + bps), fill rates, latency.

### Known shadow limitations

- **No queue-position modeling.** A simulated limit fills the moment a
  real trade crosses it, regardless of how much volume was ahead in
  the real book. Fill rates are optimistic for top-of-book strategies.
  Tracked as a future improvement in `docs/strategy-validation.md`.
- **Trade-tape only.** `TradeTapeShadowAdapter` doesn't consume L2.
  Subscribe to depth additionally if your strategy reacts to it, but
  fills won't use it.
- **Metrics-only comparison.** `ShadowTracker` reports fill divergence
  but doesn't run a duplicate portfolio. The dual-portfolio extension
  is in the validation backlog.

## Step 4 — Inspect results

Every order, status transition, fill, rejection, cancellation, and
amendment is captured to QuestDB under your `run_tag`. The schema is
documented in `docs/db.md`. Useful starting queries:

```sql
-- Per-run summary
SELECT run_tag, mode, strategy, total_orders, total_fills,
       final_equity - initial_equity AS pnl
FROM runs_meta
WHERE started_at >= dateadd('d', -1, now())
ORDER BY started_at DESC;

-- Slippage histogram for one run
SELECT side,
       avg((price - reference_price) * 10000 / reference_price) AS avg_bps
FROM strat_v1_live_shadow_1715258000_fills f
JOIN strat_v1_live_shadow_1715258000_orders o ON f.order_id = o.order_id
GROUP BY side;

-- Fill latency
SELECT order_id,
       (fill_ts - submit_ts) / 1000 AS latency_ms
FROM strat_v1_live_shadow_1715258000_fills f
JOIN strat_v1_live_shadow_1715258000_orders o ON f.order_id = o.order_id;
```

Companion analysis tooling (Python A/B comparator, per-trade analyzer)
is described in `docs/strategy-validation.md`.

## Validation acceptance criteria

A strategy is "demo-validated" against this workflow when, on a
representative tape:

1. Replay is **deterministic** — two runs with the same seed produce
   byte-identical event logs and identical QuestDB rows (modulo
   timestamps).
2. Shadow run on a live session shows **slippage and fill rate within
   tolerance** of the model's assumptions (define per strategy; e.g.
   median slippage < 2 bps for a passive maker).
3. Sim-vs-exchange divergence is bounded — `sim_only` and
   `exchange_only` fill counts in the `ShadowTracker` report stay
   under your tolerance (e.g. < 5% of total orders).
4. P&L equity curve from replay closely tracks the shadow run's
   simulated P&L on the same time window. (Manual today; automated
   once the dual-portfolio shadow extension lands.)

## Failure modes worth catching

- **Determinism breaks** when the strategy uses an unseeded RNG, wall
  clock, or thread-order-dependent state. Replay should expose this
  immediately.
- **Spurious fills** in replay vs shadow usually mean the trade-tape
  matching is being more generous than reality. Compare against a
  capture with full depth (`--stream depth`) and inspect manually.
- **Latency-induced misses** show up in the shadow report as
  `sim_filled` ≫ `exchange_filled`. Increase `--wire-latency-us` or
  add an order-latency model (see `docs/realism-wiring.md`).

## What this workflow does *not* cover

- **Live exchange protocol bugs** (auth, signing, listenKey, bracket
  placement, rate-limit handling, dead-man's switch). Use the appropriate
  testnet:
  - Spot → [`docs/testnet.md`](testnet.md)
  - **USDT-M Futures (recommended)** → [`docs/futures-testnet.md`](futures-testnet.md)
- **Production capacity / market impact at size.** Shadow mode
  assumes your orders don't move the market. For large clip sizes,
  layer the impact model from `docs/realism-wiring.md` on top.
- **Multi-venue arbitrage.** Single-provider only today.
- **Funding rate carry** and mark-price-based risk for futures strategies.
  The current shadow adapter matches on trade prints; funding P&L is
  not yet modelled in the exchange-side shadow portfolio.

## See also

- `docs/realism-wiring.md` — close the documented-but-unwired realism
  knobs (`--realistic-fills`, impact model, order latency, bar spread).
- `docs/strategy-validation.md` — A/B comparison harness, per-trade
  analyzer, dual-portfolio shadow.
- `docs/engine-optimization.md` — perf workflow once correctness is
  nailed down.
- **For USDT-M futures traders (recommended path):**  
  [`docs/futures-testnet.md`](futures-testnet.md) — account setup, dead-man's
  switch, liquidation warnings, pre-trade caps, refusal reasons, and
  kill-switch behaviour specific to perpetual futures.
- `docs/testnet.md` — spot testnet only.
- `docs/db.md` — QuestDB schema, query patterns, operator notes.
