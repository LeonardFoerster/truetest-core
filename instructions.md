# TrueTest — Instructions

## What It Does

TrueTest is a modular C++17 backtesting engine. It simulates trading strategies
against historical market data, producing performance metrics like Sharpe ratio,
max drawdown, win rate, and PnL.

The core pipeline is event-driven:

```
market data  -->  strategy  -->  order  -->  orderbook  -->  fill  -->  portfolio
                                                ^
                                          market maker
                                        (seeds liquidity)
```

**Strategies** receive price data and emit buy/sell orders. The **orderbook**
matches orders using price-time priority. The **portfolio** tracks positions,
cash, and PnL. **Analytics** computes risk and performance metrics at the end.

### Pluggable Components

| Component         | Interface         | Implementations                                    |
|-------------------|-------------------|-----------------------------------------------------|
| Data source       | `IDataSource`     | CSV, Tick CSV, PostgreSQL, WebSocket, Binary Cache   |
| Strategy          | `IStrategy`       | SMA, Mean Reversion, MA Crossover                    |
| Fee model         | `IFeeModel`       | Zero, Fixed, Tiered (maker/taker)                    |
| Fill model        | `IFillModel`      | Perfect, Realistic (probability + fade)              |
| Latency model     | `ILatencyModel`   | Zero, Fixed, Stochastic (normal distribution)        |
| Execution adapter | `IExecutionAdapter` | Local book (backtest), Exchange (live stub)         |

### Data Formats

- **OHLCV bars** — standard CSV with columns: `date,symbol,open,high,low,close,volume`
- **Tick data** — CSV with columns: `timestamp_ms,symbol,price,quantity,side`
  - Side: `B` (bid), `A` (ask), or empty (unknown)
  - When tick data is loaded, the engine automatically aggregates bars for
    bar-based strategies via the built-in `BarAggregator`

---

## Requirements

- CMake 3.22+
- C++17 compiler (GCC 8+, Clang 7+, MSVC 2017+)
- Git (for FetchContent dependencies)
- No other dependencies for the default build

---

## Build

### Default (CSV-only, zero dependencies)

```bash
cmake -B build
cmake --build build
```

The binary is `build/truetest`.

### Debug build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Release build (optimized)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### With PostgreSQL support

```bash
cmake -B build -DENABLE_POSTGRESQL=ON
cmake --build build
```

If `pg_config` is not found on your system, CMake auto-fetches vcpkg and
installs `libpqxx`. You can also install PostgreSQL dev libraries manually:

- Ubuntu/Debian: `sudo apt-get install libpq-dev`
- Arch Linux: `sudo pacman -S postgresql-libs`
- macOS: `brew install postgresql`

### With WebSocket live data feed

Requires Boost.Beast and Boost.Asio.

```bash
cmake -B build -DENABLE_LIVE_DATA=ON
cmake --build build
```

### With ThreadSanitizer

```bash
cmake -B build_tsan -DBUILD_TESTS=ON -DENABLE_TSAN=ON
cmake --build build_tsan
cd build_tsan && ctest --output-on-failure
```

### Clean rebuild

```bash
rm -rf build
cmake -B build
cmake --build build
```

---

## Run

```bash
./build/truetest
```

The TUI walks you through:

1. **Strategy** — Mean Reversion, SMA, or MA Crossover
2. **SMA Period** — lookback window (e.g. 20)
3. **Data Source** — CSV file path, Tick CSV path, or PostgreSQL
4. **Fee Model** — Zero, Fixed per trade, or Tiered maker/taker rates
5. **Engine Mode** — Backtest (batch), Shadow (paper), or Live

Example with the included sample data:

```bash
./build/truetest
# Choose strategy: 1
# SMA period: 20
# Data source: 2
# CSV path: market_data.csv
# Fee model: 1
# Engine mode: 1
```

### CLI Flags

| Flag                       | Description                                           |
|----------------------------|-------------------------------------------------------|
| `--seed <N>`               | Seed all RNGs for deterministic replay (0 = random)   |
| `--log-events <path>`      | Write binary event log to file                        |
| `--replay <path>`          | Replay a binary event log (skips TUI)                 |
| `--thread-preset <name>`   | Force a threading preset (see Threading below)        |
| `--no-pin`                 | Disable CPU core pinning                              |

Example — deterministic run with event logging:

```bash
./build/truetest --seed 42 --log-events events.bin
```

Example — replay a recorded session:

```bash
./build/truetest --replay events.bin --seed 42
```

---

## Threading

The engine automatically detects available physical cores and selects a
threading preset. Worker threads handle risk checking, analytics, logging,
and market making via lock-free SPSC ring buffers — keeping the hot path
(strategy + orderbook + portfolio) on a single core.

### Presets

| Preset     | Cores | Workers | What runs off the hot path                    |
|------------|-------|---------|-----------------------------------------------|
| `inline`   | 1-2   | 0       | Nothing — single-threaded mode                |
| `light`    | 3     | 1       | Combined observer (risk + stats + logging)    |
| `standard` | 4-5   | 2       | Logging thread + combined risk/stats thread   |
| `full`     | 6-7   | 3       | Logging + risk + stats (each separate)        |
| `extended` | 8+    | 4       | Logging + risk + stats + market maker         |

Override auto-detection:

```bash
./build/truetest --thread-preset full
./build/truetest --thread-preset inline   # force single-threaded
./build/truetest --no-pin                 # skip CPU affinity (for containers/VMs)
```

### Architecture

```
Thread 0 (hot path)           Worker threads
┌─────────────────────┐       ┌──────────────────┐
│ event loop          │──SPSC──> logging worker   │
│ strategy            │──SPSC──> risk worker      │
│ orderbook           │──SPSC──> stats worker     │
│ portfolio           │──SPSC──> MM worker        │
│                     │<─SPSC── (MM orders back)  │
└─────────────────────┘       └──────────────────┘
         ^                            │
         └────── atomic halt flag ────┘
```

- All inter-thread communication uses lock-free SPSC ring buffers (65536 slots)
- Workers own their own state (no shared mutable data)
- Ring buffer drops are counted and reported (increase capacity if needed)
- Worker exceptions are captured and reported after the run completes

---

## Event Replay & Deterministic Mode

When `--seed` is set, all RNGs (market maker, fill model, latency model) are
seeded deterministically. Combined with `--log-events`, this enables:

1. **Record** a run: `--seed 42 --log-events run.bin`
2. **Replay** identically: `--replay run.bin --seed 42`
3. **Compare** results between runs or presets

The binary event log format stores all 7 event types (market, order, fill,
tick, signal, l2_update, cancel) with timestamps and full payloads.

---

## Test

Tests use GoogleTest, fetched automatically via CMake FetchContent.

### Build tests

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
```

### Run all tests

```bash
cd build && ctest --output-on-failure
```

Or run the test binary directly for the formatted TUI output:

```bash
./build/truetest_tests
```

### Run a specific test suite

```bash
./build/truetest_tests --gtest_filter="Orderbook.*"
```

### Run a single test

```bash
./build/truetest_tests --gtest_filter="Portfolio.BuySell_RoundTrip"
```

### List all tests without running

```bash
./build/truetest_tests --gtest_list_tests
```

### Build and test in one step

```bash
cmake -B build -DBUILD_TESTS=ON && cmake --build build && cd build && ctest --output-on-failure
```

### Run with ThreadSanitizer

```bash
cmake -B build_tsan -DBUILD_TESTS=ON -DENABLE_TSAN=ON
cmake --build build_tsan
cd build_tsan && ctest --output-on-failure
```

### Test Suites

| Suite                      | Tests | What it covers                                  |
|----------------------------|-------|-------------------------------------------------|
| Events                     | 14    | Event types, serialization                       |
| Orderbook                  | 17    | Price-time matching, cancels, fills              |
| Portfolio                  | 8     | Position tracking, PnL, cash                     |
| FeeModel                   | 6     | Zero, fixed, tiered fee calculations             |
| LatencyModel               | 6     | Fixed and stochastic delay                       |
| FillModel                  | 8     | Fill probability, price fade                     |
| SMA                        | 5     | Simple moving average indicator                  |
| Strategies                 | 12    | Mean reversion, SMA, MA crossover logic          |
| RingBuffer                 | 10    | SPSC ring: push/pop, full/empty, policies        |
| BarAggregator              | 8     | Tick-to-bar aggregation                          |
| Analytics                  | 12    | Sharpe, drawdown, Welford's, streaming metrics   |
| MarketMaker                | 6     | Liquidity seeding, volatility-based spread       |
| DataHandler                | 4     | Data loading, queue management                   |
| ExecutionAdapter           | 8     | Local book adapter, fee/fill/latency integration |
| Engine                     | 13    | End-to-end: run, halt, threading, presets        |
| OrderId                    | 3     | Unique ID generation, reset                      |
| Price                      | 5     | Fixed-point price arithmetic                     |
| OrderbookRegistry          | 4     | Multi-symbol orderbook management                |
| OrderTypes                 | 8     | Limit, stop, stop-limit, TIF, day orders         |
| ObjectPool                 | 6     | Block allocation, custom deleter                 |
| RiskManager                | 8     | Pre-order and post-fill risk checks              |
| EventLog                   | 8     | Binary event round-trip, deterministic replay    |
| ThreadPreset               | 5     | Preset selection, string conversion              |
| ThreadingCorrectness       | 2     | All presets produce identical results             |

---

## CMake Options Reference

| Option              | Default | Description                                      |
|---------------------|---------|--------------------------------------------------|
| `ENABLE_POSTGRESQL` | `OFF`   | Build with PostgreSQL data source (libpqxx)      |
| `ENABLE_LIVE_DATA`  | `OFF`   | Build with WebSocket data feed (Boost.Beast)     |
| `ENABLE_TSAN`       | `OFF`   | Build with ThreadSanitizer instrumentation       |
| `BUILD_TESTS`       | `OFF`   | Build the GoogleTest unit test suite             |
| `CMAKE_BUILD_TYPE`  | (none)  | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |

### CMake Commands Quick Reference

```bash
# Configure
cmake -B build                                      # default
cmake -B build -DBUILD_TESTS=ON                     # with tests
cmake -B build -DENABLE_POSTGRESQL=ON               # with postgres
cmake -B build -DCMAKE_BUILD_TYPE=Release           # release mode
cmake -B build -DBUILD_TESTS=ON -DENABLE_TSAN=ON   # with ThreadSanitizer

# Build
cmake --build build                                 # build all
cmake --build build --target truetest               # engine only
cmake --build build --target truetest_tests         # tests only
cmake --build build -j$(nproc)                      # parallel build

# Test
cd build && ctest                                   # run tests
cd build && ctest --output-on-failure               # show failures
cd build && ctest -R "Portfolio"                    # filter by name

# Clean
cmake --build build --target clean                  # remove object files
rm -rf build                                        # full clean
```

---

## Project Layout

```
hft-engine/
├── CMakeLists.txt                  # build config
├── market_data.csv                 # sample OHLCV data
├── instructions.md                 # this file
├── CLAUDE.md                       # AI assistant context
├── done.md                         # changelog
├── mt_todo.md                      # multithreading implementation plan
├── build/                          # build output (gitignored)
│   ├── truetest                    # engine binary
│   └── truetest_tests              # test binary
├── tests/
│   ├── test_main.cpp               # custom GTest entry point
│   ├── test_ui.h                   # ASCII art test reporter
│   ├── test_events.cpp             # event type tests
│   ├── test_orderbook.cpp          # matching engine tests
│   ├── test_portfolio.cpp          # position tracking tests
│   ├── test_fee_model.cpp          # commission model tests
│   ├── test_latency_model.cpp      # latency simulation tests
│   ├── test_fill_model.cpp         # fill probability tests
│   ├── test_sma.cpp                # indicator tests
│   ├── test_strategies.cpp         # strategy logic tests
│   ├── test_ring_buffer.cpp        # SPSC buffer tests
│   ├── test_bar_aggregator.cpp     # tick-to-bar tests
│   ├── test_analytics.cpp          # metrics tests
│   ├── test_market_maker.cpp       # liquidity provider tests
│   ├── test_data_handler.cpp       # data loading tests
│   ├── test_execution_adapter.cpp  # execution tests
│   ├── test_engine.cpp             # engine integration tests
│   ├── test_order_id.cpp           # ID generation tests
│   ├── test_price.cpp              # fixed-point price tests
│   ├── test_orderbook_registry.cpp # multi-symbol tests
│   ├── test_order_types.cpp        # order type tests
│   ├── test_object_pool.cpp        # allocation pool tests
│   ├── test_risk_manager.cpp       # risk checking tests
│   ├── test_event_log.cpp          # event serialization tests
│   ├── test_thread_preset.cpp      # preset logic tests
│   ├── test_threading_correctness.cpp # cross-preset validation
│   └── fixtures/
│       ├── sample_ohlcv.csv
│       └── sample_ticks.csv
└── BacktestEngine/src/
    ├── main.cpp                    # TUI entry point, CLI parsing
    ├── core/
    │   ├── engine.h/.cpp           # engine: run loop, worker management
    │   ├── engine_config.h         # config: mode, threading, models
    │   ├── event.h                 # event types: market, order, fill, tick, etc.
    │   └── event_log.h             # binary event logger/replayer
    ├── data/
    │   ├── data_source.h           # IDataSource interface
    │   ├── data_handler.h          # in-memory OHLCV + tick storage
    │   ├── data_loader.cpp         # CSV loading helpers
    │   ├── csv_data_source.h/.cpp  # OHLCV CSV backend
    │   ├── tick_csv_data_source.h/.cpp # tick CSV backend
    │   ├── binary_cache_source.h/.cpp  # caching decorator
    │   ├── pg_data_source.h/.cpp   # PostgreSQL backend (opt-in)
    │   └── websocket_data_source.h/.cpp # WebSocket feed (opt-in)
    ├── execution/
    │   ├── portfolio.h/.cpp        # position tracking, PnL
    │   ├── execution_adapter.h     # IExecutionAdapter, LocalBookAdapter
    │   ├── fee_model.h             # IFeeModel: zero, fixed, tiered
    │   └── latency_model.h         # ILatencyModel: zero, fixed, stochastic
    ├── orderbook/
    │   ├── orderbook.h/.cpp        # price-time priority matching
    │   ├── orderbook_registry.h    # multi-symbol book management
    │   └── fill_model.h            # IFillModel: perfect, realistic
    ├── strategy/
    │   ├── strategy_interface.h    # IStrategy: on_market, on_tick
    │   ├── mean_reversion_strategy.h/.cpp
    │   ├── sma_strategy.h/.cpp
    │   └── ma_crossover_strategy.h/.cpp
    ├── analytics/
    │   ├── analytics.h/.cpp        # Welford's streaming metrics, reports
    │   └── bar_aggregator.h        # tick-to-OHLCV bar conversion
    ├── market_maker/
    │   └── market_maker.h/.cpp     # volatility-based liquidity seeding
    ├── risk/
    │   ├── risk_manager.h/.cpp     # pre-order and post-fill risk checks
    │   └── (risk_limits struct)    # max position, drawdown, loss limits
    ├── threading/
    │   ├── thread_preset.h         # preset enum, selection, helpers
    │   ├── thread_config.h         # core detection, CPU pinning
    │   ├── ring_buffer.h           # lock-free SPSC ring (3 policies)
    │   ├── worker.h                # base Worker: run loop, exception capture
    │   ├── logging_worker.h        # binary + text logging with batching
    │   ├── risk_worker.h           # shadow portfolio risk checking
    │   ├── stats_worker.h          # independent analytics accumulation
    │   ├── observer_worker.h       # combined worker (light preset)
    │   ├── risk_stats_worker.h     # combined risk+stats (standard preset)
    │   └── market_maker_worker.h   # async MM with inbound order ring
    ├── types/
    │   ├── order_id.h              # atomic unique ID generator
    │   ├── object_pool.h           # block-based allocation pool
    │   └── price.h                 # fixed-point price type
    ├── indicator/
    │   └── sma.h                   # simple moving average
    └── utils/
        └── log/log.cpp             # logging stub
```
