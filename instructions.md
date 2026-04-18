# TrueTest — User Manual


TrueTest is a modular C++17 engine that starts as a backtesting platform but is
designed to be reused across deployments: pure backtesting, Polymarket execution,
MetaTrader EA, or anything that processes market data through a strategy and
orderbook pipeline.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Building from Source](#2-building-from-source)
3. [CMake Flags Reference](#3-cmake-flags-reference)
4. [Build Types and Optimization](#4-build-types-and-optimization)
5. [Sanitizers](#5-sanitizers)
6. [Install and Packaging](#6-install-and-packaging)
7. [Docker](#7-docker)
8. [Running TrueTest](#8-running-truetest)
9. [CLI Flags Reference](#9-cli-flags-reference)
10. [Configuration File](#10-configuration-file)
11. [Dry Run Mode](#11-dry-run-mode)
12. [Interactive TUI Mode](#12-interactive-tui-mode)
13. [Provider Mode](#13-provider-mode)
14. [Replay Mode](#14-replay-mode)
15. [Strategies](#15-strategies)
16. [Data Sources](#16-data-sources)
17. [Fee Models](#17-fee-models)
18. [Risk Management](#18-risk-management)
19. [Threading Model](#19-threading-model)
20. [WebSocket UI](#20-websocket-ui)
21. [Event Pipeline](#21-event-pipeline)
22. [SQLite Persistence](#22-sqlite-persistence)
23. [Analytics & Reporting](#23-analytics--reporting)
24. [Examples](#24-examples)
25. [Observability & Debugging](#25-observability--debugging)
26. [Error Handling & Resilience](#26-error-handling--resilience)
27. [Testing](#27-testing)
28. [Performance Benchmarks](#28-performance-benchmarks)
29. [Embedding: C API and Python Bindings](#29-embedding-c-api-and-python-bindings)

---

## 1. Prerequisites

**Required (zero-dependency core):**

- C++17 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- CMake 3.22+

**Optional (enabled via CMake flags):**

| Dependency       | Required for               | Install (Debian/Ubuntu)              |
|------------------|----------------------------|--------------------------------------|
| Boost headers    | WebSocket UI, Binance, live data | `sudo apt install libboost-all-dev` |
| OpenSSL          | Binance provider           | `sudo apt install libssl-dev`        |
| SQLite3          | Trade/portfolio persistence| `sudo apt install libsqlite3-dev`    |
| PostgreSQL       | PostgreSQL data source     | `sudo apt install libpq-dev`         |
| Abseil           | Debug instrumentation      | Fetched automatically via CMake      |
| GoogleTest       | Unit tests                 | Fetched automatically via CMake      |

---

## 2. Building from Source

### Minimal build (no external dependencies)

```bash
cmake -B build
cmake --build build
```

This produces the `build/truetest` binary with CSV data sources and the core
engine. SQLite is enabled by default and requires `libsqlite3-dev`.

### Full-featured build

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_WEB_UI=ON \
  -DENABLE_BINANCE=ON \
  -DENABLE_LIVE_DATA=ON \
  -DENABLE_SQLITE=ON \
  -DENABLE_DEBUG=ON \
  -DBUILD_TESTS=ON
cmake --build build
```

### Running tests

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite contains 283 tests. Two `EngineStreaming` tick tests are known
crashers and are expected to fail.

---

## 3. CMake Flags Reference

### Feature flags

Each optional feature is gated behind an `ENABLE_*` CMake option. When enabled,
the corresponding `HAS_*` preprocessor define is set, and additional source files
are compiled in.

| CMake Flag             | Default | Preprocessor Define  | Description                                                  | Dependencies            |
|------------------------|---------|----------------------|--------------------------------------------------------------|-------------------------|
| `ENABLE_SQLITE`        | **ON**  | `HAS_SQLITE`         | SQLite persistence for equity curve, trades, portfolio state | libsqlite3              |
| `ENABLE_POSTGRESQL`    | OFF     | `HAS_POSTGRESQL`     | PostgreSQL data source backend                               | libpq, libpqxx (auto-fetched) |
| `ENABLE_WEB_UI`        | OFF     | `HAS_WEB_UI`         | WebSocket server for browser-based live dashboard            | Boost headers           |
| `ENABLE_BINANCE`       | OFF     | `HAS_BINANCE`        | Binance exchange provider (live streaming + paper trading)   | Boost headers, OpenSSL  |
| `ENABLE_LIVE_DATA`     | OFF     | `HAS_LIVE_DATA`      | Generic WebSocket data source for custom feeds               | Boost.System            |
| `ENABLE_DEBUG`         | OFF     | `HAS_DEBUG`          | Performance instrumentation (stage timer, memory sampler)    | Abseil (auto-fetched)   |
| `BUILD_TESTS`          | OFF     | —                    | Build GoogleTest unit test binary (`truetest_tests`)         | GoogleTest (auto-fetched) |
| `ENABLE_BENCHMARKS`    | OFF     | —                    | Build Google Benchmark performance suite (`truetest_benchmarks`) | google/benchmark (auto-fetched) |
| `BUILD_SHARED_LIB`     | OFF     | —                    | Build `libtruetest` shared library with C API for embedding  | (none)                  |

### Sanitizer flags

| CMake Flag       | Default | Description                                           |
|------------------|---------|-------------------------------------------------------|
| `ENABLE_TSAN`    | OFF     | ThreadSanitizer — detects data races                  |
| `ENABLE_ASAN`    | OFF     | AddressSanitizer — detects memory errors              |
| `ENABLE_UBSAN`   | OFF     | UndefinedBehaviorSanitizer — detects undefined behavior |

TSAN is mutually exclusive with ASAN and UBSAN. Enabling both causes a fatal
CMake error. ASAN and UBSAN can be combined.

---

## 4. Build Types and Optimization

Set the build type with `-DCMAKE_BUILD_TYPE=<type>`:

| Build Type       | Optimization | Debug Info | LTO  | Assertions | Use Case                    |
|------------------|-------------|------------|------|------------|-----------------------------|
| `Release`        | `-O3`       | No         | Yes  | Disabled (`-DNDEBUG`) | Production, benchmarking    |
| `RelWithDebInfo` | `-O2`       | `-g`       | No   | Enabled    | Profiling with debug symbols |
| `Debug`          | `-O0`       | `-g`       | No   | Enabled    | Development, sanitizers      |
| (none)           | Compiler default | —    | No   | Enabled    | Quick iteration              |

On GCC Debug builds, `_GLIBCXX_DEBUG` is defined to enable checked iterators.

LTO (Link-Time Optimization) is applied in Release builds via `-flto` on both
compile and link steps. This enables cross-translation-unit inlining and dead
code elimination.

---

## 5. Sanitizers

### ThreadSanitizer (TSAN)

Detects data races, deadlocks, and lock-order inversions in multithreaded code.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DBUILD_TESTS=ON
cmake --build build
TSAN_OPTIONS="halt_on_error=1" ctest --test-dir build
```

### AddressSanitizer (ASAN)

Detects heap/stack buffer overflows, use-after-free, double-free, and memory leaks.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

### UndefinedBehaviorSanitizer (UBSAN)

Detects signed integer overflow, null pointer dereference, misaligned access, and
other undefined behavior.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

ASAN and UBSAN can be combined:

```bash
cmake -B build -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
```

TSAN cannot be combined with either ASAN or UBSAN — this is a compiler constraint.

---

## 6. Install and Packaging

### Installing locally

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /usr/local
```

This installs:
- `<prefix>/bin/truetest` — the engine binary
- `<prefix>/share/truetest/web/index.html` — the WebSocket dashboard

### Creating packages

TrueTest uses CPack for generating distributable packages.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && cpack
```

This produces:
- `truetest-0.1.0-Linux.tar.gz` — portable tarball
- `truetest-0.1.0-Linux.deb` — Debian/Ubuntu package

Install the `.deb` package:

```bash
sudo dpkg -i truetest-0.1.0-Linux.deb
```

---

## 7. Docker

A multi-stage Dockerfile is provided for reproducible builds and deployment.

### Building the image

```bash
docker build -t truetest .
```

The build stage compiles with `Release` optimization and enables `WEB_UI`,
`BINANCE`, and `SQLITE`. The runtime image is based on `debian:bookworm-slim`
and contains only the binary, web assets, and minimal runtime libraries.

### Running

```bash
# Start with WebSocket UI (default)
docker run -p 8765:8765 truetest

# Backtest with a mounted CSV file
docker run -v /path/to/data:/data truetest \
  ./truetest --provider local --path /data/market_data.csv --strategy sma

# Custom port
docker run -p 9000:9000 truetest \
  ./truetest --web-ui --ws-port 9000
```

Port 8765 is exposed by default for the WebSocket UI.

The `.dockerignore` excludes `build/`, `.git/`, and `.idea/` from the build
context.

---

## 8. Running TrueTest

TrueTest has three runtime modes determined by the flags you pass:

| Mode          | Trigger                    | Description                              |
|---------------|----------------------------|------------------------------------------|
| **TUI**       | No `--provider` or `--replay` | Interactive menu-driven setup            |
| **Provider**  | `--provider <name>`        | Headless run using a registered provider |
| **Replay**    | `--replay <path>`          | Replay a binary event log                |

```bash
# TUI mode
./build/truetest

# Provider mode
./build/truetest --provider local --path market_data.csv --strategy sma

# Replay mode
./build/truetest --replay event_log.bin
```

---

## 9. CLI Flags Reference

### Core flags

| Flag                 | Type     | Default         | Description                                               |
|----------------------|----------|-----------------|-----------------------------------------------------------|
| `--mode`             | string   | `backtest`      | Engine mode: `backtest`, `shadow`, or `live`               |
| `--provider`         | string   | (none)          | Provider name: `local` or `binance`                       |
| `--strategy`         | string   | `mean-reversion`| Strategy name (registry-based: `mean-reversion`, `sma`, `ma-crossover`) |
| `--path`             | string   | (none)          | Data file path (for `local` provider)                     |
| `--replay`           | string   | (none)          | Binary event log path for replay mode                     |
| `--replay-from`      | int64    | `0`             | Replay from timestamp (microseconds since epoch)          |
| `--replay-to`        | int64    | max             | Replay to timestamp (microseconds since epoch)            |
| `--seed`             | uint64   | `0`             | RNG seed for determinism (`0` = non-deterministic)         |
| `--balance`          | double   | `10000.0`       | Initial account balance                                   |
| `--db`               | string   | `truetest.db`   | SQLite database path                                      |
| `--no-db`            | flag     | (off)           | Disable SQLite persistence entirely                       |
| `--checkpoint`       | string   | (none)          | Write periodic portfolio checkpoint to this binary file    |
| `--checkpoint-interval` | size_t | `10000`        | Write a checkpoint every N events (0 = only at shutdown)   |
| `--resume`           | string   | (none)          | Restore portfolio state from a checkpoint before running   |
| `--live`             | flag     | (off)           | Safety flag required to enable live (real money) mode      |

### Strategy parameters

| Flag                 | Type     | Default  | Description                                   |
|----------------------|----------|----------|-----------------------------------------------|
| `--sma-period`       | size_t   | `20`     | SMA indicator period (legacy, prefer `--param period=N`) |
| `--risk-fraction`    | double   | `0.02`   | Position size as fraction of equity (2%)      |
| `--sl`               | double   | `0.005`  | Stop loss fraction of entry price (0.5%)      |
| `--tp`               | double   | `0.01`   | Take profit fraction of entry price (1.0%)    |
| `--param`            | string   | (none)   | Strategy parameter as `key=value` (repeatable) |

### Fee model flags

| Flag                 | Type     | Default  | Description                                   |
|----------------------|----------|----------|-----------------------------------------------|
| `--fee`              | string   | (none)   | Fee model: `fixed` or `tiered`                |
| `--fee-value`        | double   | `0.0`    | Fixed fee per trade (when `--fee fixed`)      |
| `--maker-rate`       | double   | `0.0`    | Maker fee rate (when `--fee tiered`)          |
| `--taker-rate`       | double   | `0.0`    | Taker fee rate (when `--fee tiered`)          |

### Provider/streaming flags

| Flag                 | Type     | Default  | Description                                      |
|----------------------|----------|----------|--------------------------------------------------|
| `--symbol`           | string   | (none)   | Trading symbol (e.g., `BTCUSDT`)                 |
| `--stream`           | string   | (none)   | Stream type: `trade`, `kline`, or interval like `1h` |
| `--format`           | string   | (none)   | Data format: `tick` or `bar`                     |
| `--host`             | string   | (none)   | WebSocket host (e.g., `ws.binance.com:9443`)     |
| `--port`             | string   | (none)   | Port number (alternative to embedding in host)   |
| `--api-key`          | string   | (none)   | API key for exchange authentication              |
| `--api-secret`       | string   | (none)   | API secret for exchange authentication           |
| `--backfill`         | int      | `500`    | Historical bars to fetch before streaming starts |
| `--backfill-interval`| string   | (none)   | Kline interval for backfill (defaults to stream interval) |
| `--record`           | string   | (none)   | Path to record raw market data to file           |
| `--replay-data`      | string   | (none)   | Path to replay previously recorded market data   |

### Threading flags

| Flag                 | Type     | Default      | Description                                     |
|----------------------|----------|--------------|--------------------------------------------------|
| `--thread-preset`    | string   | auto-detect  | Preset: `inline`, `light`, `standard`, `full`, `extended` |
| `--no-pin`           | flag     | (off)        | Disable CPU affinity pinning                     |
| `--spin-policy`      | string   | `adaptive`   | Worker spin policy: `spin`, `yield`, or `adaptive` |

### Logging flags

| Flag                 | Type     | Default  | Description                                      |
|----------------------|----------|----------|--------------------------------------------------|
| `--log-events`       | string   | (none)   | Path to write binary event log                   |
| `--compress-log`     | flag     | on       | Compress binary event logs with zstd             |
| `--no-compress-log`  | flag     | (off)    | Disable zstd compression for event logs          |
| `--log-file`         | string   | (none)   | Path to write operational log (L1). Default sink is stderr. |
| `--log-max-size`     | uint     | `0`      | Max size (MB) per event/text log before rotation (0 = disabled, L3) |
| `--log-keep`         | int      | `5`      | Number of rotated log files to retain (L3)       |

### WebSocket UI flags

| Flag                 | Type     | Default  | Description                                      |
|----------------------|----------|----------|--------------------------------------------------|
| `--web-ui`           | flag     | (off)    | Enable WebSocket UI server                       |
| `--ws-port`          | uint16   | `8765`   | WebSocket server listen port                     |
| `--ws-compress`      | flag     | on       | Enable per-message deflate compression           |
| `--no-ws-compress`   | flag     | (off)    | Disable per-message deflate compression          |

### Execution constants

| Flag                 | Type     | Default  | Description                                      |
|----------------------|----------|----------|--------------------------------------------------|
| `--aggression`       | double   | `1.1`    | Market order aggression factor (buy: price×aggr, sell: price×(2−aggr)) |
| `--qty-scale`        | double   | `1e8`    | Fractional quantity → integer scale factor       |
| `--fill-rng-seed`    | unsigned | `42`     | RNG seed for fill model probability rolls        |
| `--spread-step`      | double   | `0.0001` | Spread step factor (mid × factor per level)      |

### Analytics and reporting flags

| Flag                 | Type     | Default  | Description                                      |
|----------------------|----------|----------|--------------------------------------------------|
| `--rolling-window`   | size_t   | `252`    | Rolling metrics window size (number of bars)     |
| `--risk-free-rate`   | double   | `0.0`    | Annual risk-free rate for excess return calcs (Sharpe/Sortino) |
| `--output`           | string   | (none)   | Write results to file (omit for no file export)  |
| `--output-format`    | string   | `json`   | Output format: `json` or `csv`                   |

### Risk management flags

| Flag                    | Type     | Default  | Description                                      |
|-------------------------|----------|----------|--------------------------------------------------|
| `--max-daily-loss`      | double   | `0`      | Maximum daily loss before halt (0 = no limit)    |
| `--daily-reset-hour`    | int      | `0`      | UTC hour (0-23) to reset daily loss counter      |
| `--max-trades-per-hour` | int      | `0`      | Maximum fills per rolling hour (0 = no limit)    |
| `--max-orders-per-minute`| int     | `0`      | Maximum orders per rolling minute (0 = no limit) |
| `--risk-unwind`         | flag     | (off)    | On risk halt, unwind all positions before stopping |

### Configuration and diagnostics flags

| Flag                 | Type     | Default  | Description                                      |
|----------------------|----------|----------|--------------------------------------------------|
| `--config`           | string   | (none)   | Load configuration from a JSON file              |
| `--dump-config`      | flag     | (off)    | Print resolved config as JSON and exit           |
| `--dry-run`          | flag     | (off)    | Validate config, print summary, and exit         |

CLI parsing uses [CLI11](https://github.com/CLIUtils/CLI11). Unknown flags are
rejected with an error message. Run `--help` for a full list of options with
defaults, or `--help-all` for expanded help.

---

## 10. Configuration File

TrueTest supports loading configuration from a JSON file via `--config <path>`.
CLI flags always override values from the config file.

### Config file format

The config file is a JSON object. All keys are optional — omitted keys use their
default values. Key names use underscores (e.g., `sma_period`, not `--sma-period`).

```json
{
  "strategy": "sma",
  "mode": "backtest",
  "balance": 50000.0,
  "sma_period": 50,
  "risk_fraction": 0.02,
  "sl": 0.005,
  "tp": 0.01,
  "seed": 12345,
  "provider": "local",
  "path": "market_data.csv",
  "fee": "tiered",
  "maker_rate": 0.001,
  "taker_rate": 0.002,
  "db": "results.db",
  "thread_preset": "standard",
  "web_ui": false,
  "ws_port": 8765,
  "ws_compress": true,
  "symbol": "BTCUSDT",
  "stream": "trade",
  "backfill": 500,
  "backfill_interval": "1m",
  "risk": {
    "max_drawdown": 0.20,
    "max_position_value": 100000,
    "max_loss_per_trade": 5000,
    "max_open_orders": 100,
    "max_portfolio_exposure": 500000
  }
}
```

### CLI overrides config file

When both `--config` and CLI flags are specified, CLI flags take precedence:

```bash
# Config file sets balance=50000, but CLI overrides to 10000
./build/truetest --config config.json --balance 10000
```

### Dump resolved config

Use `--dump-config` to print the fully resolved configuration (after merging
config file + CLI overrides) as JSON and exit:

```bash
./build/truetest --config config.json --balance 10000 --dump-config
```

This is useful for debugging configuration issues or generating a config file
template from your current CLI flags.

---

## 11. Dry Run Mode

The `--dry-run` flag validates the resolved configuration, prints a human-readable
summary, and exits without running the engine.

```bash
./build/truetest --dry-run --strategy sma --balance 50000 --mode backtest
```

Output:

```
  ============================================
    Dry-run config validation
  ============================================
    Mode:       backtest
    Strategy:   sma
    SMA Period: 20
    Balance:    $50000
    ...
  ── Risk Limits ──
    Max drawdown:        30%
    ...
  ============================================

  Config is VALID.
```

Exit codes:
- `0` — configuration is valid
- `1` — configuration has errors (unknown strategy, invalid mode, etc.)

Validated fields:
- `--strategy` must be a registered strategy name (built-in: `mean-reversion`, `sma`, `ma-crossover`)
- `--mode` must be one of: `backtest`, `shadow`, `live`
- `--fee` must be one of: `fixed`, `tiered`
- `--thread-preset` must be a valid preset name

This is useful for CI pipelines and deployment validation — verify your config
without starting the engine or requiring data files.

---

## 12. Interactive TUI Mode

When launched without `--provider` or `--replay`, TrueTest presents an interactive
text menu. The menus walk you through:

1. **Strategy selection** — Mean Reversion, SMA, or MA Crossover
2. **SMA period** — numeric input (default: 20)
3. **Data source** — PostgreSQL (if compiled), CSV file, or Tick CSV file
4. **Fee model** — Zero fees, Fixed fee, or Tiered (maker/taker)
5. **Engine mode** — Backtest, Shadow, or Live

Each menu shows numbered options. Enter the number and press Enter. Invalid input
falls back to the default (typically option 1).

PostgreSQL appears in the data source menu only when built with
`-DENABLE_POSTGRESQL=ON`. Otherwise it shows as unavailable.

---

## 13. Provider Mode

Providers are self-registering modules that handle data sourcing and execution.
Use `--provider <name>` to bypass the TUI and run headless.

### Local provider

File-based CSV data. Batch mode only.

```bash
./build/truetest --provider local --path market_data.csv --strategy sma
```

Supports OHLCV bar format and tick format. The format is auto-detected from the
CSV headers. Use `--format tick` to force tick parsing.

```bash
./build/truetest \
  --provider local \
  --path ticks.csv \
  --format tick \
  --strategy mean-reversion
```

### Binance provider

Live WebSocket streaming from Binance spot market. Requires
`-DENABLE_BINANCE=ON` at build time.

```bash
# Paper trading (shadow mode)
./build/truetest \
  --provider binance \
  --symbol btcusdt \
  --stream trade \
  --mode shadow \
  --strategy mean-reversion

# With kline stream
./build/truetest \
  --provider binance \
  --symbol ethusdt \
  --stream kline_1m \
  --mode shadow \
  --backfill 200
```

Supported streams:
- `trade` — individual trade ticks
- `kline_1m` — 1-minute candlestick aggregation
- `depth` — L2 orderbook depth (used by `HybridExecutor` for realistic
  paper-mode limit fills against real exchange depth)

### L2 depth stream (paper fills against real book)

```bash
./build/truetest \
  --provider binance \
  --symbol btcusdt \
  --stream depth \
  --mode shadow \
  --strategy mean-reversion \
  --backfill 200 \
  --web-ui
```

The local orderbook receives live snapshots and incremental updates, so
limit orders are matched against the real book instead of the synthetic
MarketMaker-seeded one.

### Recording live data to a file

```bash
./build/truetest \
  --provider binance \
  --symbol btcusdt \
  --stream trade \
  --mode shadow \
  --strategy sma \
  --record captured_ws.bin
```

Captures the raw WebSocket stream to `captured_ws.bin` while the engine
runs. Useful for building deterministic fixtures from real exchange data.

### Replaying a recorded WebSocket file

```bash
./build/truetest \
  --provider binance \
  --symbol btcusdt \
  --stream trade \
  --mode shadow \
  --strategy sma \
  --replay-data captured_ws.bin
```

Replays the captured file as if it were the live feed — same parser,
provider, and executor paths as the live run. Pairs well with
`--seed <n>` for fully deterministic shadow runs against real data.

### Live trading (real orders)

```bash
./build/truetest \
  --provider binance \
  --symbol btcusdt \
  --stream kline_1m \
  --mode live \
  --live \
  --api-key "$BINANCE_API_KEY" \
  --api-secret "$BINANCE_API_SECRET" \
  --strategy sma \
  --balance 1000 \
  --max-daily-loss 50 \
  --risk-unwind
```

**This submits real orders against Binance.** Both the `--mode live` flag
and the `--live` safety flag are required, and the CLI prints an explicit
confirmation prompt that requires typing `YES` before proceeding. Orders
are signed with HMAC-SHA256 via `BinanceRestClient` and submitted to
`/api/v3/order`. Always pair with tight `--max-daily-loss`,
`--max-trades-per-hour`, and `--risk-unwind` so a bug cannot drain the
account.

---

## 14. Replay Mode

Replay a previously recorded binary event log:

```bash
./build/truetest --replay event_log.bin
```

Event logs are recorded during a run with `--log-events <path>`. Logs are
compressed with zstd by default (disable with `--no-compress-log`). The replayer
auto-detects compressed vs uncompressed files.

### Time-based seeking

Event logs include an index that maps timestamps to file offsets (one entry per
1000 events). Use `--replay-from` and `--replay-to` to replay a time range
instead of the entire file:

```bash
# Replay events between two timestamps (microseconds since epoch)
./build/truetest --replay event_log.bin \
  --replay-from 1700000000000000 \
  --replay-to   1700003600000000
```

When an index is present, the replayer binary-searches for the start offset
instead of scanning from the beginning.

---

## 15. Strategies

Strategies are registered via the `StrategyRegistry` and looked up by name at
runtime. All strategies support runtime parameter configuration via `--param
key=value` (repeatable). Each strategy defines its accepted parameters through
`get_param_schema()`, which returns the name, default, min, max, and description
for each parameter.

### Mean Reversion (`mean-reversion`)

The default strategy. Buys when the current price drops below the SMA and sells
when it rises above it. Uses configurable stop-loss and take-profit levels.

Parameters (via `--param` or legacy flags):
- `period` (default: 20) — SMA lookback period (`--sma-period`)
- `equity` (default: 10000) — account equity for position sizing (`--balance`)
- `risk_fraction` (default: 0.02) — fraction of equity per trade (`--risk-fraction`)
- `sl_pct` (default: 0.005) — stop loss as fraction of entry price (`--sl`)
- `tp_pct` (default: 0.01) — take profit as fraction of entry price (`--tp`)

```bash
./build/truetest \
  --provider local --path market_data.csv \
  --strategy mean-reversion \
  --param period=30 \
  --param risk_fraction=0.01 \
  --param sl_pct=0.003 \
  --param tp_pct=0.006
```

### SMA Strategy (`sma`)

Generates signals based on price crossing above or below a simple moving average.
Buys when price > SMA, sells when price < SMA.

Parameters:
- `period` (default: 20) — SMA lookback period

```bash
./build/truetest \
  --provider local --path market_data.csv \
  --strategy sma \
  --param period=50
```

### MA Crossover (`ma-crossover`)

True dual-SMA crossover strategy. Generates buy signals on golden crosses (fast
SMA crosses above slow SMA) and sell signals on death crosses (fast SMA crosses
below slow SMA).

Parameters:
- `fast_period` (default: 10) — fast SMA lookback period
- `slow_period` (default: 50) — slow SMA lookback period

Example:
```bash
./build/truetest --strategy ma-crossover --param fast_period=5 --param slow_period=20
```

### Indicators

The following indicators are available for use in strategies:

| Indicator | Header | Class | Key method |
|-----------|--------|-------|------------|
| Simple Moving Average | `indicator/sma.h` | `simple_moving_average` | `update(price) → optional<double>` |
| Exponential Moving Average | `indicator/ema.h` | `exponential_moving_average` | `update(price) → optional<double>` |
| Relative Strength Index | `indicator/rsi.h` | `relative_strength_index` | `update(price) → optional<double>` |
| Bollinger Bands | `indicator/bollinger.h` | `bollinger_bands` | `update(price) → optional<bollinger_result>` |

All indicators follow the same pattern: call `update(price)` on each bar/tick,
returns `std::nullopt` during warmup, then returns computed values once enough
data has been accumulated. Use `ready()` to check and `value()` to get the last
computed result.

### Strategy Registry

Strategies self-register via the `REGISTER_STRATEGY("name", factory)` macro
(mirroring the provider registry pattern). To add a new strategy:

1. Create the strategy class implementing `IStrategy`
2. Add `REGISTER_STRATEGY` in the `.cpp` file with a factory lambda
3. The strategy becomes available by name via `--strategy <name>`

All strategies expose their indicator values (via `get_indicator_values()`) for
use in analytics and the WebSocket UI.

### Running Multiple Strategies Simultaneously

`--strategy` accepts a comma-separated list of registered strategy names. The
first entry becomes the **primary** strategy; subsequent entries run alongside
it and share the same portfolio, orderbook registry, and risk manager.

```bash
./build/truetest --provider local --path market_data.csv \
  --strategy mean-reversion,sma,ma-crossover \
  --param period=20 --param fast_period=10 --param slow_period=50
```

Behaviour:

- On every market/tick event, the primary strategy is dispatched first, then
  each additional strategy in order.
- Orders emitted by each strategy are tagged with the originating strategy
  name via `order_event::strategy_name`. Fills inherit the tag, which enables
  per-strategy attribution in analytics and the event log.
- SL/TP stops (`check_stops()`) are evaluated independently for every
  strategy — each manages its own position state via `set_position_open()`.
- `--param key=value` flags apply to **all** strategies in the list. Strategies
  silently ignore parameters they don't recognize, so the same flag set can
  target a heterogeneous mix.
- Risk checks, fees, and the orderbook are global; combined exposure is what
  the risk manager sees, not per-strategy exposure.

Multi-strategy mode works in batch, streaming, tick, and replay loops. The
primary strategy's name is also passed to `engine::set_primary_strategy_name()`
so its own orders are tagged for attribution.

---

## 16. Data Sources

### Data Validation

All data records (bars and ticks) are validated on load. Invalid records are
logged with a warning and skipped. The `data_handler::validation_errors()` method
returns the count of rejected records. Validation rules:

- **Bars**: `open`, `high`, `low`, `close` must be positive; `high >= low`;
  `volume >= 0`
- **Ticks**: `price` must be positive; `quantity` must be positive; timestamps
  must be monotonically increasing

### CSV (OHLCV bars)

Standard candlestick format. Expected CSV headers:

```
timestamp,symbol,open,high,low,close,volume
```

Timestamps should be ISO 8601 or Unix epoch. Rows are loaded in order into the
in-memory `data_handler` before the engine runs.

### Tick CSV

Individual trade-level data. Expected format:

```
timestamp_ms,symbol,price,quantity,side
```

Where `side` is `B` (buy aggressor), `A` (sell aggressor), or empty/`U` (unknown).

### Binary cache

The `BinaryCacheSource` wraps any other data source and caches the parsed result
as a binary file. On subsequent runs with the same data, loading is significantly
faster because the CSV parse step is skipped.

Cache files use a versioned binary format with integrity checking:

- **Header** (16 bytes): 4-byte magic (`TTBC`), 2-byte format version (currently
  1), 2-byte reserved, 8-byte CRC-64 checksum of the payload
- On read, the magic, version, and checksum are verified. Incompatible or corrupt
  cache files are automatically deleted and regenerated from the fallback source.

The cache is transparent: no CLI flag is required. A `<path>.cache` file is
written alongside the data file on first run and picked up automatically on
subsequent runs with the same `--path`.

```bash
# First run — populates market_data.csv.cache as a side effect
./build/truetest --provider local --path market_data.csv --strategy sma

# Second run — loads from the binary cache (much faster startup)
./build/truetest --provider local --path market_data.csv --strategy sma
```

### PostgreSQL

Available when built with `-DENABLE_POSTGRESQL=ON`. Connects to a PostgreSQL
database and loads OHLCV data via SQL queries. Connection parameters are
prompted in TUI mode — there is no dedicated CLI flag set yet, so launch
without `--provider` to reach the interactive menu:

```bash
./build/truetest
# Select: Data source → PostgreSQL, then enter host/db/user/password/query
```

### WebSocket data source

Available when built with `-DENABLE_LIVE_DATA=ON`. Connects to a generic
WebSocket endpoint for live streaming data. The parser supports two JSON message
formats:

**Tick format:**
```json
{"type":"tick","symbol":"BTCUSDT","price":50000.0,"qty":0.5,"ts":1700000000000}
```

**Bar format:**
```json
{"type":"bar","symbol":"BTCUSDT","o":49900,"h":50100,"l":49800,"c":50000,"v":1000,"ts":1700000000000}
```

Fields:
- `type` (required): `"tick"` or `"bar"`
- `symbol` (required): trading pair identifier
- `ts` (optional): timestamp in milliseconds since epoch (defaults to current time)
- `seq` (optional): sequence number for gap detection

The source includes automatic reconnection with exponential backoff, heartbeat
keepalive, and sequence number gap detection.

The generic WebSocket data source is wired through the TUI rather than a
dedicated CLI flag set; launch without `--provider` and choose it from the
data source menu:

```bash
./build/truetest
# Select: Data source → Live WebSocket, then enter host/port/symbol
```

### Multi-Symbol Backtesting

The engine supports running a single strategy (or a multi-strategy ensemble)
across multiple symbols in the same run. Every symbol gets its own orderbook
automatically via the `OrderbookRegistry`, and bars/ticks are routed to the
correct book by their `symbol` field.

For the **local** provider, comma-separated `--path` loads several CSV files
into the same `data_handler`:

```bash
./build/truetest --provider local \
  --path btcusdt.csv,ethusdt.csv,solusdt.csv \
  --strategy mean-reversion
```

Each file is parsed in sequence, then all bars are stably sorted by their
`timestamp` column so events interleave chronologically across symbols. The
engine's main loop iterates the merged timeline and dispatches each bar to
the strategy; strategies that keep per-symbol state (e.g. SMA windows in
`unordered_map<string, ...>`) transparently track every symbol they see.

Notes:

- The risk manager, portfolio, analytics, and fee model are **global** — all
  symbols share the same cash, exposure, and PnL accounting.
- Per-symbol analytics attribution is available via `fill_event::get_symbol()`
  in the event log.
- For streaming providers (Binance), multiple symbols require subscribing to
  multiple websocket streams; currently only a single `--symbol` is honored
  in streaming mode. Use batch mode with multi-file `--path` for multi-symbol
  backtests today.

---

## 17. Fee Models

Fee models are applied to every fill event to calculate commission.

### Zero fees (default)

No commission charged. Suitable for initial strategy development.

### Fixed fee (`--fee fixed --fee-value <amount>`)

A flat fee per trade regardless of size.

```bash
./build/truetest --provider local --path data.csv --fee fixed --fee-value 1.50
```

### Tiered fee (`--fee tiered --maker-rate <rate> --taker-rate <rate>`)

Separate maker and taker rates applied as a fraction of trade value.

```bash
./build/truetest --provider local --path data.csv \
  --fee tiered --maker-rate 0.001 --taker-rate 0.002
```

This applies 0.1% for maker orders and 0.2% for taker orders (typical exchange
fee structure).

---

## 18. Risk Management

The risk manager validates orders and fills against configurable limits. It
returns one of four actions:

| Action   | Effect                                                      |
|----------|-------------------------------------------------------------|
| `pass`   | Order/fill accepted, processing continues                   |
| `reject` | Order rejected (rejection event emitted), engine continues  |
| `halt`   | Engine stopped immediately                                  |
| `unwind` | All open positions closed via market orders, then engine halts |

Risk checks run **synchronously on the hot path** (pre-order and post-fill) to
provide real-time rejection. When a risk check rejects an order, a `rejection`
event is emitted through the full event pipeline (ring buffers, event log,
WebSocket UI). The async shadow check in the risk worker thread remains as a
secondary validation layer.

### Static risk limits

| Limit                  | Default         | CLI flag                  | Triggers         |
|------------------------|-----------------|---------------------------|------------------|
| Max position value     | $1,000,000,000  | (config file only)        | Order rejection  |
| Max drawdown           | 30%             | (config file only)        | Engine halt      |
| Max loss per trade     | $10,000         | (config file only)        | Engine halt      |
| Max open orders        | 1,000           | (config file only)        | Order rejection  |
| Max portfolio exposure | $5,000,000,000  | (config file only)        | Order rejection  |

### Time-based risk limits

| Limit                  | Default      | CLI flag                   | Triggers         |
|------------------------|-------------|----------------------------|------------------|
| Max daily loss         | 0 (off)     | `--max-daily-loss`         | Engine halt      |
| Daily reset hour (UTC) | 0           | `--daily-reset-hour`       | —                |
| Max trades per hour    | 0 (off)     | `--max-trades-per-hour`    | Engine halt      |
| Max orders per minute  | 0 (off)     | `--max-orders-per-minute`  | Order rejection  |

Time-based limits use rolling windows (trades/hour = rolling 60 minutes,
orders/minute = rolling 60 seconds). Daily loss resets at the configured UTC
hour (default: midnight). A value of `0` disables the limit.

### Automatic position unwinding

By default, a risk halt stops the engine immediately. With `--risk-unwind`, the
engine first closes all open positions by submitting market sell orders, then
halts. This prevents leaving orphaned positions on a live exchange.

```bash
# Halt and unwind positions if drawdown exceeds 10%
./build/truetest --provider binance --symbol btcusdt --stream trade \
    --max-daily-loss 500 --max-trades-per-hour 100 --risk-unwind
```

### Risk in config files

All risk limits are available in the JSON config file under the `risk` key:

```json
{
  "risk": {
    "max_position_value": 100000,
    "max_drawdown": 0.10,
    "max_loss_per_trade": 500,
    "max_open_orders": 50,
    "max_portfolio_exposure": 500000,
    "max_daily_loss": 1000,
    "daily_reset_hour": 0,
    "max_trades_per_hour": 200,
    "max_orders_per_minute": 60
  }
}
```

The defaults are deliberately permissive. For production or live trading, you
should set appropriate limits via the config or CLI flags.

---

## 19. Threading Model

TrueTest uses a lock-free architecture with SPSC (single-producer,
single-consumer) ring buffers for inter-thread communication. The event loop
always runs on the main thread (ideally Core 0). Worker threads consume events
from their dedicated ring buffers.

### Presets

The threading preset is auto-detected from the physical core count or set
explicitly with `--thread-preset`:

| Preset     | Core Count | Workers | Description                              |
|------------|-----------|---------|------------------------------------------|
| `inline`   | 1-2       | 0       | Single-threaded — no workers, all processing on main thread |
| `light`    | 3         | 1       | One combined observer worker (logging + risk + stats) |
| `standard` | 4-5       | 2       | Logging worker + combined risk/stats worker |
| `full`     | 6-7       | 3       | Logging + risk + stats (each on its own thread) |
| `extended` | 8+        | 4       | All above + market maker replenish worker |

### CPU affinity

On Linux, both the event loop thread and all worker threads are pinned to
specific CPU cores via `sched_setaffinity` to reduce context switching and
cache thrashing. The event loop is pinned to Core 0 (or the core specified by
`engine_config::pin_event_loop`). Worker threads are pinned according to the
core map derived from the system topology.

Use `--no-pin` to disable all pinning (useful in containers or VMs with
restricted CPU access).

```bash
./build/truetest \
  --provider local --path market_data.csv --strategy sma \
  --thread-preset standard \
  --no-pin \
  --spin-policy yield
```

### Worker spin policy

Worker threads poll their ring buffers in a loop. The `--spin-policy` flag
controls how workers behave when no events are available:

| Policy     | Behavior                                                              |
|------------|-----------------------------------------------------------------------|
| `spin`     | Pure busy-wait — lowest latency, highest CPU usage                   |
| `yield`    | Always calls `std::this_thread::yield()` — lowest CPU, higher latency |
| `adaptive` | Exponential backoff: spin 64 iterations, then `_mm_pause` for 256, then yield (default) |

The `adaptive` policy balances latency and CPU usage: sub-microsecond wake-up
during event bursts, near-zero CPU when idle.

### Ring buffers

Each worker has a ring buffer with 65,536 slots (configurable via
`engine_config::ring_buffer_capacity`). The ring buffer uses atomic
load/store operations — no mutexes or syscalls on the hot path.

**Watermark metrics** — each ring buffer tracks:

- `high_watermark()` — maximum observed occupancy since construction (always-on,
  one relaxed CAS per push)
- `drop_count()` — number of events dropped by `DropOldest` policy
- `on_watermark(threshold, callback)` — optional callback fired when occupancy
  exceeds a threshold (e.g., 75% of capacity)

High watermark statistics are printed at engine shutdown alongside drop counts.
These metrics are always available, not gated behind `HAS_DEBUG`.

---

## 20. WebSocket UI

When `--web-ui` is passed, TrueTest starts a Boost.Beast server that handles both
WebSocket connections and HTTP REST requests on the same port.

```bash
./build/truetest --provider binance --symbol btcusdt --stream trade --web-ui
```

Then open `web/index.html` in a browser (or serve it via any HTTP server). The
dashboard connects to `ws://localhost:8765` and renders:

- Live equity curve
- Fill history
- Open positions
- Analytics (Sharpe, Sortino, drawdown, win rate)
- Orderbook visualization

### Per-message deflate compression

WebSocket messages are compressed using the permessage-deflate extension by
default, typically reducing JSON payload size by 60-80%. The extension is
negotiated during the WebSocket handshake — clients that don't support it
fall back to uncompressed transparently.

Disable with `--no-ws-compress` if compression overhead is undesirable (e.g.,
very low latency requirements).

### Command validation

All incoming WebSocket commands are validated against a per-command schema before
processing. Malformed commands receive an error response:

```json
{"type": "error", "data": {"message": "order: missing required field 'side'", "source": "ws_validator"}}
```

Both `"command"` and `"cmd"` field names are accepted. Unrecognized command names
are rejected. Per-command field requirements:

- **order**: requires `side` (buy/sell), `quantity` (> 0); `type` defaults to
  "market" if omitted; limit orders require `price` > 0
- **set_timeframe**: requires `timeframe`
- **set_symbol**: requires `value`
- **set_strategy**: requires `value`
- **start**, **pause**, **stop**: no additional fields required

### Client commands

The WebSocket connection is bidirectional. Clients can send JSON commands:

```json
{"cmd": "start"}
{"cmd": "pause"}
{"cmd": "stop"}
{"cmd": "order", "side": "buy", "quantity": 0.1, "price": 50000, "type": "limit"}
{"cmd": "set_timeframe", "timeframe": "1h"}
{"cmd": "set_symbol", "value": "ETHUSDT"}
{"cmd": "set_strategy", "value": "sma"}
{"cmd": "subscribe", "events": ["fill", "tick"], "symbols": ["BTCUSDT"]}
```

| Command         | Fields                              | Description                 |
|-----------------|-------------------------------------|-----------------------------|
| `start`         | —                                   | Resume engine execution     |
| `pause`         | —                                   | Pause engine                |
| `stop`          | —                                   | Halt engine                 |
| `order`         | `side`, `quantity`, `price`, `type`  | Submit a manual order       |
| `set_timeframe` | `timeframe`                         | Change kline interval       |
| `set_symbol`    | `value`                             | Switch trading symbol       |
| `set_strategy`  | `value`                             | Switch active strategy      |
| `subscribe`     | `events`, `symbols`                 | Filter events for this session |

### Event filtering (subscribe)

Clients can subscribe to specific event types and/or symbols to reduce bandwidth.
Send a `subscribe` command after connecting:

```json
{"cmd": "subscribe", "events": ["fill", "market"], "symbols": ["BTCUSDT", "ETHUSDT"]}
```

- **events**: array of event type strings to receive (e.g. `fill`, `tick`,
  `market`, `order`, `status`, `orderbook`, `error`). Empty = all types.
- **symbols**: array of symbol strings. Empty = all symbols. Matching is
  case-insensitive. Messages without a symbol field (status, error) always pass.

Default (no subscribe command): all events are sent (backward compatible).
The server acknowledges with:

```json
{"type": "subscribed", "data": {"events": 2, "symbols": 1}}
```

### REST API

The same port serves HTTP REST endpoints for programmatic access without
WebSocket. All responses are JSON with CORS headers.

| Method | Endpoint                       | Description                          |
|--------|--------------------------------|--------------------------------------|
| GET    | `/api/health`                  | Health check (`{"status":"ok"}`)     |
| POST   | `/api/backtest`                | Submit a backtest (config JSON body) |
| GET    | `/api/backtest`                | List all backtest runs               |
| GET    | `/api/backtest/<id>/status`    | Get status of a specific run         |
| GET    | `/api/backtest/<id>/results`   | Get results of a completed run       |

Example:

```bash
# Submit a backtest
curl -X POST http://localhost:8765/api/backtest \
  -H "Content-Type: application/json" \
  -d '{"strategy":"sma","path":"market_data.csv"}'

# Check status
curl http://localhost:8765/api/backtest/1/status

# Get results
curl http://localhost:8765/api/backtest/1/results
```

Custom port: `--ws-port 9000`.

---

## 21. Event Pipeline

The engine processes data through a sequential event-driven pipeline:

```
market_event / tick_event
    |
    v
IStrategy::on_market()  -->  order_event
    |
    v
RiskManager::check_order()  -->  rejection_event (if rejected/halted)
    |                              risk_unwind → close all positions
    v (pass)
OrderTracker::pending    -->  order status tracking
    |
    v
Orderbook::match()      -->  fill_event (with remaining_qty, fill_id)
    |                         OrderTracker::filled / partially_filled
    v
Portfolio::on_fill()     -->  position & PnL update
    |
    v
Workers (via ring buffers):
  - LoggingWorker        -->  event log (binary + text)
  - RiskWorker           -->  shadow portfolio risk checks
  - StatsWorker          -->  analytics accumulation
  - MarketMakerWorker    -->  liquidity replenishment
  - WsWorker             -->  WebSocket broadcast

cancel_order()           -->  cancel_event (via ring buffers to workers)
                              OrderTracker::cancelled

modify_order()           -->  amend_event (via ring buffers to workers)

l2_snapshot / l2_update  -->  orderbook::apply_l2_snapshot/update
  (from provider)             l2_snapshot_event / l2_update_event
                              (via ring buffers to workers)
```

### Event types

| Type            | Struct              | Description                           |
|-----------------|---------------------|---------------------------------------|
| `market`        | `market_event`      | OHLCV candle bar                      |
| `tick`          | `tick_event`        | Individual trade tick                  |
| `signal`        | `signal_event`      | Strategy signal (buy/sell/hold)        |
| `order`         | `order_event`       | Order submission to orderbook         |
| `fill`          | `fill_event`        | Trade execution confirmation          |
| `l2_snapshot`   | `l2_snapshot_event` | Full orderbook depth snapshot         |
| `l2_update`     | `l2_update_event`   | Incremental orderbook depth update    |
| `cancel`        | `cancel_event`      | Order cancellation notification       |
| `amend`         | `amend_event`       | Order modification (price/qty change) |
| `rejection`     | `rejection_event`   | Order rejected by risk manager (with reason) |

### Order status tracking

The engine tracks each order through its lifecycle via `OrderTracker`:

| Status             | Description                                              |
|--------------------|----------------------------------------------------------|
| `pending`          | Order created, awaiting processing (e.g., latency delay) |
| `open`             | Order accepted and placed on orderbook                   |
| `partially_filled` | Order partially matched, remaining quantity on book      |
| `filled`           | Order fully filled                                       |
| `cancelled`        | Order cancelled via `cancel_order()` API                 |
| `rejected`         | Order rejected by risk manager                           |

Access via `engine::get_order_tracker()` which exposes `get_order_status(id)`,
`get_open_orders()`, and `is_active(id)`.

### Order cancellation

Orders can be cancelled through `engine::cancel_order(symbol, order_id, reason)`.
This cancels the order on the execution adapter (removes from orderbook or sends
REST DELETE to exchange), emits a `cancel_event` through the ring buffers, and
updates the order tracker status to `cancelled`. Pending stop orders that haven't
triggered yet can also be cancelled.

### Order modification (amend)

Resting orders can be modified via `engine::modify_order(symbol, order_id,
new_price, new_qty)`. This cancels the existing order and re-inserts it at the
new price and quantity with the same order ID. The order loses time priority
(standard exchange amend behavior).

On success, an `amend_event` is emitted through the ring buffers and event log,
containing the order ID, symbol, new price, and new quantity. The amend event is
serialized in both JSON (WebSocket UI) and binary (event log) formats.

The modification is exposed through `IExecutionAdapter::modify_order()`:
- **LocalBookAdapter**: modifies the order on the local orderbook
- **HybridExecutor**: delegates to the local book adapter (limit orders)
- **BinanceExecutor**: not yet implemented (returns false)
- **ExchangeAdapter**: not yet implemented (returns false)

### L2 depth routing

Level-2 orderbook data from providers (e.g., Binance depth stream) is routed
directly to the matching engine's orderbook. Two paths are available:

**Provider sink** (`provider_sink.h`): The `event_sink_l2()` function accepts a
`provider::event` variant and an `orderbook` pointer. When the event is an
`l2_snapshot`, it converts provider levels to `Price`/`quantity` pairs and calls
`orderbook::apply_l2_snapshot()`. For `l2_update` events, it calls
`orderbook::apply_l2_update()`.

**Engine API**: `engine::apply_l2_snapshot(symbol, bids, asks)` and
`engine::apply_l2_update(symbol, side, price, new_qty)` route L2 data to the
correct per-symbol orderbook via the `OrderbookRegistry`, log the events, and
publish them through ring buffers to worker threads and the WebSocket UI.

L2 snapshots replace the entire orderbook state. L2 updates modify individual
price levels (quantity of 0 removes the level).

### Partial fills

Fill events include `remaining_qty` (quantity still on book after this fill) and
`fill_id` (unique identifier distinguishing multiple fills for one order). Use
`fill_event::is_partial()` to check if the order still has remaining quantity.
Portfolio and analytics handle partial fills incrementally.

### Order types

| Type         | Description                                          |
|--------------|------------------------------------------------------|
| `market`     | Execute immediately at best available price          |
| `limit`      | Execute at specified price or better                 |
| `stop`       | Becomes market order when stop price is reached      |
| `stop_limit` | Becomes limit order when stop price is reached       |

### Time-in-force

| TIF   | Description                                            |
|-------|--------------------------------------------------------|
| `ioc` | Immediate-or-Cancel — fill what's available, cancel rest |
| `fok` | Fill-or-Kill — fill entire quantity or cancel          |
| `gtc` | Good-Till-Cancel — remains until filled or cancelled   |
| `day` | Expires at end of trading session                      |

---

## 22. SQLite Persistence

SQLite is enabled by default (`-DENABLE_SQLITE=ON`). It persists equity curve
data and trade history to a local database file. Both equity points and fill
records are batched into transactions (100 rows per transaction) for
significantly improved write throughput.

```bash
# Default database path
./build/truetest --provider local --path data.csv
# Creates truetest.db

# Custom path
./build/truetest --provider local --path data.csv --db results.db

# Disable persistence
./build/truetest --provider local --path data.csv --no-db
```

The database can be queried with any SQLite client:

```bash
sqlite3 truetest.db ".tables"
sqlite3 truetest.db "SELECT * FROM equity_curve ORDER BY timestamp DESC LIMIT 10;"
```

### Run metadata (`runs` table)

Every invocation that writes to the SQLite database also records a row in the
`runs` table. A row is inserted when the engine starts and updated when it
finishes with the final metrics. This enables run history and comparison across
backtests without parsing JSON exports.

Schema:

```
runs (
  run_id       TEXT PRIMARY KEY,    -- "run_<wall_ms>_<counter>"
  started_at   INTEGER NOT NULL,    -- wall-clock ms since epoch
  ended_at     INTEGER,             -- wall-clock ms since epoch (null if still running)
  config_json  TEXT NOT NULL,       -- compact {"mode","seed","initial_balance","threading","rolling_window"}
  status       TEXT NOT NULL,       -- "running" | "completed" | "failed"
  final_equity REAL,                -- end-of-run equity
  sharpe       REAL,
  max_drawdown REAL,
  trade_count  INTEGER
)
```

Query past runs:

```bash
sqlite3 truetest.db \
  "SELECT run_id, status, final_equity, sharpe, max_drawdown, trade_count
     FROM runs ORDER BY started_at DESC LIMIT 20;"
```

When the WebSocket UI is enabled, the same data is exposed via a REST endpoint:

```bash
# List the 100 most recent runs as JSON
curl http://localhost:8765/api/runs

# Limit the result set
curl http://localhost:8765/api/runs?limit=20
```

If SQLite persistence is disabled (`--no-db`), `GET /api/runs` returns HTTP
503 with a JSON error body explaining that run history is unavailable.

### Portfolio checkpoints (resume-after-crash)

Long-running live or shadow sessions can write periodic portfolio snapshots
to a binary checkpoint file. On the next invocation, `--resume <path>` restores
cash, positions, and trade count before the run starts.

```bash
# Write a checkpoint every 10,000 events (the default) to /tmp/session.ckpt
./build/truetest --provider local --path data.csv \
  --checkpoint /tmp/session.ckpt

# Smaller interval for debugging
./build/truetest --provider local --path data.csv \
  --checkpoint /tmp/session.ckpt --checkpoint-interval 500

# Resume from an existing checkpoint
./build/truetest --provider local --path data.csv \
  --resume /tmp/session.ckpt
```

Checkpoint flags:

- `--checkpoint <path>` — write periodic portfolio snapshots to this file. A
  final checkpoint is also written when the engine exits cleanly.
- `--checkpoint-interval <N>` — write a checkpoint every N events (default:
  10000). Set to 0 to only write the final checkpoint at shutdown.
- `--resume <path>` — restore portfolio state (cash, positions, trade count)
  from the specified checkpoint file before running. Missing or malformed files
  are reported to stderr; the run continues with fresh state.

Checkpoints capture portfolio state only. Analytics accumulators, orderbook
state, and in-flight pending orders are not restored — they rehydrate from the
market data stream as usual. For fully deterministic replay, pair `--resume`
with `--seed <n>` and a recorded event log.

### Deterministic replay and RNG seeding

When `--seed <n>` is passed with a non-zero value, every stochastic component
in the engine is initialized from that seed:

- `LocalBookAdapter` — fill-probability RNG (seeded with `seed + 2`).
- `MarketMaker` — spread jitter RNG (seeded with `seed + 1`).
- `engine::run()` — uses a fixed epoch as the base simulation timestamp
  (`chrono::system_clock::time_point(0)`) instead of wall clock so that all
  market event timestamps are deterministic.

Re-running the same backtest with the same seed, the same config, and the same
input data produces byte-identical results. When `seed == 0`, components fall
back to wall-clock or hardware-derived seeds, which is the non-deterministic
default.

---

## 23. Analytics & Reporting

TrueTest computes a comprehensive set of analytics metrics during and after each
backtest run. All metrics are computed incrementally using Welford's online
algorithm (O(1) per update) and are available in real-time via WebSocket
snapshots.

### Cumulative metrics

The analytics report includes: cumulative return, Sharpe ratio, Sortino ratio,
max drawdown, Calmar ratio, win rate, profit factor, average win/loss, largest
winner/loser, time in market, average holding period, and average slippage.

### Rolling window metrics

Rolling Sharpe ratio and rolling max drawdown are computed over a configurable
trailing window of equity-to-equity returns. The window size defaults to 252 bars
(~1 year of daily data) and is set via `--rolling-window`.

```bash
./build/truetest --provider local --path data.csv --strategy sma --rolling-window 100
```

Rolling metrics appear in the printed report, WebSocket analytics broadcasts,
and JSON export.

### Risk-free rate adjustment

Sharpe and Sortino ratios subtract a per-period risk-free rate derived from the
annualized `--risk-free-rate` flag. When the flag is `0.0` (default), the
formulas reduce to the standard unadjusted versions — existing behavior is
preserved.

```bash
./build/truetest --provider local --path data.csv --strategy sma --risk-free-rate 0.05
```

### Benchmark comparison

A buy-and-hold benchmark is tracked alongside the strategy equity curve. The
benchmark invests 100% of initial capital at the first observed price and holds.
The report includes:

- **Alpha**: strategy excess return over the benchmark per bar
- **Beta**: sensitivity of strategy returns to benchmark returns
- **Information ratio**: risk-adjusted excess return vs benchmark
- **Tracking error**: standard deviation of return differences

### Per-symbol and per-strategy attribution

When multiple symbols are traded, the report breaks down PnL, trade count, win
rate, and profit factor per symbol. When the `strategy_name` field is set on
order events, per-strategy attribution is also available.

Attribution data appears in the printed report, JSON export
(`per_symbol` and `per_strategy` objects), and WebSocket analytics.

### Console report output

At the end of every run, TrueTest prints a formatted report to stdout. The
output is produced by `analytics/report_generator.{h,cpp}` and uses small
reusable ASCII widgets from `analytics/ascii_widgets.{h,cpp}` — horizontal
bars, sparklines, histograms, and aligned tables. All widgets are UTF-8 aware
and stdlib-only; no new dependencies.

The report is divided into sections, rendered in this order:

1. **Returns** — initial / final equity, total return, buy & hold, strategy
   vs. benchmark, with bars comparing strategy to buy-and-hold.
2. **Risk** — Sharpe, Sortino, max drawdown, Calmar, rolling Sharpe,
   rolling max drawdown.
3. **Equity Curve** — one-line sparkline (`▁▂▃▄▅▆▇█`) across the full run,
   with min / max / point count.
4. **Trades** — total trades, win rate (with bar), profit factor, avg win /
   loss, largest winner / loser.
5. **Per-Trade PnL Distribution** — equal-width histogram of per-trade PnL
   across configurable bins (default 8).
6. **Execution Quality** — avg slippage, total orders, total fills.
7. **Exposure** — time in market (with bar), avg holding period.
8. **Benchmark** — alpha, beta, information ratio, tracking error.
9. **Per-Symbol / Per-Strategy Attribution** — tables of PnL, trades, win%,
   profit factor per symbol/strategy.
10. **Worst Trades** — top N worst closing trades sorted by PnL ascending
    (default 5).

Example:

```
════════════════════════════════════════════════════════════════════════
  Analytics Report
════════════════════════════════════════════════════════════════════════

━━━ Returns ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  initial equity             10,000.00
  final equity               11,842.00
  total return                 +18.42%  ████████████████████████
  buy & hold                   +10.00%  █████████████░░░░░░░░░░░
  vs benchmark                  +8.42%

━━━ Equity Curve ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  ▁▂▃▃▄▅▆▇▇█
  min 10,000.00   max 11,842.00   points 10
```

#### Programmatic access

The renderer is reusable beyond the default end-of-run print. Public entry
points in `analytics/report_generator.h`:

```cpp
#include "analytics/report_generator.h"

AnalyticsReport r = analytics.generate_report();

// Full report, default options.
std::string text = tt::render_report(r);

// Customize sections and widths.
tt::report_options opts;
opts.width = 100;
opts.bar_width = 40;
opts.distribution_bins = 12;
opts.include_per_symbol = false;
std::string partial = tt::render_report(r, opts);

// Or render individual sections — useful for streaming partial output
// over the WebSocket bridge or composing a custom layout.
std::string risk  = tt::render_risk_section(r, opts);
std::string worst = tt::render_worst_trades_section(r, opts);
```

Every section function returns a `std::string`, so the same output can feed
the CLI, a `<pre>` panel in the web UI, a file, or a Slack paste without
modification.

#### Reusable widgets

`analytics/ascii_widgets.h` exposes primitives that are useful whenever a
component needs deterministic formatted text output:

| Widget                         | Purpose                                    |
|--------------------------------|--------------------------------------------|
| `hbar(value, max, width)`      | Horizontal bar with 1/8-block precision    |
| `sparkline(values, max_width)` | One-line trend chart (`▁▂▃▄▅▆▇█`)          |
| `equal_width_bins(values, n)`  | Bucket continuous values for histogram use |
| `horizontal_histogram(bins)`   | Labelled horizontal bar chart              |
| `section_header(title, width)` | `━━━ Title ━━━━━━━━━━` section divider     |
| `table(headers, rows, align)`  | Aligned table with per-column alignment    |
| `fmt_money` / `fmt_signed_pct` | `12,345.67` / `+18.42%` number formatting  |

### Structured result export

After the engine finishes, results can be exported to a file:

```bash
# JSON export (default)
./build/truetest --provider local --path data.csv --strategy sma --output results.json

# CSV export (equity curve + trades file)
./build/truetest --provider local --path data.csv --strategy sma --output results.csv --output-format csv
```

**JSON output** contains all scalar metrics, the full equity curve as
`[timestamp_ms, equity]` arrays, the complete trade log, and per-symbol /
per-strategy attribution objects.

**CSV output** writes the equity curve to the specified path and a companion
`_trades` file (e.g., `results_trades.csv`) with the trade log including symbol
and strategy columns.

### Configuration file support

All analytics flags are supported in the JSON configuration file:

```json
{
  "rolling_window": 100,
  "risk_free_rate": 0.05,
  "output": "results.json",
  "output_format": "json"
}
```

---

## 24. Examples

### Basic CSV backtest

```bash
./build/truetest \
  --provider local \
  --path market_data.csv \
  --strategy sma \
  --sma-period 50 \
  --balance 50000
```

### Deterministic backtest with fixed seed

```bash
./build/truetest \
  --provider local \
  --path market_data.csv \
  --strategy mean-reversion \
  --seed 12345 \
  --sma-period 20 \
  --risk-fraction 0.02 \
  --sl 0.005 \
  --tp 0.01
```

Running twice with the same `--seed` produces identical results.

### Binance paper trading with WebSocket UI

```bash
./build/truetest \
  --provider binance \
  --symbol btcusdt \
  --stream trade \
  --mode shadow \
  --strategy mean-reversion \
  --web-ui \
  --ws-port 8765 \
  --balance 10000
```

Open `web/index.html` in a browser to see the live dashboard.

### Binance kline stream with backfill

```bash
./build/truetest \
  --provider binance \
  --symbol ethusdt \
  --stream kline_1m \
  --mode shadow \
  --strategy sma \
  --sma-period 14 \
  --backfill 500 \
  --web-ui
```

Fetches 500 historical 1-minute candles before switching to live streaming.

### Event logging and replay

```bash
# Record events (compressed by default)
./build/truetest \
  --provider local \
  --path market_data.csv \
  --strategy sma \
  --log-events session.bin

# Record uncompressed
./build/truetest \
  --provider local \
  --path market_data.csv \
  --strategy sma \
  --log-events session.bin \
  --no-compress-log

# Replay all events
./build/truetest --replay session.bin

# Replay a specific time range (microseconds since epoch)
./build/truetest --replay session.bin \
  --replay-from 1700000000000000 \
  --replay-to   1700003600000000
```

### Tiered fees with custom risk parameters

```bash
./build/truetest \
  --provider local \
  --path market_data.csv \
  --strategy mean-reversion \
  --fee tiered \
  --maker-rate 0.001 \
  --taker-rate 0.002 \
  --risk-fraction 0.01 \
  --sl 0.003 \
  --tp 0.006
```

### Multithreaded run with explicit preset

```bash
./build/truetest \
  --provider local \
  --path market_data.csv \
  --strategy sma \
  --thread-preset full \
  --log-events events.bin
```

### Docker deployment

```bash
docker build -t truetest .
docker run -p 8765:8765 truetest
```

### Using a JSON config file

```bash
# Create a config file
cat > backtest.json << 'EOF'
{
  "provider": "local",
  "path": "market_data.csv",
  "strategy": "sma",
  "sma_period": 50,
  "balance": 50000,
  "fee": "tiered",
  "maker_rate": 0.001,
  "taker_rate": 0.002,
  "risk": {
    "max_drawdown": 0.20,
    "max_open_orders": 100
  }
}
EOF

# Run with config file
./build/truetest --config backtest.json

# Override specific values from CLI
./build/truetest --config backtest.json --balance 100000 --strategy mean-reversion
```

### Dump and validate config

```bash
# Dump resolved config as JSON (merge of file + CLI)
./build/truetest --config backtest.json --balance 100000 --dump-config

# Validate config without running the engine
./build/truetest --config backtest.json --dry-run
```

### Release build with packaging

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && cpack
# Produces truetest-0.1.0-Linux.tar.gz and truetest-0.1.0-Linux.deb
```

### Checkpoint + resume round-trip

```bash
# First run — snapshot portfolio every 1000 events
./build/truetest \
  --provider local \
  --path first_half.csv \
  --strategy sma \
  --sma-period 50 \
  --checkpoint /tmp/session.ckpt \
  --checkpoint-interval 1000

# Second run — resume from the saved snapshot against the next slice
./build/truetest \
  --provider local \
  --path second_half.csv \
  --strategy sma \
  --sma-period 50 \
  --resume /tmp/session.ckpt
```

Pair with `--seed <n>` and a recorded event log for fully deterministic
resume-after-crash behaviour.

### Multi-strategy run with analytics export

```bash
./build/truetest \
  --provider local \
  --path market_data.csv \
  --strategy mean-reversion,sma,ma-crossover \
  --param period=20 \
  --param fast_period=10 \
  --param slow_period=50 \
  --output results.json
```

`results.json` contains scalar metrics, the full equity curve, the per-trade
log, and per-strategy / per-symbol attribution — ready to ingest from Python
or a notebook.

### Production-style live run

```bash
./build/truetest \
  --provider binance \
  --symbol btcusdt \
  --stream kline_1m \
  --mode live \
  --live \
  --api-key "$BINANCE_API_KEY" \
  --api-secret "$BINANCE_API_SECRET" \
  --strategy sma \
  --sma-period 50 \
  --balance 1000 \
  --max-daily-loss 50 \
  --max-trades-per-hour 20 \
  --risk-unwind \
  --checkpoint /var/lib/truetest/live.ckpt \
  --log-events /var/log/truetest/events.bin \
  --output /var/log/truetest/results.json \
  --web-ui --ws-port 8765
```

Combines tight risk caps with automatic unwind, periodic checkpointing,
compressed event logging, and a JSON analytics export — the shape of a run
you would actually leave unattended.

---

## 25. Observability & Debugging

TrueTest ships with a three-part observability stack (todo Step L). None of
these features require optional CMake flags unless explicitly noted.

### 25.1 Structured operational logging (L1)

A zero-dependency, always-on logger lives in
`BacktestEngine/src/utils/log/logger.h`. It emits timestamped, level-tagged
lines to stderr by default:

```
[2026-04-14T20:35:00.337Z] [INFO] [main] truetest starting (pid=107715)
```

Redirect output to a file with `--log-file <path>`:

```bash
./build/truetest --provider local --path market_data.csv \
                 --strategy sma --log-file /var/log/truetest.log
```

Call sites use the `LOG_INFO`, `LOG_WARN`, and `LOG_ERROR` macros, each of
which takes a component name followed by a printf-style format string. This
is distinct from the trading-event log handled by `LoggingWorker`: the
operational log is for engine lifecycle, config, startup errors, and
diagnostics, while the trading event log captures market, order, and fill
events for replay.

### 25.2 Prometheus metrics endpoint (L2)

When the WebSocket UI is enabled (`-DENABLE_WEB_UI=ON` plus `--web-ui`), an
HTTP endpoint at `GET /metrics` exposes engine state in Prometheus text
exposition format. Scrape it the usual way:

```bash
curl http://127.0.0.1:8765/metrics
```

Example output:

```
# HELP truetest_events_processed_total Events processed by analytics
# TYPE truetest_events_processed_total counter
truetest_events_processed_total 12853.000000
# HELP truetest_orders_submitted_total Total orders submitted
# TYPE truetest_orders_submitted_total counter
truetest_orders_submitted_total 42.000000
# HELP truetest_fills_total Total fills executed
# TYPE truetest_fills_total counter
truetest_fills_total 40.000000
# HELP truetest_equity_current Current portfolio equity (valued at last_mid_price)
# TYPE truetest_equity_current gauge
truetest_equity_current 10325.120000
# HELP truetest_cash_current Current cash balance
# TYPE truetest_cash_current gauge
truetest_cash_current 9820.440000
# HELP truetest_drawdown_current Current drawdown fraction
# TYPE truetest_drawdown_current gauge
truetest_drawdown_current 0.018000
# HELP truetest_sharpe_ratio Sharpe ratio (point-in-time)
# TYPE truetest_sharpe_ratio gauge
truetest_sharpe_ratio 1.230000
# HELP truetest_halt_flag 1 if halt flag set, 0 otherwise
# TYPE truetest_halt_flag gauge
truetest_halt_flag 0.000000
```

The endpoint is registered by the engine via
`WebSocketWorker::set_on_metrics()` and returns HTTP 503 until the engine is
running. When the web UI is disabled, the route is not served at all.

### 25.3 Event log rotation (L3)

The binary event log (`--log-events <path>`) and the structured text log
(from `LoggingWorker`) can be capped and rotated once they exceed a
configurable size threshold. Rotation is off by default.

Enable it by passing a non-zero max size in megabytes:

```bash
./build/truetest --provider local --path market_data.csv --strategy sma \
                 --log-events /var/log/truetest/events.bin \
                 --log-max-size 100 \
                 --log-keep 5
```

With those flags:

- As soon as a completed event would push `events.bin` past 100 MB, the
  current file is finalized (index footer written), renamed to
  `events.bin.1`, and a new `events.bin` is opened. Older rotated copies
  shift: `events.bin.1 → events.bin.2`, etc.
- At most `--log-keep` rotated copies are retained (default 5). When the
  limit is exceeded, the oldest file (`events.bin.5` in this example) is
  deleted.
- Rotation happens at event boundaries only — never mid-write — so replay
  tools can safely read any single rotated file.
- The same size limit and retention count are applied to the text log
  produced by `LoggingWorker` when `--log-text` (file sink) is used.

Rotation is opt-in: leaving `--log-max-size 0` (the default) preserves the
old unbounded behavior, which is fine for short-lived backtests but not for
long-running live or shadow sessions.

---

## 26. Error Handling & Resilience

### 26.1 Graceful worker recovery (N1)

Worker threads no longer halt the engine on the first exception. When a
worker's `on_event()` handler throws, the error is caught, logged via the
structured logger, and the consecutive error counter is incremented. The
worker continues processing the next event. Only when
`max_consecutive_worker_errors` consecutive failures occur without a
successful event in between does the worker set the halt flag and stop.

The default is 5 consecutive errors. Configure via `engine_config`:

```cpp
engine_config cfg;
cfg.max_consecutive_worker_errors = 10;  // more tolerant
```

A successful `on_event()` call resets the consecutive error counter to zero.
Total error counts are available via `Worker::error_count()`. Each worker
subclass provides a human-readable name through `worker_name()` which
appears in log messages (e.g., `[WARN] [logging] on_event exception ...`).

### 26.2 WebSocket input sanitization (N2)

All incoming WebSocket commands are sanitized at the system boundary before
any processing occurs:

- **Message length**: messages exceeding 4 KB are rejected with an error
  response and logged.
- **Null bytes**: messages containing `\0` are rejected.
- **Control characters**: string fields (command, side, type, timeframe,
  value) are checked for ASCII control characters (< 0x20, except tab).
  Messages with control characters in string fields are rejected.
- **Numeric validation**: numeric fields (quantity, price) are validated to
  contain actual numeric data. Non-numeric values are rejected with a
  descriptive error rather than silently defaulting to 0.

All rejections produce a structured error response to the client:

```json
{"type": "error", "data": {"message": "message too large (max 4096 bytes)", "source": "ws_validator"}}
```

Rejections are also logged via the structured logger under the `ws`
component.

### 26.3 Unified connection retry (N3)

A shared retry-with-exponential-backoff utility lives in
`BacktestEngine/src/utils/retry.h`. All external connection points use this
utility instead of implementing their own retry logic:

```cpp
#include "utils/retry.h"

// Simple form
bool ok = retry_with_backoff(
    []() { return try_connect(); },  // returns true on success
    5,                                // max_attempts
    std::chrono::milliseconds(1000),  // initial_delay
    std::chrono::milliseconds(30000)  // max_delay
);

// With logging callback
retry_config cfg;
cfg.max_attempts = 5;
cfg.initial_delay = std::chrono::milliseconds(1000);
cfg.max_delay = std::chrono::milliseconds(16000);
cfg.on_retry = [](unsigned attempt, std::exception_ptr ex) {
    std::cerr << "Retry attempt " << attempt << "\n";
};
bool ok = retry_with_backoff([]() { return try_connect(); }, cfg);
```

Components using the shared utility:

| Component                | File                              | Behavior                          |
|--------------------------|-----------------------------------|-----------------------------------|
| Binance WebSocket        | `binance_transport.h`             | 5 attempts, 1s → 16s backoff     |
| Binance combined stream  | `binance_combined_transport.h`    | 5 attempts, 1s → 16s backoff     |
| Generic WebSocket source | `websocket_data_source.cpp`       | 10 attempts, configurable delays  |
| PostgreSQL               | `pg_data_source.cpp`              | 5 attempts, 1s → 16s backoff     |

The callable should return `true` on success, `false` on failure. If the
callable throws, the exception counts as a failure and is forwarded to the
`on_retry` callback. After all attempts are exhausted, the last exception
is rethrown.

### 26.4 Binance reliability hardening (Tier 1)

Five independent hardening passes landed on `providers/binance/` to let a
live session survive long-running operation without silent stalls. Every
item is on by default; there is no CLI flag to disable them.

**Clock-skew guard.** `BinanceProvider::open()` now calls
`binance::verify_clock_skew()` before constructing the `ExecutionBridge`
on the live branch. It hits `/api/v3/time` unsigned and compares the
server clock to the local clock. If the drift exceeds 2000 ms — or the
fetch fails — `open()` logs the reason, sets the provider state to
`lifecycle::error`, and refuses to go live. Paper and hybrid modes are
unaffected. This prevents the classic `-1021 "Timestamp for this request
is outside of the recvWindow"` rejection from silently breaking every
signed order.

Header: `providers/binance/binance_time_sync.h`. The pure decision helper
`verify_clock_skew_offset(offset_ms, tolerance_ms)` is unit-tested in
`tests/test_binance_rest_client_time.cpp`.

**Proactive rate-limit backoff.** `BinanceRestClient` tracks
`X-MBX-USED-WEIGHT-1M` across requests. Before each `execute()`, a pure
function `throttle_delay_ms(used, anchor, now, cap, pct)` returns a sleep
duration if the 1-minute window is still active and weight use is at or
above 80% of the 6000 cap. On `HTTP 429`, the client sleeps (honoring
`Retry-After`, clamped to 60 s) and retries once. The soft threshold and
cap are compile-time defaults but can be overridden via
`set_weight_cap()` / `set_soft_threshold_pct()` (the settings surface is
intentionally not wired through `engine_config` yet).

Tests: `tests/test_binance_rest_client_rate_limit.cpp`.

**listenKey keepalive hardening.** `BinanceUserDataTransport`'s
keepalive loop now takes a `binance_keepalive_policy` (30 min interval,
15 s retry delay, 3 retries by default). The per-tick decision lives in
`binance_keepalive_detail::keepalive_tick`, a pure templated helper:

- PUT `/api/v3/userDataStream?listenKey=…` succeeds → stay `open`.
- All PUT retries fail → POST `/api/v3/userDataStream` to rotate the
  listenKey. A rotated key is swapped in under a mutex. State goes to
  `degraded`.
- Rotation also fails → state goes to `error`, reader thread is told to
  stop. No silent loss of the user-data stream.

Cancellable at every sleep via the existing `cv_` / `stop_flag_` pair.

Tests: `tests/test_binance_user_data_transport_keepalive.cpp` drive the
pure tick function with injected callables — no network required.

**User-data WebSocket reconnect.** Previously, any transient WS read
error put the user-data stream into `lifecycle::error` forever. The
transport now runs the socket loop inside a retry envelope:

- `run_once()` returns `stopped`, `network_error`, or `handshake_error`.
- `run()` reconnects up to 10 attempts with exponential backoff
  (1 s → 30 s), cancellable via the existing stop signal.
- If the prior session was open for >5 minutes, the reconnect counter
  resets — a long-lived stream that hiccups isn't treated as flapping.
- Before reconnecting, if the previous session lasted longer than the
  keepalive interval, the transport best-effort refreshes the listenKey
  (PUT, fallback to POST) so a reconnect against an expired key doesn't
  silently fail.
- Status notes include the current attempt (`"reconnecting user-data
  stream (attempt 3/10)"`), which `ExecutionBridge::poll_status` forwards
  to the engine.

The loop-control decision is isolated in
`BinanceUserDataTransport::decide_next(state, last_result, max_attempts,
now_ms, reset_threshold_ms)` and exercised in
`tests/test_binance_user_data_transport_reconnect.cpp`. Socket-level
behavior is intentionally not unit-tested here; rely on the
record-and-replay transport for end-to-end coverage.

**ExecutionBridge concurrency audit.** `ExecutionBridge` was reviewed
for thread safety. Every non-const, non-injected data member is now
explicitly documented as guarded by a specific mutex:

| Member                                          | Mutex       |
|-------------------------------------------------|-------------|
| `by_engine_id_`, `by_client_id_`                | `map_mu_`   |
| `pending_fills_`, `next_fill_id_`               | `fills_mu_` |
| `pending_status_`                               | `status_mu_`|
| `last_error_`                                   | `error_mu_` |

No method holds two of these locks at once. A regression test
`ExecutionBridge.ConcurrentFillIngestAndPoll` drives `handle_message`
from 4 feeder threads (1000 messages total) against a concurrent
`poll_fills` loop and asserts zero loss and zero duplicate fill IDs. The
test is suitable for running under `-DENABLE_TSAN=ON`.

---

## 27. Testing

TrueTest ships with three layers of automated tests, all built by a single
CMake flag:

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target truetest_tests
./build/truetest_tests
```

### 27.1 Unit tests

Per-component correctness tests: orderbook matching, ring buffer semantics,
indicators (SMA/EMA/RSI/Bollinger), portfolio accounting, strategies, object
pool, risk manager, etc. These live in `tests/test_*.cpp` and are the
majority of the suite.

Filter by suite or test name:

```bash
./build/truetest_tests --gtest_filter='Orderbook.*'
./build/truetest_tests --gtest_filter='*Sharpe*'
```

### 27.2 Integration tests — `tests/test_engine_integration.cpp`

End-to-end tests that exercise the full event pipeline
(`market_event → strategy → order → orderbook → fill → portfolio`) by
calling `engine::run()` on synthetic data and inspecting the resulting
`portfolio` / `Analytics` state. Unlike unit tests, these do not mock the
event loop; they construct a real `engine_config` programmatically.

Covered scenarios:

| Test                                          | What it verifies                                            |
|-----------------------------------------------|-------------------------------------------------------------|
| `SmaPipelineIsDeterministicWithFixedSeed`     | Two runs with the same seed produce bit-identical reports    |
| `OrderProducesFillAndUpdatesAnalytics`        | Strategy-emitted orders reach the orderbook and produce fills, portfolio/analytics update |
| `RiskDrawdownLimitHaltsEngine`                | `risk_limits.max_drawdown` breach halts the run before data is exhausted |

All three rely on `engine_config.seed` (K2) for determinism and
`thread_preset::inline_mode` to avoid cross-thread non-determinism.

### 27.3 Golden-file regression — `tests/test_golden_regression.cpp`

Runs a deterministic backtest against a checked-in fixture CSV and diffs
the resulting metrics against a committed golden JSON file. Any code change
that alters backtest outputs will fail this test, surfacing unintended
behavior drift.

Fixture layout under `tests/golden/`:

| File                         | Purpose                                           |
|------------------------------|---------------------------------------------------|
| `sma_basic.csv`              | 30 OHLCV bars for symbol `GOLD`, oscillating ~$100–106 |
| `sma_basic_config.json`      | Human-readable record of the engine configuration  |
| `sma_basic_expected.json`    | Golden metrics — compared against on every test run |

Metrics compared (via `EXPECT_NEAR` with `1e-6` tolerance for floats,
`EXPECT_EQ` for counts): `final_equity`, `cumulative_return`,
`max_drawdown`, `sharpe_ratio`, `sortino_ratio`, `win_rate`,
`profit_factor`, `buy_and_hold_return`, `total_trades`, `total_orders`,
`total_fills`.

**Regenerating after an intentional behavior change:**

```bash
TRUETEST_REGENERATE_GOLDEN=1 ./build/truetest_tests \
    --gtest_filter='GoldenRegression.*'
git diff tests/golden/sma_basic_expected.json   # review the delta
git add tests/golden/sma_basic_expected.json
```

Before each assertion run, the test also runs the same pipeline twice and
asserts byte-identical outputs — a prerequisite for any meaningful golden
comparison. If that pre-check fails, non-determinism has crept in and the
golden comparison is disabled until fixed.

---

## 28. Performance Benchmarks

Google Benchmark suite for the hot-path components. Opt-in via
`-DENABLE_BENCHMARKS=ON`. Builds a separate binary `truetest_benchmarks`
that does not ship in release packaging.

```bash
cmake -B build -DENABLE_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target truetest_benchmarks
./build/truetest_benchmarks
./build/truetest_benchmarks --benchmark_filter=Orderbook
./build/truetest_benchmarks --benchmark_format=json > bench.json
```

Benchmarks (`benchmarks/bench_main.cpp`):

| Benchmark                          | What it measures                                           |
|------------------------------------|------------------------------------------------------------|
| `BM_Orderbook_InsertCancel`        | Non-crossing insert + cancel round trip (flat-array price levels + node pool) |
| `BM_Orderbook_Match`               | Aggressive crossing order against a pre-seeded ask ladder  |
| `BM_RingBuffer_PushPop`            | SPSC `RingBuffer` single-thread push+pop cycle             |
| `BM_EventJson_Market`              | `market_event` → JSON (`snprintf` hot path)                |
| `BM_EventJson_Fill`                | `fill_event` → JSON                                        |
| `BM_SMA_Update`                    | `simple_moving_average::update()` per tick                  |
| `BM_Engine_Throughput_100k`        | End-to-end engine run over 100 000 synthetic bars (bars/sec) |

The full-engine benchmark uses `thread_preset::inline_mode` so the number
reflects single-threaded hot-path throughput; enable a worker preset in
`engine_config` to measure multithreaded scaling.

Always build with `-DCMAKE_BUILD_TYPE=Release` for meaningful numbers —
the default Debug profile is ~100× slower. If Google Benchmark warns about
CPU scaling, pin frequency with `cpupower frequency-set -g performance`
before capturing baselines.

---

## 29. Embedding: C API and Python Bindings

TrueTest ships a stable `extern "C"` API and a ctypes-based Python wrapper so
the engine can be driven from host languages without linking the full C++
interface. The native surface lives in
`BacktestEngine/src/api/truetest_api.h`; the Python module is
`python/truetest.py`.

### 29.1 Building `libtruetest`

```bash
cmake -B build -DBUILD_SHARED_LIB=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target truetest_shared
# produces build/libtruetest.so (linux), build/libtruetest.dylib (macOS),
# or build/truetest.dll (windows).
```

`BUILD_SHARED_LIB` is independent of the CLI target — the two can be built
together or separately. The shared library uses the same production sources
as `truetest`, plus `BacktestEngine/src/api/truetest_api.cpp`. Symbol
visibility defaults to hidden; only `tt_*` functions are exported.

Installing via `cmake --install build` drops the library into `lib/` and the
public header into `include/truetest/truetest_api.h`.

### 29.2 C API reference

All functions use `extern "C"` linkage and opaque handles. Errors are signaled
via `NULL` return values or non-zero status codes; the last message is
available through `tt_last_error()` (thread-local).

| Function                              | Returns                                               | Notes                               |
|---------------------------------------|-------------------------------------------------------|-------------------------------------|
| `tt_version()`                        | `const char*` — library version (`"0.1.0"`)           | Always non-null                     |
| `tt_create_engine(config_json)`       | `tt_engine_handle` or `NULL` on error                 | `config_json` is a UTF-8 JSON string |
| `tt_run(handle)`                      | `0` on success, non-zero on error                     | Synchronous; blocks until done      |
| `tt_get_results(handle)`              | Heap `const char*` JSON or `NULL`                     | Must be freed with `tt_free_string` |
| `tt_free_string(str)`                 | void                                                  | `NULL`-safe                          |
| `tt_destroy(handle)`                  | void                                                  | `NULL`-safe                          |
| `tt_last_error()`                     | Thread-local `const char*` (possibly empty)           | Valid until the next API call       |

**Config JSON schema** (fields optional unless noted):

| Field               | Type     | Default           | Purpose                                                    |
|---------------------|----------|-------------------|------------------------------------------------------------|
| `data_path`         | string   | — (**required**)  | Path to OHLCV CSV (consumed by `CsvDataSource`)            |
| `strategy`          | string   | `"mean-reversion"`| Registered strategy name (`sma`, `mean-reversion`, `ma-crossover`) |
| `initial_balance`   | number   | `10000.0`         | Starting cash                                              |
| `seed`              | uint     | `0`               | RNG seed for deterministic replays                         |
| `rolling_window`    | uint     | `252`             | Rolling Sharpe/drawdown window                             |
| `risk_free_rate`    | number   | `0.0`             | Annualized risk-free rate                                  |
| `market_aggression` | number   | `1.1`             | Market order price multiplier                              |
| `qty_scale`         | number   | `1e8`             | Fractional-quantity scale factor                           |
| `fill_rng_seed`     | uint     | `42`              | Fill model RNG seed                                        |
| `spread_step_factor`| number   | `0.0001`          | Spread step (as a fraction of mid)                         |
| `db_path`           | string   | `""`              | Optional SQLite persistence path                           |
| `event_log_path`    | string   | `""`              | Optional binary event log path                             |
| `params`            | object   | `{}`              | `{key: number}` pairs forwarded to `strategy.set_param()`  |

**Results JSON** — the object returned from `tt_get_results()` contains the
same headline metrics as `Analytics::export_json` plus `equity_curve`,
`per_symbol`, and `per_strategy` breakdowns.

### 29.3 Minimal C usage

```c
#include <stdio.h>
#include <stdlib.h>
#include "truetest/truetest_api.h"

int main(void) {
    const char* cfg =
        "{\"data_path\":\"market_data.csv\","
        "\"strategy\":\"sma\","
        "\"params\":{\"period\":10}}";

    tt_engine_handle h = tt_create_engine(cfg);
    if (!h) { fprintf(stderr, "%s\n", tt_last_error()); return 1; }

    if (tt_run(h) != 0) { fprintf(stderr, "%s\n", tt_last_error()); return 2; }

    const char* json = tt_get_results(h);
    if (json) { printf("%s\n", json); tt_free_string(json); }

    tt_destroy(h);
    return 0;
}
```

Link with `-ltruetest` after installing, or `-L build -ltruetest` against a
dev build.

### 29.4 Python bindings

The Python module is a zero-dependency ctypes wrapper — no numpy/pandas, so
it installs cleanly on minimal targets. Install in editable mode from a
development checkout:

```bash
cmake -B build -DBUILD_SHARED_LIB=ON
cmake --build build --target truetest_shared
pip install -e python/
```

Library-discovery order in `python/truetest.py`:

1. The `TRUETEST_LIB` environment variable (absolute path override).
2. Alongside the module (for wheels that bundle the `.so`).
3. `<repo>/build/libtruetest.so` (development checkout default).
4. The system loader's default search path.

**Example:**

```python
from truetest import Engine, version

print(version())  # "0.1.0"

results = Engine({
    "data_path": "market_data.csv",
    "strategy":  "sma",
    "seed":      1,
    "params":    {"period": 10},
}).run()

print(results["sharpe_ratio"], results["final_equity"])
```

The `Engine` class supports the context-manager protocol, so
`with Engine(cfg) as eng: eng.run()` guarantees the native handle is
released even on exceptions. `Engine.close()` is idempotent.

`TrueTestError` wraps any non-zero C API status, including malformed config,
missing CSV files, unknown strategies, and runtime exceptions inside
`engine::run()`. The exception message includes `tt_last_error()` for
diagnostics.

A runnable example lives at `python/example.py`.

### 29.5 Limitations

The C API intentionally covers the batch-backtest use case only. Live
streaming, WebSocket UI, provider-based execution, replay, and multi-symbol
runs are driven through the CLI binary — callers that need those features
today should shell out to `truetest` rather than the shared library. The
API surface is deliberately small so it can stay stable while the internal
engine evolves.

