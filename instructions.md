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

### Logging flags

| Flag                 | Type     | Default  | Description                                      |
|----------------------|----------|----------|--------------------------------------------------|
| `--log-events`       | string   | (none)   | Path to write binary event log                   |
| `--compress-log`     | flag     | on       | Compress binary event logs with zstd             |
| `--no-compress-log`  | flag     | (off)    | Disable zstd compression for event logs          |

### WebSocket UI flags

| Flag                 | Type     | Default  | Description                                      |
|----------------------|----------|----------|--------------------------------------------------|
| `--web-ui`           | flag     | (off)    | Enable WebSocket UI server                       |
| `--ws-port`          | uint16   | `8765`   | WebSocket server listen port                     |

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

Live mode (`--mode live --live`) is stubbed — REST API order submission is not
yet implemented. Shadow mode (paper trading) works fully.

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

### SMA Strategy (`sma`)

Generates signals based on price crossing above or below a simple moving average.
Buys when price > SMA, sells when price < SMA.

Parameters:
- `period` (default: 20) — SMA lookback period

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

### PostgreSQL

Available when built with `-DENABLE_POSTGRESQL=ON`. Connects to a PostgreSQL
database and loads OHLCV data via SQL queries. Connection parameters are prompted
in TUI mode.

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
returns one of three actions:

| Action   | Effect                                    |
|----------|-------------------------------------------|
| `pass`   | Order/fill accepted, processing continues |
| `reject` | Order rejected, engine continues running  |
| `halt`   | Engine stopped immediately                |

### Default risk limits

| Limit                  | Default      | Triggers         |
|------------------------|-------------|------------------|
| Max position value     | $1,000,000,000 | Order rejection |
| Max drawdown           | 30%         | Engine halt       |
| Max loss per trade     | $10,000     | —                 |
| Max open orders        | 1,000       | Order rejection   |
| Max portfolio exposure | $5,000,000,000 | —              |

The defaults are deliberately permissive. For production or live trading, you
should set appropriate limits via the config.

Risk checks currently run asynchronously in the risk worker thread on a shadow
copy of the portfolio.

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

On Linux, worker threads are pinned to specific CPU cores via
`sched_setaffinity` to reduce context switching and cache thrashing. Use
`--no-pin` to disable pinning (useful in containers or VMs with restricted
CPU access).

### Ring buffers

Each worker has a ring buffer with 65,536 slots (configurable via
`engine_config::ring_buffer_capacity`). The ring buffer uses atomic
load/store operations — no mutexes or syscalls on the hot path.

---

## 20. WebSocket UI

When `--web-ui` is passed, TrueTest starts a Boost.Beast WebSocket server that
broadcasts all engine events as JSON to connected browser clients.

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

### Client commands

The WebSocket connection is bidirectional. Clients can send JSON commands:

```json
{"cmd": "start"}
{"cmd": "pause"}
{"cmd": "stop"}
{"cmd": "order", "side": "buy", "quantity": 0.1, "price": 50000, "order_type": "limit"}
{"cmd": "set_timeframe", "timeframe": "1h"}
{"cmd": "set_symbol", "value": "ETHUSDT"}
{"cmd": "set_strategy", "value": "sma"}
```

| Command         | Fields                              | Description                 |
|-----------------|-------------------------------------|-----------------------------|
| `start`         | —                                   | Resume engine execution     |
| `pause`         | —                                   | Pause engine                |
| `stop`          | —                                   | Halt engine                 |
| `order`         | `side`, `quantity`, `price`, `order_type` | Submit a manual order  |
| `set_timeframe` | `timeframe`                         | Change kline interval       |
| `set_symbol`    | `value`                             | Switch trading symbol       |
| `set_strategy`  | `value`                             | Switch active strategy      |

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
data and trade history to a local database file.

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
