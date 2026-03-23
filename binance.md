# Binance Integration — Implementation Steps

Instructions for Claude Code. Execute each step in order. Build and run tests after each step.

Build command: `cmake -B build -DENABLE_BINANCE=ON -DBUILD_TESTS=ON && cmake --build build -j$(nproc)`
Test command: `./build/truetest_tests`

---

## Already Implemented

The following features were implemented in prior sessions. They are listed here for context and should not be re-implemented.

### Binance Fee Auto-Detection (was Step 1)
- `--provider binance` auto-sets `TieredFeeModel(0.001, 0.001)` when no `--fee` flag is passed
- Wired in `main.cpp` CLI provider path

### Starting Balance + Position Sizing + SL/TP (was Step 3 subset)
- `engine_config.initial_balance` wired to portfolio constructor
- `mean_reversion_strategy` has dynamic position sizing: `equity * risk_fraction / price`
- SL/TP in `IStrategy::check_stops()` — triggers market sell on SL/TP hit
- CLI flags: `--balance`, `--risk-fraction`, `--sl`, `--tp`

### Fractional Quantities (qty_t = double)
- `order_event::quantity_` migrated from `int` to `double`
- `fill_event::filled_quantity_` migrated from `int` to `double`
- `position::qty` migrated from `int` to `double`
- `IFeeModel::compute_commission()` signature uses `double quantity`
- `position_stops::quantity` uses `double`
- `trade_record::quantity` in analytics uses `double`
- `mm_order::quantity` in market maker uses `double`
- `shadow_tracker` quantities use `double`
- `LocalBookAdapter` scales `double qty * 1e8` → `uint64_t` on submit, unscales on fill
- Market maker `add_orders()` and `replenish()` scale with `1e8` for orderbook
- `event_json.h` uses `%.8g` format for quantities
- `event_log.h` serialises/deserialises quantities as `f64` instead of `i32`
- Orderbook internals (`using quantity = std::uint64_t`) unchanged — scaling is in the adapter layer

---

## Step 1: Integrate Latency Model into process_order()

**Goal:** Use `engine_config::latency_model` to delay order eligibility in the engine. Currently defined but never called.

### Files to modify

**`BacktestEngine/src/core/engine.cpp` — `process_order()` (line ~452)**

After `publish_event(o);`, before `auto adapter = get_adapter(...)`, insert latency delay logic:

```cpp
// Apply order latency: push the order's earliest eligible timestamp forward
if (config_.latency_model)
{
    auto delay = config_.latency_model->get_order_latency();
    if (delay.count() > 0)
    {
        auto eligible = o->get_earliest_eligible_ts() + delay;
        const_cast<order_event&>(*o).set_earliest_eligible_ts(eligible);
    }
}
```

**`BacktestEngine/src/main.cpp`**

Add a `--latency` CLI flag. Parse it after the existing flags:

```cpp
else if (std::strcmp(argv[i], "--latency") == 0 && i + 1 < argc)
    cli_latency_us = std::stoull(argv[++i]);
```

Declare `uint64_t cli_latency_us = 0;` with the other CLI vars.

When building `prov_cfg`, wire it:

```cpp
if (cli_latency_us > 0)
    prov_cfg.latency_model = std::make_shared<FixedLatencyModel>(
        latency_duration(cli_latency_us));
```

Auto-set a default for Binance if not specified:

```cpp
if (!prov_cfg.latency_model && provider_name == "binance")
    prov_cfg.latency_model = std::make_shared<FixedLatencyModel>(
        latency_duration(500)); // 500us default for Binance
```

Add `#include "execution/latency_model.h"` to main.cpp includes.

---

## Step 2: Binance Historical Kline Downloader

**Goal:** Add a `--download-klines` mode that fetches historical kline data from Binance REST API and saves to CSV for backtesting.

### New file: `BacktestEngine/src/providers/binance/binance_kline_downloader.h`

Create a `BinanceKlineDownloader` class that:
- Uses `BinanceRestClient` (no API key needed for public data) to call `GET /api/v3/klines`
- Paginates (1000 candles per request, advances `startTime`)
- Writes CSV output in `date,symbol,open,high,low,close,volume` format compatible with `CsvDataSource`
- Rate-limits at 100ms between requests

### Wire into main.cpp

CLI flags: `--download-klines <output.csv>`, `--interval <1m>`, `--start-time <epoch_ms>`, `--end-time <epoch_ms>`

Early-exit block before provider mode (gated by `#ifdef HAS_BINANCE`).

### Usage example

```bash
./build/truetest --download-klines btcusdt_jan2024.csv \
    --symbol btcusdt --interval 1m \
    --start-time 1704067200000 --end-time 1706745600000

./build/truetest --provider local --path btcusdt_jan2024.csv --strategy mean-reversion
```

---

## Step 3: Wire Depth Stream to Orderbook in Engine

**Goal:** When using `--stream depth` or combined streams, parse depth updates and apply them to the orderbook via `orderbook::apply_l2_update()` and `apply_l2_snapshot()`.

### Overview

The pieces exist but are not connected:
- `BinanceDepthSnapshotParser` in `binance_depth_parser.h` → parses JSON to `provider::l2_snapshot`
- `BinanceCombinedTransport` in `binance_combined_transport.h` → multi-stream WebSocket
- `orderbook::apply_l2_snapshot()` and `apply_l2_update()` in `orderbook.cpp`
- `IStrategy::on_l2_update()` in `strategy_interface.h` — default no-op

### Engine changes

Add `void process_single_l2(const provider::l2_snapshot& snap, std::size_t& event_count)` to engine. Convert provider levels to orderbook format (apply 1e8 qty scaling), call `ob->apply_l2_snapshot()`, then dispatch individual updates to strategy via `on_l2_update()`.

Add `run_streaming(DataBridge<provider::l2_snapshot>)` overload, modeled after the tick streaming path.

### main.cpp

Wire `--stream depth` and combined `--stream trade+depth` using `BinanceCombinedTransport`.

---

## Step 4: Exchange Filter Validation in Executor

**Goal:** Before submitting live orders, validate against Binance exchange info filters (LOT_SIZE, PRICE_FILTER, MIN_NOTIONAL).

### New file: `BacktestEngine/src/providers/binance/binance_filters.h`

Create a `BinanceFilters` struct that:
- Stores LOT_SIZE (min/max/step), PRICE_FILTER (min/max/tick), MIN_NOTIONAL
- Provides `round_qty()` and `round_price()` for step/tick rounding
- Provides `validate()` that returns error string or empty on success
- Provides `from_exchange_info()` static factory that parses the `/api/v3/exchangeInfo` response

### Integrate into BinanceExecutor

- Fetch filters in `set_live_trading()` via `GET /api/v3/exchangeInfo?symbol=BTCUSDT`
- In `submit_live_order()`, round and validate before submitting

---

## Step 5: Leverage / Margin Support

**Goal:** Support leveraged positions for crypto futures. Quantities are already fractional (double). This step adds margin tracking.

### Design

- `margin_portfolio` class (extends or replaces `portfolio` for margin mode)
- Signed `qty` (negative = short position)
- `margin_used = abs(qty) * entry_price / leverage`
- Liquidation check: `equity <= maintenance_margin`
- CLI flags: `--leverage <N>`, `--margin-mode` (cross/isolated)

### Files to create

- `BacktestEngine/src/execution/margin_portfolio.h/.cpp`

### Files to modify

- `BacktestEngine/src/core/engine_config.h` — add `double leverage = 1.0;`, `bool margin_mode = false;`
- `BacktestEngine/src/core/engine.cpp` — use margin_portfolio when `margin_mode` is true
- `BacktestEngine/src/main.cpp` — wire `--leverage` and `--margin-mode` flags

---

## Step 6: Integration Tests for Shadow Mode

**Goal:** Test the full shadow mode pipeline: engine receives ticks/bars, submits orders to both local book and mock exchange adapter, ShadowTracker compares fills.

### New file: `tests/test_shadow_mode.cpp`

Create `MockExecutionAdapter` (fills at slightly worse price for slippage simulation) and `MockProvider` that returns it. Test:
- `DualExecution` — mock exchange adapter receives orders in shadow mode
- `ShadowTrackerRecordsFills` — shadow tracker records both sim and exchange fills

Add to `CMakeLists.txt` test sources.

---

## Execution Order

1. **Step 1** (latency) — refactors process_order(), wires latency model
2. **Step 2** (downloader) — new file, main.cpp wiring, standalone utility
3. **Step 3** (depth→orderbook) — largest step, new engine method + main.cpp
4. **Step 4** (filters) — new file, executor edits
5. **Step 5** (leverage) — new portfolio variant for margin trading
6. **Step 6** (tests) — new test file, verifies Steps 1-5

Build and test after each step. Do not proceed to the next step if tests fail.
