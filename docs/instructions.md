# TrueTest — Build & Runtime Instructions

> **Status:** Authoritative operator manual. Covers prerequisites, CMake flags,
> build outputs, CLI usage, provider modes, runtime behaviour, observability,
> and embedding for the three engine binaries (`engine_backtest`,
> `engine_shadow`, `engine_live`). For the long-term target architecture, see
> [`target-architecture.md`](target-architecture.md). For the authoritative
> build/architecture spec (project conventions), see
> [`../CLAUDE.md`](../CLAUDE.md).

TrueTest is a modular C++23 engine that starts as a backtesting platform but is
designed to be reused across deployments: pure backtesting, Binance spot
execution, Polymarket AMM, MetaTrader EA, or anything that processes market
data through a strategy and orderbook pipeline.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Building from Source](#2-building-from-source)
3. [CMake Flags Reference](#3-cmake-flags-reference)
4. [Build Types and Optimization](#4-build-types-and-optimization)
5. [Sanitizers](#5-sanitizers)
6. [Build Outputs](#6-build-outputs)
7. [FetchContent Dependencies](#7-fetchcontent-dependencies)
8. [CMake Presets](#8-cmake-presets)
9. [Build Audit Header](#9-build-audit-header)
10. [Install and Packaging](#10-install-and-packaging)
11. [Running TrueTest](#11-running-truetest)
12. [CLI Flags Reference](#12-cli-flags-reference)
13. [JSON Configuration File](#13-json-configuration-file)
14. [Dry Run Mode](#14-dry-run-mode)
15. [Interactive TUI Mode](#15-interactive-tui-mode)
16. [Provider Mode](#16-provider-mode)
17. [Replay Mode](#17-replay-mode)
18. [Strategies](#18-strategies)
19. [Data Sources](#19-data-sources)
20. [Fee Models](#20-fee-models)
21. [Risk Management](#21-risk-management)
22. [Threading Model](#22-threading-model)
23. [WebSocket UI](#23-websocket-ui)
24. [Event Pipeline](#24-event-pipeline)
25. [SQLite Persistence](#25-sqlite-persistence)
26. [Analytics & Reporting](#26-analytics--reporting)
27. [Observability & Debugging](#27-observability--debugging)
28. [Error Handling & Resilience](#28-error-handling--resilience)
29. [start.sh Launcher](#29-startsh-launcher)
30. [Testing](#30-testing)
31. [Performance Benchmarks](#31-performance-benchmarks)
32. [Embedding: C API and Python Bindings](#32-embedding-c-api-and-python-bindings)
33. [CI Pipeline](#33-ci-pipeline)
34. [Examples](#34-examples)

---

## 1. Prerequisites

**Required (zero-dependency core):**

| Requirement | Minimum | Notes |
|---|---|---|
| CMake | 3.22 | |
| C++ compiler | C++23 support | GCC 13+, Clang 17+, MSVC 2022+. `CMAKE_CXX_STANDARD` is locked to 23; values below 23 are a fatal error. Extensions are OFF. |
| Git | any | FetchContent clones dependencies at configure time |
| libsqlite3-dev | any | Default ON; skip with `-DENABLE_SQLITE=OFF` |

**Optional system packages** (only needed when the corresponding `ENABLE_*`
flag is ON):

| Package | Install (Debian/Ubuntu) | Required by |
|---|---|---|
| `libboost-dev`, `libboost-system-dev` | `sudo apt install libboost-all-dev` | `ENABLE_WEB_UI`, `ENABLE_BINANCE`, `ENABLE_LIVE_DATA` |
| `libssl-dev` (OpenSSL) | `sudo apt install libssl-dev` | `ENABLE_BINANCE` |
| `libpq-dev` / `postgresql-server-dev-all` | `sudo apt install libpq-dev` | `ENABLE_POSTGRESQL` |
| Abseil | auto-fetched via CMake | `ENABLE_DEBUG` |
| GoogleTest | auto-fetched via CMake | `BUILD_TESTS` |
| Google Benchmark | auto-fetched via CMake | `ENABLE_BENCHMARKS` |

---

## 2. Building from Source

### Minimal build (no external dependencies)

```bash
cmake -B build
cmake --build build
```

Produces the three engine binaries (`engine_backtest`, `engine_shadow`,
`engine_live`) with CSV data sources, the core event pipeline, and SQLite
persistence. SQLite is enabled by default and requires `libsqlite3-dev`.

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
cmake --build build -j$(nproc)
```

### Running tests

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite contains ~310 cases across `truetest_tests` (GoogleTest v1.15.2)
and `truetest_cli_tests` (CLI integration). A few `EngineStreaming` tick tests
are known to crash at teardown — a tracked issue.

---

## 3. CMake Flags Reference

Each optional feature is gated behind an `ENABLE_*` CMake option. When
enabled, the corresponding `HAS_*` preprocessor define is set and additional
source files are compiled in. Optional dependencies are wired into the
`engine_core` OBJECT library exactly once via `tt_wire_optional_backends()` in
`cmake/Dependencies.cmake`, with PUBLIC scope so every linked binary sees the
`HAS_*` defines.

### Feature flags

| Flag | Default | `HAS_*` define | System deps | Description |
|---|---|---|---|---|
| `ENABLE_SQLITE` | **ON** | `HAS_SQLITE` | libsqlite3 | SQLite persistence for trades, portfolio snapshots, and equity curves |
| `ENABLE_POSTGRESQL` | OFF | `HAS_POSTGRESQL` | libpq + libpqxx (auto-fetched) | PostgreSQL/TimescaleDB storage backend. Auto-fetches vcpkg if `pg_config` is not on PATH. FetchContent pins libpqxx 7.9.2 |
| `ENABLE_WEB_UI` | OFF | `HAS_WEB_UI` | Boost headers | Boost.Beast WebSocket + HTTP server that broadcasts engine events as JSON to the React frontend |
| `ENABLE_BINANCE` | OFF | `HAS_BINANCE` | Boost headers, OpenSSL | Binance spot exchange provider: live WebSocket streaming, REST execution, HMAC-SHA256 signing, historical kline backfill |
| `ENABLE_LIVE_DATA` | OFF | `HAS_LIVE_DATA` | Boost.System | Generic WebSocket data feed via `websocket_data_source` |
| `ENABLE_DEBUG` | OFF | `HAS_DEBUG` | Abseil 20240722.0 (auto-fetched) | Performance instrumentation: stage timers, memory info, hardware info, copy tracker, debug report |

### Sanitizers

| Flag | Default | Description |
|---|---|---|
| `ENABLE_TSAN` | OFF | ThreadSanitizer. Adds `-fsanitize=thread` to compile and link |
| `ENABLE_ASAN` | OFF | AddressSanitizer. Adds `-fsanitize=address` |
| `ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer. Adds `-fsanitize=undefined` |

TSAN is mutually exclusive with ASAN and UBSAN. CMake emits `FATAL_ERROR` if
you combine them. ASAN and UBSAN can be combined.

### Build targets

| Flag | Default | Description |
|---|---|---|
| `BUILD_TESTS` | OFF | Builds `truetest_tests` (GoogleTest v1.15.2, ~310 cases) and `truetest_cli_tests` (CLI integration). Enables `ctest` |
| `BUILD_SHARED_LIB` | OFF | Builds `libtruetest.so` (or `.dylib`/`.dll`) with the C API from `src/api/truetest_api.h`. Sets `-fvisibility=hidden`, `POSITION_INDEPENDENT_CODE ON`, SOVERSION 0 |
| `ENABLE_BENCHMARKS` | OFF | Builds `truetest_benchmarks` (Google Benchmark v1.8.5) |

### Optimization

| Flag | Default | Description |
|---|---|---|
| `ENABLE_NATIVE_OPT` | OFF | Applies `-march=native -mtune=native -funroll-loops -fomit-frame-pointer` to `engine_live` only, Release config only. Backtest and shadow binaries remain portable |

### Standard CMake variables

| Variable | Effect |
|---|---|
| `CMAKE_BUILD_TYPE=Debug` | `-O0 -g`. GCC also sets `_GLIBCXX_DEBUG` (checked iterators) |
| `CMAKE_BUILD_TYPE=Release` | `-O3 -DNDEBUG -flto` (compile + link). Enables cross-translation-unit inlining and dead-code elimination |
| `CMAKE_BUILD_TYPE=RelWithDebInfo` | `-O2 -g`. Useful for profiling with symbols |
| `CMAKE_CXX_STANDARD` | Locked to 23. Lower values are a fatal error. Extensions OFF |

---

## 4. Build Types and Optimization

Set the build type with `-DCMAKE_BUILD_TYPE=<type>`:

| Build Type | Optimization | Debug info | LTO | Assertions | Use case |
|---|---|---|---|---|---|
| `Release` | `-O3` | No | Yes (`-flto`) | Disabled (`-DNDEBUG`) | Production, benchmarking |
| `RelWithDebInfo` | `-O2` | `-g` | No | Enabled | Profiling with debug symbols |
| `Debug` | `-O0` | `-g` | No | Enabled | Development, sanitizers |
| (none) | Compiler default | — | No | Enabled | Quick iteration |

LTO is applied in Release builds via `-flto` on both compile and link steps.

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

Detects heap/stack buffer overflows, use-after-free, double-free, and memory
leaks.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DBUILD_TESTS=ON
cmake --build build
ASAN_OPTIONS="halt_on_error=1:detect_leaks=0" ctest --test-dir build
```

### UndefinedBehaviorSanitizer (UBSAN)

Detects signed integer overflow, null pointer dereference, misaligned access,
and other undefined behavior.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON -DBUILD_TESTS=ON
cmake --build build
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" ctest --test-dir build
```

ASAN and UBSAN can be combined. TSAN cannot be combined with either —
compiler constraint.

---

## 6. Build Outputs

Three executables are always produced from the same source tree. Each is
compiled with a distinct `TT_TARGET` define from `core/tt_target.h`:

| Binary | `TT_TARGET` | Default mode | Can place live orders |
|---|---|---|---|
| `engine_backtest` | `TT_TARGET_BACKTEST` (1) | `backtest` | No |
| `engine_shadow` | `TT_TARGET_SHADOW` (2) | `shadow` | No |
| `engine_live` | `TT_TARGET_LIVE` (3) | `live` | Yes |

`--mode=live` is rejected at runtime by any binary where
`target_allows_live_orders()` returns false — so only `engine_live` can ever
submit real orders, regardless of flags.

### Optional outputs

| Artifact | Condition | Path |
|---|---|---|
| `libtruetest.so` | `BUILD_SHARED_LIB=ON` | `build/libtruetest.so` |
| `truetest_tests` | `BUILD_TESTS=ON` | `build/truetest_tests` |
| `truetest_cli_tests` | `BUILD_TESTS=ON` | `build/truetest_cli_tests` |
| `truetest_benchmarks` | `ENABLE_BENCHMARKS=ON` | `build/truetest_benchmarks` |

---

## 7. FetchContent Dependencies

Automatically downloaded at configure time. Pinned tags are also captured in
the build audit header.

| Library | Version | Purpose |
|---|---|---|
| CLI11 | v2.4.2 | Command-line argument parsing |
| zstd | v1.5.6 | Binary event-log compression |
| nlohmann/json | v3.11.3 | Config-file parsing and C API serialization (not hot-path) |
| GoogleTest | v1.15.2 | Unit tests (`BUILD_TESTS=ON` only) |
| Google Benchmark | v1.8.5 | Performance benchmarks (`ENABLE_BENCHMARKS=ON` only) |
| libpqxx | 7.9.2 | PostgreSQL client (`ENABLE_POSTGRESQL=ON` only) |
| Abseil | 20240722.0 | Debug instrumentation logging (`ENABLE_DEBUG=ON` only) |

---

## 8. CMake Presets

Defined in `CMakePresets.json`:

| Preset | Platform | Generator | Build dir | Default type |
|---|---|---|---|---|
| `linux-default` | Linux | Unix Makefiles | `out/build/linux-default` | Debug |
| `windows-ninja` | Windows | Ninja | `out/build/windows-ninja` | Debug |
| `windows-vs-2022` | Windows | Visual Studio 17 2022 (x64) | `out/build/windows-vs-2022` | Debug;Release |

```bash
cmake --preset linux-default
cmake --build out/build/linux-default
```

---

## 9. Build Audit Header

CMake generates `build/generated/tt/truetest_version.h` at configure time. It
captures:

- `TRUETEST_VERSION` — project version (currently `0.1.0`)
- `TRUETEST_GIT_SHA` — 12-char abbreviated commit hash
- `TRUETEST_GIT_DIRTY` — `clean` or `dirty`
- `TRUETEST_BUILD_TIMESTAMP` — UTC ISO-8601
- `TRUETEST_BUILD_TYPE` — `Debug` / `Release` / `RelWithDebInfo`
- `TRUETEST_CXX_COMPILER` — compiler id and version
- Pinned dependency tags for all FetchContent libraries

Every binary emits an `AUDIT` log line at startup using these values so
production runs can be traced back to the exact source, flags, and deps.

---

## 10. Install and Packaging

### Installing locally

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /opt/truetest
```

This installs:
- `<prefix>/bin/engine_backtest`, `engine_shadow`, `engine_live`
- `<prefix>/share/truetest/web/index.html` — the WebSocket dashboard entry page
- When `BUILD_SHARED_LIB=ON`: `<prefix>/lib/libtruetest.so` +
  `<prefix>/include/truetest/truetest_api.h`

### Creating packages

TrueTest uses CPack for distributable packages.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && cpack -G TGZ
```

Produces:
- `truetest-0.1.0-Linux.tar.gz` — portable tarball containing all three
  engine binaries
- `truetest-0.1.0-Linux.deb` — Debian/Ubuntu package (with `-G DEB`)

Install the `.deb` package:

```bash
sudo dpkg -i truetest-0.1.0-Linux.deb
```

---

## 11. Running TrueTest

Each binary accepts the same CLI. Flags are parsed by CLI11; unknown flags are
rejected. The runtime dispatches on flags rather than the binary name:

| Runtime mode | Trigger | Description |
|---|---|---|
| **TUI** | No `--provider` or `--replay` | Interactive menu-driven setup |
| **Provider** | `--provider <name>` | Headless run using a registered provider |
| **Replay** | `--replay <path>` | Replay a binary event log |

```bash
# TUI mode (choose backtest binary for interactive exploration)
./build/engine_backtest

# Provider mode
./build/engine_backtest --provider local --path market_data.csv --strategy sma

# Shadow streaming with Binance
./build/engine_shadow --provider binance --symbol btcusdt --stream trade --web-ui

# Live execution (real money) — only engine_live accepts --mode live
./build/engine_live --provider binance --symbol btcusdt --stream kline_1m \
  --live --api-key "$KEY" --api-secret "$SECRET"

# Replay
./build/engine_backtest --replay event_log.bin
```

Run `<binary> --help` for a full flag summary or `--help-all` for expanded
help.

---

## 12. CLI Flags Reference

All three binaries accept the same CLI.

### Mode selection

| Flag | Values | Default | Description |
|---|---|---|---|
| `--mode` | `backtest`, `shadow`, `live` | Per-binary default | Engine operating mode. `live` rejected on non-live binaries |
| `--live` | flag | off | Safety flag required for live (real money) execution |
| `--dry-run` | flag | off | Validate config, print summary, exit |
| `--dump-config` | flag | off | Print resolved config as JSON and exit |
| `--config` | path | none | Load configuration from a JSON file. CLI flags override file values |

### Data provider

| Flag | Values | Default | Description |
|---|---|---|---|
| `--provider` | `local`, `binance` | none (TUI mode) | Data provider. Omit for interactive TUI |
| `--path` | file path | none | CSV file path for `local` provider. Accepts comma-separated paths for multi-symbol |
| `--format` | `bar`, `tick` | auto-detected | Data format. Auto-detected for Binance based on `--stream` |
| `--symbol` | string | none | Trading symbol (e.g. `btcusdt`) |
| `--stream` | `trade`, `kline_1m`, `kline_5m`, `depth`, etc. | none | Stream type for Binance provider |
| `--host` | hostname | exchange default | WebSocket host override |
| `--port` | number | exchange default | Port override |
| `--testnet` | flag | off | Route Binance to spot testnet (`stream.testnet.binance.vision`) |

### Binance credentials

| Flag | Description |
|---|---|
| `--api-key` | Binance API key. Required for `--mode=live` |
| `--api-secret` | Binance API secret. Required for `--mode=live` |

### Strategy

| Flag | Values | Default | Description |
|---|---|---|---|
| `--strategy` | `mean-reversion`, `sma`, `ma-crossover` | `mean-reversion` | Comma-separated for multi-strategy (e.g. `sma,mean-reversion`) |
| `--sma-period` | integer | 20 | SMA indicator lookback window (legacy; prefer `--param period=N`) |
| `--param` | `key=value` | none | Repeatable. Passed to `IStrategy::set_param()` |

### Portfolio & risk

| Flag | Default | Description |
|---|---|---|
| `--balance` | `10000.0` | Initial cash balance |
| `--risk-fraction` | `0.02` | Position size as fraction of equity (2%) |
| `--sl` | `0.005` | Stop-loss as fraction of entry price (0.5%) |
| `--tp` | `0.01` | Take-profit as fraction of entry price (1.0%) |
| `--max-daily-loss` | `0.0` | Maximum daily loss before halt (0 = no limit) |
| `--daily-reset-hour` | `0` | UTC hour (0-23) to reset daily loss counter |
| `--max-trades-per-hour` | `0` | Maximum fills per rolling hour (0 = no limit) |
| `--max-orders-per-minute` | `0` | Maximum orders per rolling minute (0 = no limit) |
| `--risk-unwind` | flag | On risk halt, unwind all open positions before stopping |

### Fee model

| Flag | Values | Default | Description |
|---|---|---|---|
| `--fee` | `fixed`, `tiered` | none (zero fees) | Fee model type. Binance provider auto-applies `tiered` at 0.1%/0.1% if unset |
| `--fee-value` | double | `0.0` | Flat fee per trade (for `fixed` model) |
| `--maker-rate` | double | `0.0` | Maker rate (for `tiered` model) |
| `--taker-rate` | double | `0.0` | Taker rate (for `tiered` model) |

### Threading

| Flag | Values | Default | Description |
|---|---|---|---|
| `--thread-preset` | `inline`, `light`, `standard`, `full`, `extended` | auto-detected | Worker thread preset |
| `--spin-policy` | `spin`, `yield`, `adaptive` | `adaptive` | Worker spin/wait policy |
| `--no-pin` | flag | off | Disable CPU affinity pinning (`sched_setaffinity`) |
| `--seed` | uint64 | `0` (random) | RNG seed for deterministic runs |

Thread preset auto-selection:

| Physical cores | Preset | Workers |
|---|---|---|
| 1-2 | `inline` | 0 (single-threaded) |
| 3 | `light` | 1 (ObserverWorker: logging + risk + stats combined) |
| 4-5 | `standard` | 2 (LoggingWorker + RiskStatsWorker) |
| 6-7 | `full` | 3 (LoggingWorker + RiskWorker + StatsWorker) |
| 8+ | `extended` | 4 (+ MarketMakerWorker) |

### Persistence

| Flag | Default | Description |
|---|---|---|
| `--db` | `truetest.db` | SQLite database path |
| `--no-db` | flag | Disable SQLite persistence entirely |

### Checkpointing

| Flag | Default | Description |
|---|---|---|
| `--checkpoint` | none | Write periodic portfolio snapshots to this binary file |
| `--checkpoint-interval` | `10000` | Write a checkpoint every N events. `0` = only at shutdown |
| `--resume` | none | Restore portfolio state from a checkpoint before running |

### Historical backfill (Binance)

| Flag | Default | Description |
|---|---|---|
| `--backfill` | `500` | Number of historical candles to fetch via REST before streaming. `0` = disabled |
| `--backfill-interval` | matches stream | Kline interval for backfill (e.g. `1m`, `5m`) |

### Event logging & recording

| Flag | Default | Description |
|---|---|---|
| `--log-events` | none | Path to write binary event log (zstd-compressed by default) |
| `--compress-log` / `--no-compress-log` | on | Toggle zstd compression for binary event logs |
| `--log-file` | stderr | Path to write operational text log (L1 structured logger) |
| `--log-max-size` | `0` (no rotation) | Max size in MB per log file before rotation (L3) |
| `--log-keep` | `5` | Number of rotated log files to retain (L3) |
| `--replay` | none | Path to a binary event log for replay mode |
| `--replay-from` | `0` | Replay from this timestamp (microseconds since epoch) |
| `--replay-to` | `INT64_MAX` | Replay to this timestamp (microseconds since epoch) |
| `--record` | none | Record raw transport data to file (for later replay) |
| `--replay-data` | none | Replay previously recorded transport data |

### Orderbook & fill tuning

| Flag | Default | Description |
|---|---|---|
| `--aggression` | `1.1` | Market order aggression factor. Buy: `price × aggr`; sell: `price × (2 − aggr)` |
| `--qty-scale` | `1e8` | Quantity scale factor (fractional → integer conversion) |
| `--fill-rng-seed` | `42` | RNG seed for fill probability model |
| `--spread-step` | `0.0001` | Spread step factor (`mid × factor`) |
| `--debug-fills` | flag | Log the first N fills with full book state |
| `--debug-fills-budget` | `20` | How many fills to log when `--debug-fills` is on |

### WebSocket UI

| Flag | Default | Description |
|---|---|---|
| `--web-ui` | flag | Enable WebSocket + HTTP server for the React frontend |
| `--ws-port` | `8765` | WebSocket server listen port |
| `--ws-compress` / `--no-ws-compress` | on | Per-message deflate compression |

### Analytics & output

| Flag | Default | Description |
|---|---|---|
| `--rolling-window` | `252` | Rolling metrics window size (number of bars) |
| `--risk-free-rate` | `0.0` | Annual risk-free rate for Sharpe/Sortino ratios |
| `--output` | none (stdout) | Write results to file |
| `--output-format` | `json` | Output format: `json` or `csv` |

---

## 13. JSON Configuration File

Pass `--config config.json` to load settings from a file. CLI flags take
precedence over file values. All keys are optional — omitted keys use their
default values. Key names use underscores (e.g. `sma_period`, not
`--sma-period`).

### Structure

```json
{
  "provider": "binance",
  "symbol": "btcusdt",
  "stream": "kline_1m",
  "strategy": "sma",
  "sma_period": 50,
  "balance": 50000.0,
  "risk_fraction": 0.01,
  "sl": 0.003,
  "tp": 0.008,
  "fee": "tiered",
  "maker_rate": 0.001,
  "taker_rate": 0.001,
  "mode": "backtest",
  "db": "my_run.db",
  "web_ui": true,
  "ws_port": 8765,
  "ws_compress": true,
  "thread_preset": "full",
  "seed": 12345,
  "backfill": 1000,
  "backfill_interval": "5m",
  "rolling_window": 252,
  "risk_free_rate": 0.05,
  "output": "results.json",
  "output_format": "json",
  "risk": {
    "max_position_value": 100000,
    "max_drawdown": 0.20,
    "max_loss_per_trade": 5000,
    "max_open_orders": 50,
    "max_portfolio_exposure": 500000,
    "max_daily_loss": 2000,
    "daily_reset_hour": 0,
    "max_trades_per_hour": 100,
    "max_orders_per_minute": 30
  }
}
```

### CLI overrides config file

```bash
# Config file sets balance=50000 and strategy=sma;
# CLI overrides balance to 10000 and strategy to mean-reversion.
./build/engine_backtest --config config.json --balance 10000 --strategy mean-reversion
```

### Dump resolved config

```bash
./build/engine_backtest --config config.json --balance 10000 --dump-config
```

Prints the merged configuration (file + CLI) as JSON and exits — useful for
debugging config precedence or generating a file template from CLI flags.

---

## 14. Dry Run Mode

`--dry-run` validates the resolved configuration, prints a human-readable
summary, and exits without running the engine.

```bash
./build/engine_backtest --dry-run --strategy sma --balance 50000 --mode backtest
```

Example output:

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
- `--strategy` must be a registered strategy name (built-in: `mean-reversion`,
  `sma`, `ma-crossover`)
- `--mode` must be one of `backtest`, `shadow`, `live`
- `--fee` must be one of `fixed`, `tiered`
- `--thread-preset` must be a valid preset name

Useful for CI pipelines and deployment validation — verify your config without
starting the engine or requiring data files.

---

## 15. Interactive TUI Mode

Without `--provider` or `--replay`, TrueTest presents an interactive text
menu. The menus walk you through:

1. **Strategy selection** — Mean Reversion, SMA, or MA Crossover
2. **SMA period** — numeric input (default: 20)
3. **Data source** — PostgreSQL (if compiled), CSV file, or Tick CSV file,
   or Live WebSocket (if compiled)
4. **Fee model** — Zero fees, Fixed fee, or Tiered (maker/taker)
5. **Engine mode** — Backtest, Shadow, or Live (availability gated by binary)

Each menu shows numbered options. Enter the number and press Enter. Invalid
input falls back to the default (typically option 1).

PostgreSQL appears in the data source menu only when built with
`-DENABLE_POSTGRESQL=ON`; it shows as unavailable otherwise. Live WebSocket
likewise requires `-DENABLE_LIVE_DATA=ON`.

---

## 16. Provider Mode

Providers are self-registering modules that handle data sourcing and
execution. Every external data/execution flow goes through `IProvider`, which
owns a `IDataTransport`, a `IDataParser<T>`, and an `IExecutionAdapter`. Use
`--provider <name>` to bypass the TUI and run headless.

### 16.1 Local provider

File-based CSV data. Batch mode only. Always compiled in (zero deps).

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma
```

Supports OHLCV bar format and tick format. Format is auto-detected from CSV
headers. Use `--format tick` to force tick parsing.

```bash
./build/engine_backtest \
  --provider local \
  --path ticks.csv \
  --format tick \
  --strategy mean-reversion
```

Multi-file multi-symbol backtesting via comma-separated `--path` — see
[Data Sources](#19-data-sources).

### 16.2 Binance provider

Live WebSocket streaming from Binance spot market. Requires
`-DENABLE_BINANCE=ON` at build time. Parser is pure C++ (no JSON library on
the hot path).

```bash
# Paper trading (shadow binary, shadow mode default)
./build/engine_shadow \
  --provider binance --symbol btcusdt --stream trade \
  --strategy mean-reversion --web-ui

# Kline stream with backfill and tiered fees
./build/engine_shadow \
  --provider binance --symbol ethusdt --stream kline_1m \
  --fee tiered --maker-rate 0.001 --taker-rate 0.001 \
  --backfill 500
```

**Supported streams:**

- `trade` — individual trade ticks
- `kline_1m`, `kline_5m`, ... — 1-minute / 5-minute / etc. candlestick
  aggregation
- `depth` — L2 orderbook depth (used by `HybridExecutor` for realistic
  paper-mode limit fills against real exchange depth)
- combined multi-stream (via `BinanceCombinedTransport`)

**Execution modes:**

- **Paper** (default) — orders logged, fills simulated from last price.
- **Hybrid** — paper market orders + local-book limit fills (default for
  backtest/shadow/paper modes). Owns synthetic book-seeding around the mid
  price.
- **Live** — signed REST order submission against `/api/v3/order` via
  `BinanceRestClient`. `poll_live_fills()` polls order status for fills.
  Cancel and modify are wired. Requires `--live` flag, `--api-key`,
  `--api-secret`, and explicit `YES` confirmation at the prompt.

**Historical backfill.** Historical bars are fetched via REST and injected
into the live stream through `PrependTransport` — invisible to the engine.

**L2 depth stream (paper fills against real book):**

```bash
./build/engine_shadow \
  --provider binance --symbol btcusdt --stream depth \
  --strategy mean-reversion --backfill 200 --web-ui
```

The local orderbook receives live snapshots and incremental updates, so
limit orders match against the real book instead of the synthetic
MarketMaker-seeded one.

### 16.3 Recording and replaying Binance data

`BinanceRecorder` captures a live stream to file;
`BinanceReplayTransport` replays it as a transport. Useful for deterministic
testing against real exchange data.

```bash
# Record
./build/engine_shadow \
  --provider binance --symbol btcusdt --stream trade \
  --record captured_ws.bin

# Replay — same parser, provider, and executor paths as the live run
./build/engine_backtest \
  --provider binance --symbol btcusdt --stream trade \
  --replay-data captured_ws.bin --strategy sma
```

Pair with `--seed <n>` for fully deterministic shadow runs against real data.

### 16.4 Live trading (real orders)

**Only `engine_live` accepts `--mode live`.** The other binaries fail the
live-target gate at startup.

```bash
./build/engine_live \
  --provider binance --symbol btcusdt --stream kline_1m \
  --live --api-key "$BINANCE_API_KEY" --api-secret "$BINANCE_API_SECRET" \
  --strategy sma --balance 1000 \
  --max-daily-loss 50 --max-trades-per-hour 20 \
  --risk-unwind
```

**This submits real orders against Binance.** Both `--mode live` (implied by
the binary default) and the `--live` safety flag are required, and the CLI
prints an explicit confirmation prompt that requires typing `YES` before
proceeding. Orders are signed with HMAC-SHA256 and submitted to
`/api/v3/order`. Always pair with tight `--max-daily-loss`,
`--max-trades-per-hour`, and `--risk-unwind` so a bug cannot drain the
account.

For testnet:

```bash
./build/engine_live --provider binance --symbol btcusdt --stream trade \
  --testnet --live --api-key "$KEY" --api-secret "$SECRET" \
  --strategy sma --balance 10000
```

---

## 17. Replay Mode

Replay a previously recorded binary event log:

```bash
./build/engine_backtest --replay event_log.bin
```

Event logs are recorded with `--log-events <path>`. Logs are compressed with
zstd by default (disable with `--no-compress-log`). The replayer auto-detects
compressed vs uncompressed files.

### Time-based seeking

Event logs include an index that maps timestamps to file offsets (one entry
per 1000 events). Use `--replay-from` and `--replay-to` to replay a time
range instead of the entire file:

```bash
./build/engine_backtest --replay event_log.bin \
  --replay-from 1700000000000000 \
  --replay-to   1700003600000000
```

When an index is present, the replayer binary-searches for the start offset
instead of scanning from the beginning.

---

## 18. Strategies

Strategies are registered via `StrategyRegistry` and looked up by name at
runtime. All strategies support runtime parameter configuration via
`--param key=value` (repeatable). Each strategy defines its accepted
parameters through `get_param_schema()`, which returns the name, default,
min, max, and description for each parameter.

### 18.1 Mean Reversion (`mean-reversion`)

Default strategy. Buys when the current price drops below the SMA and sells
when it rises above. Uses configurable stop-loss and take-profit levels.

Parameters (via `--param` or legacy flags):

- `period` (default `20`) — SMA lookback (`--sma-period`)
- `equity` (default `10000`) — account equity for position sizing (`--balance`)
- `risk_fraction` (default `0.02`) — fraction of equity per trade (`--risk-fraction`)
- `sl_pct` (default `0.005`) — stop loss as fraction of entry price (`--sl`)
- `tp_pct` (default `0.01`) — take profit as fraction of entry price (`--tp`)

```bash
./build/engine_backtest \
  --provider local --path market_data.csv \
  --strategy mean-reversion \
  --param period=30 --param risk_fraction=0.01 \
  --param sl_pct=0.003 --param tp_pct=0.006
```

### 18.2 SMA Strategy (`sma`)

Buys when price > SMA, sells when price < SMA.

Parameters:

- `period` (default `20`) — SMA lookback period

```bash
./build/engine_backtest --provider local --path market_data.csv \
  --strategy sma --param period=50
```

### 18.3 MA Crossover (`ma-crossover`)

Dual-SMA crossover. Buys on golden cross (fast SMA crosses above slow SMA)
and sells on death cross (fast SMA crosses below slow SMA).

Parameters:

- `fast_period` (default `10`) — fast SMA lookback
- `slow_period` (default `50`) — slow SMA lookback

```bash
./build/engine_backtest --strategy ma-crossover \
  --param fast_period=5 --param slow_period=20
```

### 18.4 Indicators

The following indicators are available for use in strategies:

| Indicator | Header | Class | Key method |
|---|---|---|---|
| Simple Moving Average | `indicator/sma.h` | `simple_moving_average` | `update(price) → optional<double>` |
| Exponential Moving Average | `indicator/ema.h` | `exponential_moving_average` | `update(price) → optional<double>` |
| Relative Strength Index | `indicator/rsi.h` | `relative_strength_index` | `update(price) → optional<double>` |
| Bollinger Bands | `indicator/bollinger.h` | `bollinger_bands` | `update(price) → optional<bollinger_result>` |

All follow the same pattern: call `update(price)` on each bar/tick; returns
`std::nullopt` during warmup, then the computed value once enough data has
accumulated. Use `ready()` to check and `value()` to read the last result.

### 18.5 Strategy registry

Strategies self-register via `REGISTER_STRATEGY("name", factory)` (mirroring
the provider registry). To add a new strategy:

1. Create the class implementing `IStrategy`
2. Add `REGISTER_STRATEGY` in the `.cpp` file with a factory lambda
3. The strategy becomes available by name via `--strategy <name>`

All strategies expose their indicator values via `get_indicator_values()` for
use in analytics and the WebSocket UI.

### 18.6 Multi-strategy runs

`--strategy` accepts a comma-separated list. The first entry becomes the
**primary** strategy; subsequent entries run alongside it and share the same
portfolio, orderbook registry, and risk manager.

```bash
./build/engine_backtest --provider local --path market_data.csv \
  --strategy mean-reversion,sma,ma-crossover \
  --param period=20 --param fast_period=10 --param slow_period=50
```

Behaviour:

- On every market/tick event, the primary strategy dispatches first, then
  each additional strategy in order.
- Orders are tagged with `order_event::strategy_name`. Fills inherit the tag,
  enabling per-strategy attribution in analytics and the event log.
- SL/TP stops (`check_stops()`) are evaluated independently per strategy —
  each manages its own position state via `set_position_open()`.
- `--param key=value` applies to **all** strategies; unknown params are
  silently ignored.
- Risk checks, fees, and the orderbook are global; the risk manager sees
  combined exposure.

Multi-strategy works in batch, streaming, tick, and replay loops.

---

## 19. Data Sources

### 19.1 Validation

All data records (bars and ticks) are validated on load. Invalid records are
logged with a warning and skipped. `data_handler::validation_errors()`
returns the count of rejected records.

- **Bars**: `open`, `high`, `low`, `close` must be positive; `high >= low`;
  `volume >= 0`.
- **Ticks**: `price` must be positive; `quantity` must be positive;
  timestamps must be monotonically increasing.

### 19.2 CSV (OHLCV bars)

```
timestamp,symbol,open,high,low,close,volume
```

Timestamps may be ISO 8601 or Unix epoch. Rows are loaded in order into the
in-memory `data_handler` before the engine runs.

### 19.3 Tick CSV

```
timestamp_ms,symbol,price,quantity,side
```

`side` is `B` (buy aggressor), `A` (sell aggressor), or empty/`U` (unknown).

### 19.4 Binary cache

`BinaryCacheSource` wraps any `IDataSource` and caches the parsed result as
a binary file. Subsequent runs skip the CSV parse step entirely.

Cache format: 16-byte header (4-byte magic `TTBC`, 2-byte format version,
2-byte reserved, 8-byte CRC-64 of payload). On read, magic, version, and
checksum are verified; incompatible or corrupt caches are automatically
deleted and regenerated.

Transparent — no CLI flag needed. A `<path>.cache` file is written alongside
the data file on first run and picked up automatically afterwards.

### 19.5 PostgreSQL

Available with `-DENABLE_POSTGRESQL=ON`. Connects to a PostgreSQL database
and loads OHLCV via SQL queries. Connection parameters are prompted in TUI
mode (no dedicated CLI flag set yet):

```bash
./build/engine_backtest
# Select: Data source → PostgreSQL, then enter host/db/user/password/query
```

### 19.6 WebSocket data source

Available with `-DENABLE_LIVE_DATA=ON`. Connects to a generic WebSocket
endpoint for live streaming. The parser supports two JSON message formats:

**Tick:**
```json
{"type":"tick","symbol":"BTCUSDT","price":50000.0,"qty":0.5,"ts":1700000000000}
```

**Bar:**
```json
{"type":"bar","symbol":"BTCUSDT","o":49900,"h":50100,"l":49800,"c":50000,"v":1000,"ts":1700000000000}
```

Fields:

- `type` (required): `"tick"` or `"bar"`
- `symbol` (required)
- `ts` (optional): ms since epoch (defaults to current time)
- `seq` (optional): sequence number for gap detection

Includes automatic reconnection with exponential backoff, heartbeat
keepalive, and sequence-gap detection. Wired through the TUI rather than a
dedicated CLI flag set:

```bash
./build/engine_backtest
# Select: Data source → Live WebSocket, then enter host/port/symbol
```

### 19.7 Multi-symbol backtesting

A single strategy (or a multi-strategy ensemble) can run across multiple
symbols in the same run. Every symbol gets its own orderbook via
`OrderbookRegistry`; bars/ticks are routed to the correct book by their
`symbol` field.

For the `local` provider, comma-separated `--path` loads several CSV files
into the same `data_handler`:

```bash
./build/engine_backtest --provider local \
  --path btcusdt.csv,ethusdt.csv,solusdt.csv \
  --strategy mean-reversion
```

Each file is parsed in sequence, then all bars are stably sorted by
`timestamp` so events interleave chronologically. The main loop dispatches
each bar to the strategy; strategies keeping per-symbol state (e.g. SMA
windows in `unordered_map<string, ...>`) transparently track every symbol.

Notes:

- Risk manager, portfolio, analytics, and fee model are **global** — all
  symbols share the same cash, exposure, and PnL accounting.
- Per-symbol analytics attribution via `fill_event::get_symbol()` in the
  event log.
- For streaming providers (Binance), only a single `--symbol` is honored in
  streaming mode today. Use batch mode with multi-file `--path` for
  multi-symbol backtests.

---

## 20. Fee Models

Fee models are applied to every fill event to calculate commission.

### 20.1 Zero fees (default)

No commission charged. Suitable for initial strategy development.

### 20.2 Fixed fee

`--fee fixed --fee-value <amount>` — flat fee per trade regardless of size.

```bash
./build/engine_backtest --provider local --path data.csv \
  --fee fixed --fee-value 1.50
```

### 20.3 Tiered fee

`--fee tiered --maker-rate <rate> --taker-rate <rate>` — separate maker and
taker rates applied as a fraction of trade value.

```bash
./build/engine_backtest --provider local --path data.csv \
  --fee tiered --maker-rate 0.001 --taker-rate 0.002
```

Applies 0.1% for maker orders and 0.2% for taker orders. Binance provider
auto-applies a 0.1%/0.1% tiered default if `--fee` is unset.

---

## 21. Risk Management

The risk manager validates orders and fills against configurable limits. It
returns one of four actions:

| Action | Effect |
|---|---|
| `pass` | Order/fill accepted, processing continues |
| `reject` | Order rejected (rejection event emitted), engine continues |
| `halt` | Engine stopped immediately |
| `unwind` | All open positions closed via market orders, then engine halts |

Risk checks run **synchronously on the hot path** (pre-order and post-fill)
for real-time rejection. A `rejection` event is emitted through the full
event pipeline (ring buffers, event log, WebSocket UI). The async shadow
check in the risk worker thread remains as a secondary validation layer.

### 21.1 Static risk limits (config file only)

| Limit | Default | Triggers |
|---|---|---|
| `max_position_value` | $1,000,000,000 | Order rejection |
| `max_drawdown` | 30% | Engine halt |
| `max_loss_per_trade` | $10,000 | Engine halt |
| `max_open_orders` | 1,000 | Order rejection |
| `max_portfolio_exposure` | $5,000,000,000 | Order rejection |

### 21.2 Time-based risk limits (CLI or config)

| Limit | Default | CLI flag | Triggers |
|---|---|---|---|
| Max daily loss | `0` (off) | `--max-daily-loss` | Engine halt |
| Daily reset hour (UTC) | `0` | `--daily-reset-hour` | — |
| Max trades per hour | `0` (off) | `--max-trades-per-hour` | Engine halt |
| Max orders per minute | `0` (off) | `--max-orders-per-minute` | Order rejection |

Trades/hour = rolling 60 minutes; orders/minute = rolling 60 seconds. Daily
loss resets at the configured UTC hour (default: midnight). `0` disables.

### 21.3 Automatic position unwinding

By default, a risk halt stops the engine immediately. With `--risk-unwind`,
the engine first closes all open positions via market sell orders, then
halts. This prevents leaving orphaned positions on a live exchange.

```bash
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --max-daily-loss 500 --max-trades-per-hour 100 --risk-unwind
```

### 21.4 Risk in config files

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

Defaults are deliberately permissive. For production or live trading, set
appropriate limits via CLI or config.

---

## 22. Threading Model

Lock-free architecture with SPSC (single-producer, single-consumer) ring
buffers for inter-thread communication. The event loop always runs on the
main thread (ideally Core 0). Worker threads consume events from dedicated
ring buffers.

### 22.1 Presets

Auto-detected from physical core count, overridable with `--thread-preset`:

| Preset | Cores | Workers |
|---|---|---|
| `inline` | 1-2 | 0 — single-threaded, all processing on main thread |
| `light` | 3 | 1 ObserverWorker (logging + risk + stats combined) |
| `standard` | 4-5 | LoggingWorker + RiskStatsWorker |
| `full` | 6-7 | LoggingWorker + RiskWorker + StatsWorker |
| `extended` | 8+ | + MarketMakerWorker |

### 22.2 CPU affinity

On Linux, the event loop and all workers are pinned via `sched_setaffinity`
to reduce context switching and cache thrashing. The event loop is pinned to
Core 0 (or the core specified by `engine_config::pin_event_loop`). Workers
are pinned according to the core map derived from system topology.

Use `--no-pin` in containers or VMs with restricted CPU access.

### 22.3 Worker spin policy

Workers poll their ring buffers in a loop. `--spin-policy` controls behavior
when no events are available:

| Policy | Behavior |
|---|---|
| `spin` | Pure busy-wait — lowest latency, highest CPU usage |
| `yield` | Always calls `std::this_thread::yield()` — lowest CPU, higher latency |
| `adaptive` | Exponential backoff: spin 64 iterations, then `_mm_pause` for 256, then yield (default) |

`adaptive` balances latency and CPU usage: sub-microsecond wake-up during
bursts, near-zero CPU when idle.

### 22.4 Ring buffers

Each worker has a ring buffer with 65,536 slots (configurable via
`engine_config::ring_buffer_capacity`). Atomic load/store operations — no
mutexes or syscalls on the hot path.

**Watermark metrics** (always-on, not gated behind `HAS_DEBUG`):

- `high_watermark()` — maximum observed occupancy since construction
- `drop_count()` — number of events dropped by `DropOldest` policy
- `on_watermark(threshold, callback)` — optional callback when occupancy
  exceeds a threshold (e.g. 75% of capacity)

High watermark statistics are printed at engine shutdown alongside drop
counts.

---

## 23. WebSocket UI

`--web-ui` starts a Boost.Beast server that handles both WebSocket
connections and HTTP REST requests on the same port. Open the React SPA at
`web/index.html` (or serve via any HTTP server); the dashboard connects to
`ws://localhost:8765` and renders:

- Live equity curve
- Fill history
- Open positions
- Analytics (Sharpe, Sortino, drawdown, win rate)
- Orderbook visualization

### 23.1 Per-message deflate compression

Default on. Typically reduces JSON payload size by 60-80%. Negotiated during
handshake; clients without support fall back transparently. Disable with
`--no-ws-compress` for very low-latency requirements.

### 23.2 Command validation

All incoming WebSocket commands are validated against a per-command schema
before processing. Malformed commands receive:

```json
{"type": "error", "data": {"message": "order: missing required field 'side'", "source": "ws_validator"}}
```

Both `"command"` and `"cmd"` field names are accepted. Unrecognized
commands are rejected. Per-command requirements:

- **order**: `side` (buy/sell), `quantity` (> 0); `type` defaults to
  `"market"`; limit orders require `price > 0`.
- **set_timeframe**: `timeframe`
- **set_symbol**: `value`
- **set_strategy**: `value`
- **start**, **pause**, **stop**: no fields required

### 23.3 Client commands

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

### 23.4 Event filtering (subscribe)

Clients can subscribe to specific event types and/or symbols to reduce
bandwidth. Send `subscribe` after connecting:

```json
{"cmd": "subscribe", "events": ["fill", "market"], "symbols": ["BTCUSDT", "ETHUSDT"]}
```

- **events**: array of event type strings (`fill`, `tick`, `market`,
  `order`, `status`, `orderbook`, `error`). Empty = all.
- **symbols**: array of symbols. Empty = all. Case-insensitive. Messages
  without a symbol field (status, error) always pass.

Default (no subscribe command): all events are sent (backward compatible).
Server ack:

```json
{"type": "subscribed", "data": {"events": 2, "symbols": 1}}
```

### 23.5 REST API

The same port serves HTTP REST endpoints. All responses are JSON with CORS
headers.

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/health` | Health check (`{"status":"ok"}`) |
| POST | `/api/backtest` | Submit a backtest (config JSON body) |
| GET | `/api/backtest` | List all backtest runs |
| GET | `/api/backtest/<id>/status` | Status of a specific run |
| GET | `/api/backtest/<id>/results` | Results of a completed run |
| GET | `/api/runs` | List recent runs from the SQLite `runs` table (limit param supported) |
| GET | `/metrics` | Prometheus metrics exposition (see §27.2) |

```bash
curl -X POST http://localhost:8765/api/backtest \
  -H "Content-Type: application/json" \
  -d '{"strategy":"sma","path":"market_data.csv"}'

curl http://localhost:8765/api/backtest/1/status
curl http://localhost:8765/api/backtest/1/results
```

Custom port: `--ws-port 9000`.

---

## 24. Event Pipeline

Sequential event-driven pipeline:

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

Hot-path events use pre-allocated object pools to avoid heap pressure.

### 24.1 Event types

| Type | Struct | Description |
|---|---|---|
| `market` | `market_event` | OHLCV candle bar |
| `tick` | `tick_event` | Individual trade tick |
| `signal` | `signal_event` | Strategy signal (buy/sell/hold) |
| `order` | `order_event` | Order submission to orderbook |
| `fill` | `fill_event` | Trade execution confirmation |
| `l2_snapshot` | `l2_snapshot_event` | Full orderbook depth snapshot |
| `l2_update` | `l2_update_event` | Incremental orderbook depth update |
| `cancel` | `cancel_event` | Order cancellation notification |
| `amend` | `amend_event` | Order modification (price/qty change) |
| `rejection` | `rejection_event` | Order rejected by risk manager (with reason) |

### 24.2 Order status tracking

`OrderTracker` tracks each order through its lifecycle:

| Status | Description |
|---|---|
| `pending` | Order created, awaiting processing (e.g. latency delay) |
| `open` | Order accepted and placed on orderbook |
| `partially_filled` | Order partially matched, remainder on book |
| `filled` | Order fully filled |
| `cancelled` | Order cancelled via `cancel_order()` API |
| `rejected` | Order rejected by risk manager |

Access via `engine::get_order_tracker()` which exposes `get_order_status(id)`,
`get_open_orders()`, and `is_active(id)`.

### 24.3 Order cancellation

`engine::cancel_order(symbol, order_id, reason)` cancels the order on the
execution adapter (removes from orderbook or sends REST DELETE to exchange),
emits a `cancel_event` through the ring buffers, and updates tracker status
to `cancelled`. Pending stop orders that haven't triggered yet can also be
cancelled.

### 24.4 Order modification (amend)

`engine::modify_order(symbol, order_id, new_price, new_qty)` cancels the
existing order and re-inserts it at the new price and quantity with the same
order ID. The order loses time priority (standard exchange amend behavior).

On success, an `amend_event` is emitted through the ring buffers and event
log, containing the order ID, symbol, new price, and new quantity.
Serialized in both JSON (WebSocket UI) and binary (event log).

Exposed through `IExecutionAdapter::modify_order()`:

- **LocalBookAdapter**: modifies the order on the local orderbook.
- **HybridExecutor**: delegates to the local book adapter (limit orders).
- **BinanceExecutor**: not yet implemented (returns false).
- **ExchangeAdapter**: not yet implemented (returns false).

### 24.5 L2 depth routing

Level-2 orderbook data from providers is routed directly to the matching
engine's orderbook.

**Provider sink** (`provider_sink.h`): `event_sink_l2()` accepts a
`provider::event` variant and an `orderbook` pointer. For `l2_snapshot`, it
converts provider levels to `Price`/`quantity` pairs and calls
`orderbook::apply_l2_snapshot()`. For `l2_update`, it calls
`orderbook::apply_l2_update()`.

**Engine API**: `engine::apply_l2_snapshot(symbol, bids, asks)` and
`engine::apply_l2_update(symbol, side, price, new_qty)` route L2 data to
the correct per-symbol orderbook via `OrderbookRegistry`, log the events,
and publish them through ring buffers.

L2 snapshots replace the entire orderbook state. L2 updates modify
individual price levels (quantity `0` removes the level).

### 24.6 Partial fills

Fill events include `remaining_qty` (quantity still on book after this
fill) and `fill_id` (unique identifier for multiple fills of one order).
Use `fill_event::is_partial()` to check. Portfolio and analytics handle
partial fills incrementally.

### 24.7 Order types

| Type | Description |
|---|---|
| `market` | Execute immediately at best available price |
| `limit` | Execute at specified price or better |
| `stop` | Becomes market order when stop price is reached |
| `stop_limit` | Becomes limit order when stop price is reached |

### 24.8 Time-in-force

| TIF | Description |
|---|---|
| `ioc` | Immediate-or-Cancel — fill what's available, cancel rest |
| `fok` | Fill-or-Kill — fill entire quantity or cancel |
| `gtc` | Good-Till-Cancel — remains until filled or cancelled |
| `day` | Expires at end of trading session |

---

## 25. SQLite Persistence

SQLite is enabled by default (`-DENABLE_SQLITE=ON`). It persists equity
curve data and trade history to a local database file. Both equity points
and fill records are batched into transactions (100 rows per txn) for
improved write throughput.

```bash
# Default path
./build/engine_backtest --provider local --path data.csv
# Creates truetest.db

# Custom path
./build/engine_backtest --provider local --path data.csv --db results.db

# Disable
./build/engine_backtest --provider local --path data.csv --no-db
```

Query with any SQLite client:

```bash
sqlite3 truetest.db ".tables"
sqlite3 truetest.db "SELECT * FROM equity_curve ORDER BY timestamp DESC LIMIT 10;"
```

### 25.1 Run metadata (`runs` table)

Every invocation that writes to SQLite also records a row in `runs`. A row
is inserted when the engine starts and updated when it finishes with the
final metrics — enabling run history and comparison across backtests without
parsing JSON exports.

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

When the WebSocket UI is enabled:

```bash
curl http://localhost:8765/api/runs
curl http://localhost:8765/api/runs?limit=20
```

If SQLite is disabled (`--no-db`), `GET /api/runs` returns HTTP 503.

### 25.2 Portfolio checkpoints (resume-after-crash)

Long-running sessions can write periodic portfolio snapshots to a binary
checkpoint file. On the next invocation, `--resume <path>` restores cash,
positions, and trade count before the run starts.

```bash
# Write a checkpoint every 10,000 events (default) to /tmp/session.ckpt
./build/engine_shadow --provider local --path data.csv \
  --checkpoint /tmp/session.ckpt

# Smaller interval for debugging
./build/engine_shadow --provider local --path data.csv \
  --checkpoint /tmp/session.ckpt --checkpoint-interval 500

# Resume from an existing checkpoint
./build/engine_shadow --provider local --path data.csv \
  --resume /tmp/session.ckpt
```

Flags:

- `--checkpoint <path>` — write periodic portfolio snapshots. A final
  checkpoint is written when the engine exits cleanly.
- `--checkpoint-interval <N>` — write every N events (default `10000`). `0` =
  only at shutdown.
- `--resume <path>` — restore portfolio state (cash, positions, trade
  count). Missing or malformed files are reported to stderr; the run
  continues with fresh state.

Checkpoints capture **portfolio state only**. Analytics accumulators,
orderbook state, and in-flight pending orders are not restored — they
rehydrate from the market data stream. For fully deterministic replay, pair
`--resume` with `--seed <n>` and a recorded event log.

### 25.3 Deterministic replay and RNG seeding

When `--seed <n>` is non-zero, every stochastic component is initialized
from that seed:

- `LocalBookAdapter` — fill-probability RNG (seeded with `seed + 2`).
- `MarketMaker` — spread jitter RNG (seeded with `seed + 1`).
- `engine::run()` — uses a fixed epoch base simulation timestamp
  (`chrono::system_clock::time_point(0)`) instead of wall clock, so market
  event timestamps are deterministic.

Re-running with the same seed, config, and input data produces byte-
identical results. `seed == 0` falls back to wall-clock / hardware seeds
(the non-deterministic default).

---

## 26. Analytics & Reporting

TrueTest computes a comprehensive set of analytics metrics during and after
each run. All metrics are computed incrementally using Welford's online
algorithm (O(1) per update) and are available in real-time via WebSocket
snapshots.

### 26.1 Cumulative metrics

Cumulative return, Sharpe ratio, Sortino ratio, max drawdown, Calmar ratio,
win rate, profit factor, average win/loss, largest winner/loser, time in
market, average holding period, average slippage.

### 26.2 Rolling window metrics

Rolling Sharpe ratio and rolling max drawdown over a configurable trailing
window of equity-to-equity returns. Default 252 bars (~1 year of daily
data); set via `--rolling-window`.

### 26.3 Risk-free rate adjustment

Sharpe and Sortino subtract a per-period risk-free rate derived from
`--risk-free-rate` (annualized). `0.0` reduces to the standard unadjusted
formulas.

### 26.4 Benchmark comparison

A buy-and-hold benchmark is tracked alongside strategy equity. The
benchmark invests 100% of initial capital at the first observed price and
holds. Report includes:

- **Alpha** — strategy excess return over benchmark per bar
- **Beta** — sensitivity of strategy returns to benchmark returns
- **Information ratio** — risk-adjusted excess return vs benchmark
- **Tracking error** — standard deviation of return differences

### 26.5 Per-symbol and per-strategy attribution

When multiple symbols are traded, the report breaks down PnL, trade count,
win rate, and profit factor per symbol. When `strategy_name` is set on
order events, per-strategy attribution is also available.

Appears in the printed report, JSON export (`per_symbol` and `per_strategy`
objects), and WebSocket analytics.

### 26.6 Console report output

End-of-run report printed to stdout via `analytics/report_generator.{h,cpp}`
using ASCII widgets from `analytics/ascii_widgets.{h,cpp}` — horizontal
bars, sparklines, histograms, aligned tables. UTF-8 aware, stdlib-only.

Sections (in order):

1. **Returns** — initial / final equity, total return, buy & hold,
   strategy vs benchmark, with bars.
2. **Risk** — Sharpe, Sortino, max drawdown, Calmar, rolling Sharpe,
   rolling max drawdown.
3. **Equity Curve** — one-line sparkline (`▁▂▃▄▅▆▇█`) across the full run,
   with min / max / point count.
4. **Trades** — total trades, win rate (with bar), profit factor, avg win /
   loss, largest winner / loser.
5. **Per-Trade PnL Distribution** — equal-width histogram (default 8 bins).
6. **Execution Quality** — avg slippage, total orders, total fills.
7. **Exposure** — time in market (with bar), avg holding period.
8. **Benchmark** — alpha, beta, information ratio, tracking error.
9. **Per-Symbol / Per-Strategy Attribution** — tables.
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

Public entry points in `analytics/report_generator.h`:

```cpp
#include "analytics/report_generator.h"

AnalyticsReport r = analytics.generate_report();

// Full report, default options
std::string text = tt::render_report(r);

// Customize
tt::report_options opts;
opts.width = 100;
opts.bar_width = 40;
opts.distribution_bins = 12;
opts.include_per_symbol = false;
std::string partial = tt::render_report(r, opts);

// Individual sections
std::string risk  = tt::render_risk_section(r, opts);
std::string worst = tt::render_worst_trades_section(r, opts);
```

#### Reusable widgets

`analytics/ascii_widgets.h` exposes:

| Widget | Purpose |
|---|---|
| `hbar(value, max, width)` | Horizontal bar with 1/8-block precision |
| `sparkline(values, max_width)` | One-line trend chart (`▁▂▃▄▅▆▇█`) |
| `equal_width_bins(values, n)` | Bucket continuous values for histograms |
| `horizontal_histogram(bins)` | Labelled horizontal bar chart |
| `section_header(title, width)` | `━━━ Title ━━━━━━━━━━` section divider |
| `table(headers, rows, align)` | Aligned table with per-column alignment |
| `fmt_money` / `fmt_signed_pct` | `12,345.67` / `+18.42%` formatting |

### 26.7 Structured result export

```bash
# JSON (default)
./build/engine_backtest --provider local --path data.csv --strategy sma \
  --output results.json

# CSV (equity curve + trades file)
./build/engine_backtest --provider local --path data.csv --strategy sma \
  --output results.csv --output-format csv
```

**JSON output** contains all scalar metrics, the full equity curve as
`[timestamp_ms, equity]` arrays, the complete trade log, and per-symbol /
per-strategy attribution objects.

**CSV output** writes the equity curve to the specified path and a
companion `_trades` file (e.g. `results_trades.csv`) with symbol and
strategy columns.

### 26.8 Configuration file support

```json
{
  "rolling_window": 100,
  "risk_free_rate": 0.05,
  "output": "results.json",
  "output_format": "json"
}
```

---

## 27. Observability & Debugging

Three-part always-on observability stack. None of these features require
optional CMake flags unless explicitly noted.

### 27.1 Structured operational logging (L1)

Zero-dependency logger in `BacktestEngine/src/utils/log/logger.h`. Emits
timestamped, level-tagged lines to stderr by default:

```
[2026-04-14T20:35:00.337Z] [INFO] [main] truetest starting (pid=107715)
```

Redirect to a file with `--log-file <path>`:

```bash
./build/engine_backtest --provider local --path market_data.csv \
  --strategy sma --log-file /var/log/truetest.log
```

Call sites use `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` macros — each takes a
component name + printf-style format. This is distinct from the trading-
event log handled by `LoggingWorker`: the operational log is for engine
lifecycle, config, startup errors, and diagnostics; the trading event log
captures market/order/fill events for replay.

### 27.2 Prometheus metrics endpoint (L2)

When `-DENABLE_WEB_UI=ON` and `--web-ui` is passed, `GET /metrics` exposes
engine state in Prometheus text exposition format:

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

Registered via `WebSocketWorker::set_on_metrics()`; returns HTTP 503 until
the engine is running. When the web UI is disabled, the route is not served.

### 27.3 Event log rotation (L3)

Binary event log (`--log-events`) and the text log from `LoggingWorker` can
be capped and rotated once they exceed a configurable size threshold. Off
by default.

Enable with a non-zero max size in megabytes:

```bash
./build/engine_shadow --provider local --path market_data.csv --strategy sma \
  --log-events /var/log/truetest/events.bin \
  --log-max-size 100 --log-keep 5
```

With those flags:

- As soon as a completed event would push `events.bin` past 100 MB, the
  current file is finalized (index footer written), renamed to
  `events.bin.1`, and a new `events.bin` is opened. Older copies shift:
  `events.bin.1 → events.bin.2`, etc.
- At most `--log-keep` rotated copies are retained (default 5). Excess
  oldest files are deleted.
- Rotation happens at event boundaries only — never mid-write — so replay
  tools can safely read any single rotated file.
- The same size limit and retention apply to the text log from
  `LoggingWorker`.

Leaving `--log-max-size 0` preserves unbounded behavior — fine for short
backtests but not for long-running live/shadow sessions.

### 27.4 Debug instrumentation (opt-in)

Built with `-DENABLE_DEBUG=ON`. Enables stage timers, memory info, hardware
info, copy tracker, and a consolidated debug report. Uses Abseil logging.
Writes `truetest_debug.log` alongside the default output.

---

## 28. Error Handling & Resilience

### 28.1 Graceful worker recovery (N1)

Worker threads no longer halt the engine on the first exception. When
`on_event()` throws, the error is caught, logged via the structured logger,
and the consecutive error counter increments. The worker continues
processing the next event. Only when `max_consecutive_worker_errors`
consecutive failures occur without a successful event in between does the
worker set the halt flag and stop.

Default: 5 consecutive errors. Configure via `engine_config`:

```cpp
engine_config cfg;
cfg.max_consecutive_worker_errors = 10;  // more tolerant
```

A successful `on_event()` resets the counter. Total errors via
`Worker::error_count()`. Each worker exposes `worker_name()` used in log
messages (e.g. `[WARN] [logging] on_event exception ...`).

### 28.2 WebSocket input sanitization (N2)

All incoming WebSocket commands are sanitized at the system boundary:

- **Message length**: messages exceeding 4 KB are rejected with an error
  response and logged.
- **Null bytes**: messages containing `\0` are rejected.
- **Control characters**: string fields (command, side, type, timeframe,
  value) are checked for ASCII control characters (< 0x20, except tab).
  Rejected.
- **Numeric validation**: numeric fields (quantity, price) must contain
  actual numeric data. Non-numeric values rejected with descriptive errors
  rather than silently defaulting to 0.

All rejections produce a structured error response:

```json
{"type": "error", "data": {"message": "message too large (max 4096 bytes)", "source": "ws_validator"}}
```

Rejections also logged under the `ws` component.

### 28.3 Unified connection retry (N3)

Shared retry-with-exponential-backoff utility in
`BacktestEngine/src/utils/retry.h`. All external connection points use this
rather than their own retry logic:

```cpp
#include "utils/retry.h"

// Simple form
bool ok = retry_with_backoff(
    []() { return try_connect(); },
    5,
    std::chrono::milliseconds(1000),
    std::chrono::milliseconds(30000)
);

// With config + callback
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

| Component | File | Behavior |
|---|---|---|
| Binance WebSocket | `binance_transport.h` | 5 attempts, 1s → 16s backoff |
| Binance combined stream | `binance_combined_transport.h` | 5 attempts, 1s → 16s backoff |
| Generic WebSocket source | `websocket_data_source.cpp` | 10 attempts, configurable delays |
| PostgreSQL | `pg_data_source.cpp` | 5 attempts, 1s → 16s backoff |

The callable returns `true` on success, `false` on failure. If it throws,
the exception counts as a failure and is forwarded to `on_retry`. After all
attempts are exhausted, the last exception is rethrown.

### 28.4 Binance reliability hardening (Tier 1)

Five independent hardening passes landed on `providers/binance/` to let a
live session survive long-running operation without silent stalls. Every
item is on by default; there is no CLI flag to disable.

**Clock-skew guard.** `BinanceProvider::open()` calls
`binance::verify_clock_skew()` before constructing the `ExecutionBridge` on
the live branch. Hits `/api/v3/time` unsigned and compares server vs local
clock. If drift > 2000 ms — or the fetch fails — `open()` logs the reason,
sets provider state to `lifecycle::error`, and refuses to go live. Paper
and hybrid modes unaffected. Prevents the classic `-1021 "Timestamp for
this request is outside of the recvWindow"` rejection.

Header: `providers/binance/binance_time_sync.h`. Pure decision helper
`verify_clock_skew_offset(offset_ms, tolerance_ms)` unit-tested in
`tests/test_binance_rest_client_time.cpp`.

**Proactive rate-limit backoff.** `BinanceRestClient` tracks
`X-MBX-USED-WEIGHT-1M` across requests. Before each `execute()`, a pure
function `throttle_delay_ms(used, anchor, now, cap, pct)` returns a sleep
duration if the 1-minute window is still active and weight use is at or
above 80% of the 6000 cap. On `HTTP 429`, the client sleeps (honoring
`Retry-After`, clamped to 60 s) and retries once. Soft threshold and cap
are compile-time defaults but overridable via `set_weight_cap()` /
`set_soft_threshold_pct()` (not yet wired through `engine_config`).

Tests: `tests/test_binance_rest_client_rate_limit.cpp`.

**listenKey keepalive hardening.** `BinanceUserDataTransport`'s keepalive
loop takes a `binance_keepalive_policy` (30 min interval, 15 s retry delay,
3 retries by default). Per-tick decision lives in
`binance_keepalive_detail::keepalive_tick`, a pure templated helper:

- `PUT /api/v3/userDataStream?listenKey=...` succeeds → stay `open`.
- All PUT retries fail → `POST /api/v3/userDataStream` to rotate the key.
  A rotated key is swapped in under a mutex. State → `degraded`.
- Rotation also fails → state → `error`, reader thread told to stop. No
  silent loss of the user-data stream.

Cancellable at every sleep via the existing `cv_` / `stop_flag_` pair.
Tests: `tests/test_binance_user_data_transport_keepalive.cpp` drive the
pure tick function with injected callables — no network required.

**User-data WebSocket reconnect.** Transient WS read errors no longer pin
the stream to `lifecycle::error` forever. The socket loop runs inside a
retry envelope:

- `run_once()` returns `stopped`, `network_error`, or `handshake_error`.
- `run()` reconnects up to 10 attempts with exponential backoff
  (1 s → 30 s), cancellable via the stop signal.
- If the prior session was open for >5 minutes, the reconnect counter
  resets — a long-lived stream that hiccups isn't treated as flapping.
- Before reconnecting, if the previous session lasted longer than the
  keepalive interval, the transport best-effort refreshes the listenKey
  (PUT, fallback to POST).
- Status notes include the current attempt (`"reconnecting user-data stream
  (attempt 3/10)"`).

Loop-control decision isolated in
`BinanceUserDataTransport::decide_next(state, last_result, max_attempts,
now_ms, reset_threshold_ms)`, exercised in
`tests/test_binance_user_data_transport_reconnect.cpp`.

**ExecutionBridge concurrency audit.** Every non-const, non-injected data
member is explicitly documented as guarded by a specific mutex:

| Member | Mutex |
|---|---|
| `by_engine_id_`, `by_client_id_` | `map_mu_` |
| `pending_fills_`, `next_fill_id_` | `fills_mu_` |
| `pending_status_` | `status_mu_` |
| `last_error_` | `error_mu_` |

No method holds two of these locks at once. A regression test
`ExecutionBridge.ConcurrentFillIngestAndPoll` drives `handle_message` from
4 feeder threads (1000 messages total) against a concurrent `poll_fills`
loop and asserts zero loss / zero duplicate fill IDs. Suitable for
`-DENABLE_TSAN=ON`.

---

## 29. start.sh Launcher

`start.sh` wraps cmake configure + build + engine launch + frontend
serving. Edit the configuration section at the top, then run `./start.sh`.

### Script modes

| `MODE` | Behavior |
|---|---|
| `full` | Build engine + build frontend + start engine + serve static files |
| `dev` | Build engine + start engine + Vite dev server (hot reload) |
| `build-only` | Build engine + build frontend, do not start |
| `no-build` | Skip build, start engine + serve directly |

The script auto-bootstraps vcpkg when optional features are enabled,
cloning it into `build/_vcpkg/`.

```bash
chmod +x start.sh
./start.sh
```

---

## 30. Testing

Three layers of automated tests, all built by a single CMake flag:

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target truetest_tests
./build/truetest_tests
```

### 30.1 Unit tests

Per-component correctness tests: orderbook matching, ring buffer semantics,
indicators (SMA/EMA/RSI/Bollinger), portfolio accounting, strategies,
object pool, risk manager, etc. Live in `tests/test_*.cpp` — the majority
of the suite.

Filter by suite or name:

```bash
./build/truetest_tests --gtest_filter='Orderbook.*'
./build/truetest_tests --gtest_filter='*Sharpe*'
```

### 30.2 Integration tests — `tests/test_engine_integration.cpp`

End-to-end tests that exercise the full event pipeline
(`market_event → strategy → order → orderbook → fill → portfolio`) by
calling `engine::run()` on synthetic data and inspecting the resulting
`portfolio` / `Analytics` state. Construct a real `engine_config`
programmatically — no mocking.

Covered scenarios:

| Test | What it verifies |
|---|---|
| `SmaPipelineIsDeterministicWithFixedSeed` | Two runs with the same seed produce bit-identical reports |
| `OrderProducesFillAndUpdatesAnalytics` | Strategy-emitted orders reach the orderbook and produce fills; portfolio/analytics update |
| `RiskDrawdownLimitHaltsEngine` | `risk_limits.max_drawdown` breach halts the run before data is exhausted |

All three rely on `engine_config.seed` for determinism and
`thread_preset::inline_mode` to avoid cross-thread non-determinism.

### 30.3 Golden-file regression — `tests/test_golden_regression.cpp`

Runs a deterministic backtest against a checked-in fixture CSV and diffs
the metrics against a committed golden JSON file. Any code change that
alters backtest outputs fails this test, surfacing unintended drift.

Fixture layout under `tests/golden/`:

| File | Purpose |
|---|---|
| `sma_basic.csv` | 30 OHLCV bars for symbol `GOLD`, oscillating ~$100–106 |
| `sma_basic_config.json` | Human-readable record of the engine configuration |
| `sma_basic_expected.json` | Golden metrics — compared on every test run |

Metrics compared (`EXPECT_NEAR` `1e-6` for floats, `EXPECT_EQ` for counts):
`final_equity`, `cumulative_return`, `max_drawdown`, `sharpe_ratio`,
`sortino_ratio`, `win_rate`, `profit_factor`, `buy_and_hold_return`,
`total_trades`, `total_orders`, `total_fills`.

**Regenerating after an intentional behavior change:**

```bash
TRUETEST_REGENERATE_GOLDEN=1 ./build/truetest_tests \
  --gtest_filter='GoldenRegression.*'
git diff tests/golden/sma_basic_expected.json   # review the delta
git add tests/golden/sma_basic_expected.json
```

Before each assertion run, the test runs the same pipeline twice and
asserts byte-identical outputs — a prerequisite for meaningful golden
comparison. If that pre-check fails, non-determinism has crept in and the
golden comparison is disabled until fixed.

---

## 31. Performance Benchmarks

Google Benchmark suite for the hot-path components. Opt-in via
`-DENABLE_BENCHMARKS=ON`. Builds a separate `truetest_benchmarks` binary
that does not ship in release packaging.

```bash
cmake -B build -DENABLE_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target truetest_benchmarks
./build/truetest_benchmarks
./build/truetest_benchmarks --benchmark_filter=Orderbook
./build/truetest_benchmarks --benchmark_format=json > bench.json
./build/truetest_benchmarks --benchmark_min_time=1s
```

Benchmarks (`benchmarks/bench_main.cpp`):

| Benchmark | What it measures |
|---|---|
| `BM_Orderbook_InsertCancel` | Non-crossing insert + cancel round trip |
| `BM_Orderbook_Match` | Aggressive crossing order against pre-seeded ask ladder |
| `BM_RingBuffer_PushPop` | SPSC `RingBuffer` single-thread push+pop cycle |
| `BM_EventJson_Market` | `market_event` → JSON (`snprintf` hot path) |
| `BM_EventJson_Fill` | `fill_event` → JSON |
| `BM_SMA_Update` | `simple_moving_average::update()` per tick |
| `BM_Engine_Throughput_100k` | End-to-end engine run over 100,000 synthetic bars (bars/sec) |

The full-engine benchmark uses `thread_preset::inline_mode` so the number
reflects single-threaded hot-path throughput; enable a worker preset in
`engine_config` to measure multithreaded scaling.

Always build with `-DCMAKE_BUILD_TYPE=Release` for meaningful numbers —
Debug is ~100× slower. If Google Benchmark warns about CPU scaling, pin
frequency with `cpupower frequency-set -g performance` before capturing
baselines.

---

## 32. Embedding: C API and Python Bindings

TrueTest ships a stable `extern "C"` API and a ctypes-based Python wrapper
so the engine can be driven from host languages without linking the full
C++ interface. Native surface:
`BacktestEngine/src/api/truetest_api.h`. Python module: `python/truetest.py`.

### 32.1 Building `libtruetest`

```bash
cmake -B build -DBUILD_SHARED_LIB=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target truetest_shared
# produces build/libtruetest.so (linux), build/libtruetest.dylib (macOS),
# or build/truetest.dll (windows).
```

`BUILD_SHARED_LIB` is independent of the CLI targets. The shared library
uses the same production sources as the engine binaries, plus
`BacktestEngine/src/api/truetest_api.cpp`. Symbol visibility defaults to
hidden; only `tt_*` functions are exported.

Installing via `cmake --install build` drops the library into `lib/` and
the public header into `include/truetest/truetest_api.h`.

### 32.2 C API reference

All functions use `extern "C"` linkage and opaque handles. Errors are
signaled via `NULL` return values or non-zero status codes; the last
message is available through `tt_last_error()` (thread-local).

| Function | Returns | Notes |
|---|---|---|
| `tt_version()` | `const char*` — library version (`"0.1.0"`) | Always non-null |
| `tt_create_engine(config_json)` | `tt_engine_handle` or `NULL` | `config_json` is a UTF-8 JSON string |
| `tt_run(handle)` | `0` on success, non-zero on error | Synchronous; blocks until done |
| `tt_get_results(handle)` | Heap `const char*` JSON or `NULL` | Must be freed with `tt_free_string` |
| `tt_free_string(str)` | void | `NULL`-safe |
| `tt_destroy(handle)` | void | `NULL`-safe |
| `tt_last_error()` | Thread-local `const char*` (possibly empty) | Valid until next API call |

**Config JSON schema** (fields optional unless noted):

| Field | Type | Default | Purpose |
|---|---|---|---|
| `data_path` | string | — (**required**) | Path to OHLCV CSV (consumed by `CsvDataSource`) |
| `strategy` | string | `"mean-reversion"` | Registered strategy name |
| `initial_balance` | number | `10000.0` | Starting cash |
| `seed` | uint | `0` | RNG seed for deterministic replays |
| `rolling_window` | uint | `252` | Rolling Sharpe/drawdown window |
| `risk_free_rate` | number | `0.0` | Annualized risk-free rate |
| `market_aggression` | number | `1.1` | Market order price multiplier |
| `qty_scale` | number | `1e8` | Fractional-quantity scale factor |
| `fill_rng_seed` | uint | `42` | Fill model RNG seed |
| `spread_step_factor` | number | `0.0001` | Spread step (fraction of mid) |
| `db_path` | string | `""` | Optional SQLite persistence path |
| `event_log_path` | string | `""` | Optional binary event log path |
| `params` | object | `{}` | `{key: number}` pairs forwarded to `strategy.set_param()` |

**Results JSON** — the object returned from `tt_get_results()` contains the
same headline metrics as `Analytics::export_json` plus `equity_curve`,
`per_symbol`, and `per_strategy` breakdowns.

### 32.3 Minimal C usage

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

### 32.4 Python bindings

The Python module is a zero-dependency ctypes wrapper — no numpy/pandas —
so it installs cleanly on minimal targets.

```bash
cmake -B build -DBUILD_SHARED_LIB=ON
cmake --build build --target truetest_shared
pip install -e python/
```

Library-discovery order in `python/truetest.py`:

1. The `TRUETEST_LIB` environment variable (absolute-path override).
2. Alongside the module (for wheels that bundle the `.so`).
3. `<repo>/build/libtruetest.so` (development-checkout default).
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

`Engine` supports the context-manager protocol:
`with Engine(cfg) as eng: eng.run()` guarantees the native handle is
released even on exceptions. `Engine.close()` is idempotent.

`TrueTestError` wraps any non-zero C API status — malformed config, missing
CSV, unknown strategy, runtime exceptions in `engine::run()`. The message
includes `tt_last_error()` for diagnostics.

A runnable example lives at `python/example.py`.

### 32.5 Limitations

The C API intentionally covers the batch-backtest use case only. Live
streaming, WebSocket UI, provider-based execution, replay, and multi-
symbol runs are driven through the CLI binaries — callers that need those
features today should shell out to `engine_backtest` / `engine_shadow` /
`engine_live` rather than the shared library. The API surface is
deliberately small so it can stay stable while the internal engine evolves.

---

## 33. CI Pipeline

Defined in `.github/workflows/ci.yml`. All jobs use ccache and shared
FetchContent caching.

| Job | Compiler | Type | Extra flags | Purpose |
|---|---|---|---|---|
| `build` (matrix) | gcc-13, clang-17 | Debug, Release | `BUILD_TESTS=ON ENABLE_SQLITE=ON` | Core build + test across compilers and configs |
| `asan` | gcc-13 | Debug | `ENABLE_ASAN=ON ENABLE_UBSAN=ON` | Memory safety + undefined behavior |
| `binance` | gcc-13 | Release | `ENABLE_BINANCE=ON` | Binance provider compilation + tests |
| `postgresql` | gcc-13 | Release | `ENABLE_POSTGRESQL=ON` | PostgreSQL backend compilation + tests |
| `format` | clang-format-17 | — | — | `clang-format --dry-run --Werror` on all sources |
| `tidy` | clang-tidy-17 | Debug | `CMAKE_EXPORT_COMPILE_COMMANDS=ON` | Static analysis, warnings-as-errors |
| `benchmarks` | gcc-13 | Release | `ENABLE_BENCHMARKS=ON` | Build + smoke-run benchmarks |
| `layer-deps` | — | — | — | `scripts/check-layer-deps.sh` — enforces layered dependency graph |
| `credentials-check` | — | — | — | `scripts/check-credentials.sh` — no secrets in source |
| `hotpath-json-check` | — | — | — | `scripts/check-hotpath-json.sh` — nlohmann/json stays off hot path |

TSAN runs on a nightly cron schedule, not per-PR.

---

## 34. Examples

### 34.1 Minimal backtest from CSV

```bash
cmake -B build
cmake --build build
./build/engine_backtest --provider local --path market_data.csv \
  --strategy sma --sma-period 30
```

### 34.2 Backtest with tick data and fixed fees

```bash
./build/engine_backtest --provider local --path ticks.csv --format tick \
  --strategy mean-reversion \
  --fee fixed --fee-value 0.50 --balance 50000 --output results.json
```

### 34.3 Multi-file multi-symbol backtest

```bash
./build/engine_backtest --provider local \
  --path "btc.csv,eth.csv,sol.csv" --strategy ma-crossover
```

### 34.4 Multi-strategy run

```bash
./build/engine_backtest --provider local --path market_data.csv \
  --strategy sma,mean-reversion,ma-crossover
```

### 34.5 Deterministic backtest with fixed seed

```bash
./build/engine_backtest --provider local --path market_data.csv \
  --strategy mean-reversion \
  --seed 12345 --sma-period 20 \
  --risk-fraction 0.02 --sl 0.005 --tp 0.01
```

Running twice with the same `--seed` produces identical results.

### 34.6 Backtest with PostgreSQL data source

```bash
cmake -B build -DENABLE_POSTGRESQL=ON
cmake --build build
./build/engine_backtest   # TUI mode — select PostgreSQL interactively
```

### 34.7 Backtest with SQLite persistence disabled

```bash
cmake -B build -DENABLE_SQLITE=OFF
cmake --build build
./build/engine_backtest --provider local --path market_data.csv \
  --strategy sma --no-db
```

### 34.8 Binance paper trading with WebSocket UI

```bash
cmake -B build -DENABLE_BINANCE=ON -DENABLE_WEB_UI=ON
cmake --build build
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --web-ui --ws-port 8765 --strategy mean-reversion --backfill 1000
```

### 34.9 Binance kline streaming with tiered fees

```bash
./build/engine_shadow --provider binance --symbol ethusdt --stream kline_1m \
  --fee tiered --maker-rate 0.001 --taker-rate 0.001 --balance 100000
```

### 34.10 Binance testnet live execution

```bash
./build/engine_live --provider binance --symbol btcusdt --stream trade \
  --testnet --live --api-key "$KEY" --api-secret "$SECRET" \
  --strategy sma --sma-period 14 --balance 10000
```

### 34.11 Binance mainnet live execution

```bash
./build/engine_live --provider binance --symbol btcusdt --stream kline_1m \
  --live --api-key "$KEY" --api-secret "$SECRET" \
  --strategy mean-reversion --balance 50000 \
  --risk-fraction 0.01 --sl 0.003 --tp 0.008 \
  --max-daily-loss 1000 --max-trades-per-hour 50 --risk-unwind
# Prompts "Type YES to continue" before placing real orders.
```

### 34.12 Record and replay Binance data

```bash
# Record
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --record btc_session.bin

# Replay
./build/engine_backtest --provider binance --symbol btcusdt --stream trade \
  --replay-data btc_session.bin --strategy sma
```

### 34.13 Binary event log: write, then replay

```bash
./build/engine_backtest --provider local --path market_data.csv \
  --strategy sma --log-events events.bin

./build/engine_backtest --replay events.bin
```

### 34.14 Replay a time slice from an event log

```bash
./build/engine_backtest --replay events.bin \
  --replay-from 1700000000000000 --replay-to 1700003600000000
```

### 34.15 Portfolio checkpointing and resume

```bash
# Run with checkpointing
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --checkpoint portfolio.ckpt --checkpoint-interval 5000

# Resume from checkpoint
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --resume portfolio.ckpt
```

### 34.16 JSON config file with CLI override

```bash
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
./build/engine_backtest --config backtest.json

# Override from CLI
./build/engine_backtest --config backtest.json --balance 100000 --strategy mean-reversion
```

### 34.17 Dry run and dump config

```bash
./build/engine_backtest --config backtest.json --dry-run
./build/engine_backtest --config backtest.json --balance 100000 --dump-config
```

### 34.18 Full-featured Release build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_BINANCE=ON -DENABLE_WEB_UI=ON -DENABLE_SQLITE=ON \
  -DENABLE_NATIVE_OPT=ON -DBUILD_TESTS=ON -DENABLE_BENCHMARKS=ON
cmake --build build -j$(nproc)
```

### 34.19 Debug build with ASAN + UBSAN

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ASAN_OPTIONS="halt_on_error=1:detect_leaks=0" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
  ctest --test-dir build --output-on-failure
```

### 34.20 Debug build with TSAN

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TSAN=ON -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### 34.21 Build with debug instrumentation (Abseil)

```bash
cmake -B build -DENABLE_DEBUG=ON
cmake --build build
./build/engine_backtest --provider local --path market_data.csv --strategy sma
# Writes truetest_debug.log with stage timers, memory, and hardware info.
```

### 34.22 Build shared library for embedding

```bash
cmake -B build -DBUILD_SHARED_LIB=ON
cmake --build build
ls build/libtruetest.so
# Use from Python: ctypes.CDLL("build/libtruetest.so")
```

### 34.23 Run benchmarks

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_BENCHMARKS=ON
cmake --build build --target truetest_benchmarks
./build/truetest_benchmarks --benchmark_min_time=1s
```

### 34.24 Threading preset override

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --thread-preset extended --spin-policy spin --seed 42
```

### 34.25 Disable CPU pinning (containers)

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --no-pin
```

### 34.26 Export results to CSV

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --output results.csv --output-format csv
# Produces results.csv (equity) + results_trades.csv (trade log)
```

### 34.27 Custom risk limits

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --balance 100000 --risk-fraction 0.01 --sl 0.002 --tp 0.005 \
  --max-daily-loss 2000 --daily-reset-hour 8 \
  --max-trades-per-hour 120 --max-orders-per-minute 30 --risk-unwind
```

### 34.28 Log rotation

```bash
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --log-events events.bin --log-file engine.log \
  --log-max-size 100 --log-keep 10
# Rotates event and text logs at 100 MB, keeps last 10 files.
```

### 34.29 WebSocket UI with custom port and compression disabled

```bash
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --web-ui --ws-port 9000 --no-ws-compress
```

### 34.30 CMake preset — Linux

```bash
cmake --preset linux-default
cmake --build out/build/linux-default
```

### 34.31 CMake preset — Windows Ninja

```cmd
cmake --preset windows-ninja
cmake --build out/build/windows-ninja
```

### 34.32 CMake preset — Windows Visual Studio

```cmd
cmake --preset windows-vs-2022
cmake --build out/build/windows-vs-2022 --config Release
```

### 34.33 start.sh dev mode

```bash
chmod +x start.sh
./start.sh
# Builds engine + starts Vite dev server with hot reload.
# Edit the CONFIGURATION section at the top of start.sh to change
# provider, strategy, etc.
```

### 34.34 Install via CPack

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && cpack -G TGZ
# Produces truetest-0.1.0-Linux.tar.gz with bin/ containing all three engine binaries.
```

### 34.35 Install to prefix

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/truetest
cmake --build build
cmake --install build
# Installs engine_backtest, engine_shadow, engine_live to /opt/truetest/bin/
# Installs web/index.html to /opt/truetest/share/truetest/web/
# If BUILD_SHARED_LIB=ON: libtruetest.so to lib/, truetest_api.h to include/truetest/
```

### 34.36 Production-style live run

```bash
./build/engine_live \
  --provider binance --symbol btcusdt --stream kline_1m \
  --live --api-key "$BINANCE_API_KEY" --api-secret "$BINANCE_API_SECRET" \
  --strategy sma --sma-period 50 --balance 1000 \
  --max-daily-loss 50 --max-trades-per-hour 20 --risk-unwind \
  --checkpoint /var/lib/truetest/live.ckpt \
  --log-events /var/log/truetest/events.bin \
  --log-max-size 100 --log-keep 10 \
  --output /var/log/truetest/results.json \
  --web-ui --ws-port 8765
```

Combines tight risk caps with automatic unwind, periodic checkpointing,
rotated event logging, and a JSON analytics export — the shape of a run you
would actually leave unattended.
