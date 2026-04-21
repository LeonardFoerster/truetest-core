# 14 — Paper-mode realism

## Goal

Fix the two systematic biases in current paper-mode execution so forward-test
results track live results more closely:

1. **Market orders fill at `last_price`** with no slippage or depth
   awareness.
2. **Limit orders are matched against a local `orderbook` that is seeded
   only by the MarketMaker**, not by real exchange depth — so limits
   fill against an imaginary book.

The practical effect: paper results are optimistic, often by 10–50 bps
per trade. For a SaaS promising "forward testing before going live,"
this is a credibility issue.

## Context

- `providers/binance/binance_executor.h::submit_order()`: in paper mode,
  market orders fill at `last_price_` with zero commission.
- `providers/binance/hybrid_executor.h`: routes market orders to
  `BinanceExecutor` (paper) and limit orders to `LocalBookAdapter`
  (which wraps a local `orderbook`).
- The local `orderbook` in hybrid mode is populated by `MarketMaker`
  (`market_maker/market_maker.cpp`), not by real exchange L2 depth.
- `BinanceTransport` already supports an `ENABLE_BINANCE` depth stream
  (`binance_depth_parser.h`). Subscribing to it is a one-line stream type
  change.

## Instructions

1. **Subscribe to real L2 depth in paper mode**:
   - Extend `BinanceProvider` to accept a `stream_type` that includes
     `trade+depth` or `kline_1m+depth` combined streams.
   - Route depth snapshots and updates into the local `orderbook` via
     the existing `apply_l2_snapshot` / `apply_l2_update` methods (E1).
   - Retire the MarketMaker-seeded book in paper mode. The `MarketMaker`
     stays for pure-backtest (no L2 available) scenarios.

2. **Market orders walk the real book**:
   - In `BinanceExecutor::submit_order` paper path, construct a synthetic
     market order against the local `orderbook` (now populated with real
     depth) instead of filling at `last_price_`.
   - Reuse `LocalBookAdapter`'s matching logic. The hybrid executor already
     routes limits through that adapter; route markets through it too
     once the book carries real depth.

3. **Slippage model** for markets: the book-walking already accounts for
   consumed levels. Add an optional configurable extra spread (bps) to
   account for queue position uncertainty and latency — default 2 bps for
   crypto spot, expose via `engine_config.paper_slippage_bps`.

4. **Fill probability for limit orders at the top**:
   - Today `RealisticFillModel` gives a probability curve based on
     distance from mid. That is a crude proxy for queue position.
   - Improvement: when a limit order is placed at the best bid/ask, mark
     it as "queued" and fill it only when aggregate volume traded at that
     level (summed from incoming trade stream) exceeds the position ahead
     of it in the queue. The position ahead = total qty at that level
     when the order was placed.

5. **Commission**: paper mode currently uses zero commission. Apply the
   same `IFeeModel` that live mode uses; read Binance's maker/taker
   defaults from `FeeModelRegistry::for_venue("binance_spot")`. This
   requires a small venue-keyed fee registry — add it under
   `execution/fee_registry.h`.

6. **Partial fills on market orders**: when a market order cannot fully
   fill because it reaches the end of the book, emit a partial fill then
   expire the remainder (IOC semantics). Do not pretend it filled.

7. **Tests**:
   - `tests/test_paper_realism.cpp` — seed a local orderbook with a known
     L2 snapshot, submit a market order larger than the top level, assert
     the fill price is a volume-weighted average across consumed levels.
   - Regression test: a replay of recorded Binance data through paper
     mode produces fill prices within N bps of the recorded-trade prices
     (N to be calibrated empirically; set to 10 bps as a ceiling).

## Acceptance criteria

- Paper-mode fills show non-zero commission.
- Market orders fill against real L2 depth with slippage proportional to
  order size vs top-of-book depth.
- Limit orders at top of book have a queue-position model, not a blind
  probability roll.
- `paper_slippage_bps` is configurable and exposed.

## Out of scope

- Latency modeling for paper mode beyond what `ILatencyModel` already does.
- Exchange-side "iceberg" or reserve-order mechanics.
- Venues other than Binance. MetaTrader and Polymarket have their own
  execution realism tasks in their respective provider documents.
