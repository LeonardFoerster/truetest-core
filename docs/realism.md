# Realism toolkit

A growing set of independent knobs that calibrate backtest and shadow
execution toward real-venue behaviour. All default off; live mode bypasses
every one of them (the real exchange supplies real latency, impact, fill
price, and queue position).

Current knobs: realistic fills, latency models, impact models, bar-spread,
walked-book impact, shadow queue position (`--queue-model`), and maker queue
model (`--maker-queue-model`).

## Fill-price modes

Default ("legacy") records every trade at the **aggressor's** limit
price — for market orders that's `mid * market_aggression` (default
1.1), so a market buy always paid a 10% markup regardless of resting
depth. This means calibrated spread / depth / walked-book partial fills
don't show through.

`engine_config::realistic_fills` (CLI `--realistic-fills`) opts into the
correct behaviour:

- Fills print at the **passive** (resting) side's price.
- A market order that eats multiple levels emits one `fill_event` per
  level.
- The bar-spread shift in `LocalBookAdapter::submit_order` is
  auto-skipped — the seeded MM book's spread already drives fill price;
  applying both would double-count.
- `market_aggression` is still required to *cross* the book; it just no
  longer contaminates fill price.

Default is `false` so existing backtests don't shift unannounced.
Ignored in `engine_mode::live`.

## Latency

Two models, applied at different points:

- **`latency_model`** — strategy → order-eligible delay.
  `engine::route_order` pushes to `pending_orders_` with bumped
  `earliest_eligible_ts`; releases on a bar/tick crossing that ts.
  **Bar-mode caveat:** latencies smaller than the bar interval are
  invisible.
- **`wire_latency_model`** — order → venue delay. Consumed by
  trade-tape / hybrid / local-book cancel windows.

CLI: `--order-latency-us`, `--order-latency-stddev-us`.

## Impact

`impact_model` is applied by `LocalBookAdapter` to the reference price
for market orders **before** aggression markup. CLI: `--impact-k-bps`,
`--impact-adv`.

## Bar-spread

`bar_spread_bps` — calibrated full bid-ask charged to bar-mode market
orders. The MM seeds at deterministic half-spread; `LocalBookAdapter`
shifts the market-order reference by ±half-spread. Suppressed for
symbols carrying real L2 (`l2_seeded_symbols_`). CLI:
`--bar-spread-bps`.

## Queue modeling

Passive limit orders are the hardest thing to simulate realistically. Two
separate models attack the "I was first in the queue" optimism problem.

### Shadow-mode queue position (`--queue-model`)

When running in shadow mode against a live trade tape, the
`L2SnapshotQueueModel` (enabled with `--queue-model l2-snapshot`) records
the exact depth resting at your limit price the moment the order becomes
eligible. Subsequent real prints must eat through that queue before the
shadow fill is released. This is the correct way to measure adverse
selection on maker orders.

Requires `--depth-stream`. Only affects shadow mode.

### Maker queue model for paper & backtest (`--maker-queue-model`)

In paper and backtest modes (including the Binance hybrid executor), the
`QueueAwareBookAdapter` + `IQueueModel` family gives you realistic passive
fill dynamics.

When you post a limit, the adapter looks up the current aggregate size at
that price in the L2 book you are feeding (`--depth-stream`). It tracks
`size_ahead` per order. Real trades consume the front of the level. L2
shrinkage beyond observed trade volume is treated as cancels and attributed
according to the chosen model:

| Model     | Behaviour                              | When to use                  |
|-----------|----------------------------------------|------------------------------|
| `uniform` | You absorb a pro-rata fraction of cancels at the level | Recommended default |
| `front`   | Every cancel advances you (optimistic) | Stress-test robustness     |
| `back`    | Cancels never advance you (pessimistic)| Conservative maker sizing  |
| `none`    | Legacy: every limit is at the front    | Baseline comparisons       |

Requires `--depth-stream` (otherwise it silently degrades to `none`).

CLI: `--maker-queue-model {none,uniform,front,back}`

The dashboard shows live quote count and average queue position (in basis
points, 0 = front of queue, 10000 = back) when this model is active. A
summary is also printed at shutdown.

Ignored in live mode (the real venue supplies real queue position).
