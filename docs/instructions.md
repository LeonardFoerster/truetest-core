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
23. [QuestDB Persistence](#23-questdb-persistence)
24. [Event Pipeline](#24-event-pipeline)
25. [Checkpointing & Determinism](#25-checkpointing--determinism)
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

**Optional system packages** (only needed when the corresponding `ENABLE_*`
flag is ON):

| Package | Install (Debian/Ubuntu) | Required by |
|---|---|---|
| `libboost-dev`, `libboost-system-dev` | `sudo apt install libboost-all-dev` | `ENABLE_BINANCE`, `ENABLE_LIVE_DATA` |
| `libssl-dev` (OpenSSL) | `sudo apt install libssl-dev` | `ENABLE_BINANCE` |
| ncurses (`libncurses-dev`) | `sudo apt install libncurses-dev` | Rich tabbed dashboard on `engine_shadow` / `engine_live` |
| Abseil | auto-fetched via CMake | `ENABLE_DEBUG` |
| GoogleTest | auto-fetched via CMake | `BUILD_TESTS` |
| Google Benchmark | auto-fetched via CMake | `ENABLE_BENCHMARKS` |

QuestDB itself is a separate runtime daemon (Docker recipe in
[§23](#23-questdb-persistence)); the engine's QuestDB client uses raw POSIX
sockets, no system package needed at build time.

---

## 2. Building from Source

### Minimal build (no external dependencies)

```bash
cmake -B build
cmake --build build
```

Produces the three engine binaries (`engine_backtest`, `engine_shadow`,
`engine_live`) with CSV data sources and the core event pipeline. No
external libraries required.

### Full-featured build

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_BINANCE=ON \
  -DENABLE_LIVE_DATA=ON \
  -DENABLE_QUESTDB=ON \
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
| `ENABLE_QUESTDB` | OFF | `HAS_QUESTDB` | none (raw POSIX sockets) | QuestDB persistence backend: per-run capture of every order-lifecycle event via ILP/TCP + HTTP DDL. Activated at runtime by `--persist`. See [§23](#23-questdb-persistence) |
| `ENABLE_BINANCE` | OFF | `HAS_BINANCE` | Boost headers, OpenSSL | Binance spot exchange provider: live WebSocket streaming, REST execution, HMAC-SHA256 signing, historical kline backfill, spot testnet support |
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
| `ENABLE_NATIVE_OPT` | OFF | Applies `-march=native -mtune=native -funroll-loops -fomit-frame-pointer` to **all three binaries** (backtest, shadow, live), Release config only. Opt-in because CI and portable builds must stay CPU-agnostic; when set, the user has already accepted native-only builds across the board |

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
./build/engine_shadow --provider binance --symbol btcusdt --stream trade

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
| `--live` | flag | off | Safety flag required for live (real money) execution. On mainnet, also triggers an interactive math-challenge confirmation before any order is sent (skipped with `--testnet`) |
| `--dry-run` | flag | off | Validate config, print summary, exit |
| `--dump-config` | flag | off | Print resolved config as JSON and exit |
| `--config` | path | none | Load configuration from a JSON file. CLI flags override file values |

### Data provider

| Flag | Values | Default | Description |
|---|---|---|---|
| `--provider` | `local`, `binance`, `binance-futures` | none (TUI mode) | Data provider. Omit for interactive TUI |
| `--path` | file path | none | CSV file path for `local` provider. Accepts comma-separated paths for multi-symbol |
| `--format` | `bar`, `tick` | auto-detected | Data format. Auto-detected for Binance based on `--stream` |
| `--symbol` | string | none | Trading symbol (e.g. `btcusdt`) |
| `--stream` | `trade`, `kline_1m`, `kline_5m`, `depth`, etc. | none | Stream type for Binance provider |
| `--host` | hostname | exchange default | WebSocket host override |
| `--port` | number | exchange default | Port override |
| `--testnet` | flag | off | Route to the testnet stack — spot (`stream.testnet.binance.vision`) for `binance`, USDT-M futures (`stream.binancefuture.com`) for `binance-futures` |
| `--margin-type` | `ISOLATED`, `CROSSED` | empty (no check) | (`binance-futures` only) Expected margin type for advisory comparison. Empty disables the check |
| `--margin-type-strict` | flag | off | (`binance-futures` only) Escalate margin-mode mismatch from advisory (warning) to refusal. Has no effect unless `--margin-type` is also set |
| `--liquidation-warn-pct` | float | 0.05 | (`binance-futures` only) Warn at startup if any open position is within this fraction of liquidation. Set to 0 to disable |
| `--max-notional` | float (USDT) | 0 (disabled) | (`binance-futures` only) Per-order notional cap: refuses if `\|post_qty\| × mark` exceeds this |
| `--max-leverage` | float | 0 (disabled) | (`binance-futures` only) Per-order leverage cap: refuses if `post_notional / cash` exceeds this |
| `--min-liq-distance-pct` | float | 0 (disabled) | (`binance-futures` only) Per-order minimum projected buffer to liquidation. 0.05 = 5%. See `docs/futures-testnet.md` for the approximation caveats |
| `--dead-man-countdown-ms` | long (ms) | 30000 | (`binance-futures` only) Server-side dead-man's switch via `POST /fapi/v1/countdownCancelAll`. If the engine dies between heartbeats, Binance auto-cancels open orders within this many ms of the last heartbeat. 0 disables for the run |
| `--dead-man-heartbeat-ms` | long (ms) | 0 (= countdown / 3) | (`binance-futures` only) Heartbeat interval. Smaller = tighter recovery, more API calls. Larger = fewer calls, more network-flap tolerance |
| `--disarm-deadman` | flag | off | (`binance-futures` only) Don't arm the DMS for this run. Intent-revealing alternative to `--dead-man-countdown-ms 0`; useful for debug / deploy workflows where the operator deliberately wants to pause without venue-side auto-cancel |

### Binance credentials

**Prefer environment variables.** When these are set, they override the CLI
flags. Passing secrets via argv leaks them to `ps` and shell history, and
TrueTest emits a warning if you do:

| Env var | Purpose |
|---|---|
| `TRUETEST_BINANCE_API_KEY` | Binance API key (wins over `--api-key`) |
| `TRUETEST_BINANCE_API_SECRET` | Binance API secret (wins over `--api-secret`) |

| Flag | Description |
|---|---|
| `--api-key` | Fallback when the env var is unset. Required for `--mode=live` if no env var |
| `--api-secret` | Fallback when the env var is unset. Required for `--mode=live` if no env var |

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
| `--risk-unwind` | flag | On risk halt, unwind all open positions before stopping. Handles both longs and shorts — longs close with market SELL, shorts with market BUY |
| `--reconcile-tolerance-bps` | `10.0` | Live only. Acceptable local/exchange balance drift (basis points) at startup. The Binance provider hits `/api/v3/account` before the engine starts; larger drift refuses startup |
| `--kill-switch-deadline-ms` | `5000` | Live only. Deadline (ms) for the cancel-all + flatten sequence at shutdown. A missed deadline prints a warning so the operator knows to intervene manually |

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
| `--persist` | off | Activate QuestDB writes for this session. Requires `-DENABLE_QUESTDB=ON` at build time |
| `--run-tag` | auto | Table prefix for the per-run tables. Auto: `run_<YYYYMMDD>_<HHMMSS>_<6 hex>`. User-supplied tags must match `[A-Za-z0-9_]{1,64}` |
| `--run-notes` | empty | Free-form note recorded in `runs_meta` |
| `--questdb-host` | `127.0.0.1` | Where to find the daemon |
| `--questdb-ilp-port` | `9009` | InfluxDB Line Protocol ingest port (TCP) |
| `--questdb-http-port` | `9000` | HTTP `/exec` port for DDL + queries |

See [§23](#23-questdb-persistence) for schema, write pipeline, and example queries.

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
| `--exec-bar-delay` | `1` | Bars of simulated execution delay for backtest/shadow. `1` (default) parks orders until next bar's open — kills same-bar look-ahead. `0` restores legacy same-bar fill. Ignored in live mode and when a latency model is configured |
| `--wire-latency-us` | `0` | Extra wire + exchange-ingest latency (microseconds) applied by the execution adapter on top of any engine-side latency model. Shadow: gates real trade prints matching open orders, surfacing "sim filled, exchange didn't" cases. Backtest against the Binance paper-hybrid executor: holds fills in a release buffer for this window before returning them. Ignored in live mode. See [§16.2.2](#1622-wire-latency-modeling) |
| `--instrument` | none | Per-symbol trading rules (repeatable). Format: `SYMBOL:tick=X,lot=Y,minq=Q,minn=N,maker=M,taker=T` (any field optional). Price/qty get quantized before routing; orders below `min_qty` or `min_notional` are rejected |
| `--depth-stream` | none | Optional L2 depth stream subscribed alongside `--stream` on a single combined WebSocket (provider-specific suffix; for Binance e.g. `depth20@100ms`). When set, the provider's orderbook is driven by real exchange levels and the paper market-maker is suppressed for that symbol. See [§16.2 Binance provider](#162-binance-provider) |
| `--queue-model` | `none` | Shadow-mode only. `l2-snapshot` records depth at your limit price on submit and only releases the shadow fill once real tape has consumed that queue. Requires `--depth-stream`. |
| `--maker-queue-model` | `none` | Paper/backtest only. `uniform` (recommended), `front`, or `back` cancel attribution for passive limits using real L2. Produces realistic maker fill rates and adverse selection. Requires `--depth-stream`. See [realism.md §Queue modeling](realism.md#queue-modeling). |

### Analytics & output

| Flag | Default | Description |
|---|---|---|
| `--rolling-window` | `252` | Rolling metrics window size (number of bars) |
| `--risk-free-rate` | `0.0` | Annual risk-free rate for Sharpe/Sortino ratios |
| `--periods-per-year` | `252` | Annualization factor for Sharpe / Sortino / Calmar. Common values: `252` daily, `252×390` US-equities minute, `525600` crypto minute, `31536000` crypto second. Sharpe and Sortino are now multiplied by `sqrt(periods_per_year)`, so this must match your bar cadence |
| `--max-equity-points` | `100000` | Hard cap on retained equity-curve points. Exceeded curves are decimated in place (every second point dropped, stride doubled) to keep memory bounded on long runs |
| `--output` | none (stdout) | Write results to file |
| `--output-format` | `json` | Output format: `json` or `csv`. JSON now includes `annualized_return` alongside `cumulative_return` |

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

Two TUIs ship in the tree:

- **Setup menu** (`engine_backtest` only) — without `--provider` or
  `--replay`, the backtest binary presents a numbered text menu that walks
  through strategy selection, SMA period, data source (CSV or tick CSV;
  Live WebSocket if `-DENABLE_LIVE_DATA=ON`), fee model, and engine mode.
  Invalid input falls back to the default (typically option 1).
- **Rich tabbed dashboard** (`engine_shadow`, `engine_live`) — when the
  resolved status mode is TUI (default unless `--no-tui` /
  `--status-format=plain` / `--status-format=ndjson`), shadow and live
  binaries take over the terminal with an ncurses tabbed dashboard:
  positions, lots, brackets, fills, debug counters. Operator hotkeys:
  pause/resume, flatten, kill-switch (live only). Implementation in
  `src/ui/tabbed_dashboard.{h,cpp}` + `src/ui/panels/`.

| Flag | Effect |
|---|---|
| `--status-format auto` (default) | TUI when stdout is a tty, plain text otherwise |
| `--status-format tui` | Force ncurses TUI |
| `--status-format plain` | Line-buffered ANSI updates |
| `--status-format ndjson` | One JSON event per line — pipe-friendly |
| `--status-format off` | No status output |
| `--no-tui` | Shortcut for `--status-format=plain` |

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
  --strategy mean-reversion

# Kline stream with backfill and tiered fees
./build/engine_shadow \
  --provider binance --symbol ethusdt --stream kline_1m \
  --fee tiered --maker-rate 0.001 --taker-rate 0.001 \
  --backfill 500
```

**Supported streams (for `--stream`):**

- `trade` — individual trade ticks
- `kline_1m`, `kline_5m`, ... — 1-minute / 5-minute / etc. candlestick
  aggregation

**Execution modes:**

- **Paper** (default, backtest only) — `HybridExecutor`: paper market
  orders + local-book limit fills against a synthetic MM-seeded book.
  When `--wire-latency-us` is set, each fill's release is deferred by
  the sampled latency — the engine doesn't see the fill until
  simulated time has advanced past `fill_ts + latency` (see
  [§16.2.2](#1622-wire-latency-modeling)).
- **Shadow** — primary fills come from `LocalBookAdapter` against the
  engine's orderbook. The *exchange* side of `ShadowTracker` is
  `TradeTapeShadowAdapter`: every real trade print is matched against
  open orders, and a BUY-limit `P` fills when a trade prints at `price
  ≤ P` after submit (SELL symmetric). Fill price = the printed trade
  price. No order ever crosses the wire. Matching is gated by the
  order's `earliest_eligible_ts` plus an optional `--wire-latency-us`
  window — see [§16.2.1](#1621-l2-depth-feeding-the-orderbook-shadow-realism)
  and [§16.2.2](#1622-wire-latency-modeling).
- **Live** — signed REST order submission against `/api/v3/order` via
  `BinanceRestClient`. Order acks + fills come from Binance's user-data
  WebSocket. Cancel and modify are wired. Requires `--live` flag,
  credentials (env vars or CLI), and explicit `YES` confirmation at the
  prompt.

**Live-safety surfaces** (live mode only, all provider-owned):

- **Reconciler** — hits `/api/v3/account` at startup and compares local
  cash + position to the exchange's free+locked balance for the quote
  and base assets. Larger drift than `--reconcile-tolerance-bps`
  refuses startup. Heuristic symbol→base/quote split handles standard
  Binance suffixes (`USDT`, `USDC`, `BUSD`, `BTC`, `ETH`, …).
- **Kill-switch** — on shutdown: `DELETE /api/v3/openOrders?symbol=X`,
  then query the account and market-SELL any remaining free base-asset
  balance. Honors `--kill-switch-deadline-ms`; warns when deadline is
  missed so the operator knows to intervene.
- **Client-order-id minter** — every submitted order gets a unique id
  of form `tt-<epoch_hex>-<seed_hex>-<seq_hex>`, making reconnect
  replays idempotent against exchange-side dedup.
- **Order-rate limiter** — token bucket sized to Binance spot's
  50-orders-per-10s cap, gating every `submit_order` / `cancel_order`
  on the bridge.
- **Clock sync** — `BinanceRestClient` caches the offset from
  `/api/v3/time` and applies it to every signed timestamp. The offset
  refreshes lazily every 5 minutes; a `-1021` response triggers an
  immediate resync + one retry so a drifted NTP step doesn't kill the
  whole session.

**Historical backfill.** Historical bars are fetched via REST and injected
into the live stream through `PrependTransport` — invisible to the engine.

#### 16.2.1 L2 depth feeding the orderbook (shadow realism)

Without `--depth-stream`, the engine's orderbook for the symbol is seeded
by `MarketMaker::replenish` — paper liquidity around the mid price.
`LocalBookAdapter` fills against that paper book, which is useful for
rough shadow but doesn't reflect actual market depth.

With `--depth-stream`, the Binance provider subscribes to a combined
WebSocket carrying both feeds and the engine's orderbook is driven by
real exchange levels:

```bash
./build/engine_shadow \
  --provider binance --symbol btcusdt --stream trade \
  --depth-stream depth20@100ms \
  --strategy mean-reversion
```

What changes:
- One WS subscribes to `/stream?streams=btcusdt@trade/btcusdt@depth20@100ms`.
- Depth frames populate `orderbook_registry_` every 100 ms via
  `apply_l2_snapshot`; `LocalBookAdapter` now matches strategy orders
  against the **real** top-20 book.
- `MarketMaker::replenish` is automatically suppressed for any symbol
  that receives L2 frames.
- The trade stream still drives strategy events and the
  `TradeTapeShadowAdapter` as before.
- `ShadowTracker`'s report now compares two meaningful things:
  engine-sim fills against the real book vs. trade-tape fills.

**Supported depth suffixes (Binance):** `depth5@100ms`, `depth10@100ms`,
`depth20@100ms` (partial-book streams — each frame is a fresh top-N
snapshot). Diff streams (`depth@100ms` with REST seed + sequence-id
resequencing) are not yet supported.

**Provider-generic.** `--depth-stream` is a venue-agnostic CLI flag; its
value is passed opaquely through `provider_config` to the provider. Any
provider that overrides `IProvider::supports_event_stream()` and
`get_event_parser()` participates in the unified `provider::event`
streaming path — no engine or CLI changes needed per venue.

#### 16.2.2 Wire latency modeling

`--wire-latency-us N` layers an `ILatencyModel` on top of the execution
adapter, on top of any engine-side `latency_model` (which already delays
order submission via `earliest_eligible_ts`). The two model different
things and stack:

| Layer | Represents | Applied by |
|---|---|---|
| `latency_model` | strategy → order-ready (engine-side processing) | Engine deferral of `submit_order` call |
| `wire_latency_model` (`--wire-latency-us`) | order → venue (network + ingest) | Execution adapter internally |

Ignored when `--mode live` (the real exchange supplies real latency).

**Shadow mode** — `TradeTapeShadowAdapter` sets each open order's
`submit_ts = earliest_eligible_ts + wire_latency`. Any real trade
printing inside the wire window is correctly *missed* on the shadow
side — `ShadowTracker` then reports it as a "sim filled, exchange
didn't" divergence, which is precisely the cost of latency the operator
cares about.

**Backtest paper-hybrid** — `HybridExecutor` samples a per-order
latency at `submit_order`, tags each emitted fill with a `release_ts =
fill_ts + latency`, and holds it in a delayed-fills buffer. Fills only
become visible to the engine once the simulated clock (tracked from the
max `earliest_eligible_ts` seen across subsequent submits) has passed
`release_ts`. Cancelling an order inside the wire window also discards
any buffered fill for that id — prevents a "cancel won, but the fill
reappears later" artifact.

**Typical values.** Co-located HFT: `50–200 µs`. VPS in the same
region: `500 µs – 2 ms`. Residential WAN: `20–50 ms`. Set what
matches your deployment, not what looks best in the PnL.

```bash
# Shadow with 1 ms wire latency — trades printing within 1 ms of
# submit are now correctly excluded from the "exchange" side of the
# shadow comparison.
./build/engine_shadow \
  --provider binance --symbol btcusdt --stream trade \
  --depth-stream depth20@100ms \
  --strategy mean-reversion --wire-latency-us 1000
```

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
the binary default) and the `--live` safety flag are required. Before any
order is sent, the CLI prints a red `LIVE TRADING — REAL MONEY` banner and
prompts the operator to solve a fresh random addition challenge — two
integers in `[100, 9999]`. The run aborts cleanly on a wrong answer,
non-numeric input, or EOF, so a stray `Enter` cannot launch live trading.
The challenge is skipped only when `--testnet` is set. Orders are signed
with HMAC-SHA256 and submitted to `/api/v3/order`. Always pair with tight
`--max-daily-loss`, `--max-trades-per-hour`, and `--risk-unwind` so a bug
cannot drain the account.

For testnet, see [§16.5](#165-binance-spot-testnet) below or
[`docs/testnet.md`](testnet.md) for the full walkthrough.

### 16.5 Binance spot testnet

The spot testnet (`testnet.binance.vision`) is a Binance-operated sandbox:
real-time market data, real signed REST/WebSocket protocols, demo balances.
**Same code path as mainnet live** — the only differences are the
endpoints, three testnet-specific refusal gates at provider open, the
reconciler's monthly-reset tolerance, and the captcha (skipped on
`--testnet`).

```bash
export TRUETEST_BINANCE_API_KEY=<testnet-key>
export TRUETEST_BINANCE_API_SECRET=<testnet-secret>

./build/engine_live --provider binance --symbol btcusdt --stream kline_1m \
  --testnet --live \
  --strategy sma --sma-period 14 --balance 10000 \
  --max-daily-loss 200 --max-trades-per-hour 20 --risk-unwind \
  --persist --run-tag testnet_sma_smoke   # optional QuestDB capture
```

#### Testnet-only refusals at `BinanceProvider::open()`

- **Symbol existence** — unsigned `GET /api/v3/exchangeInfo?symbol=…`.
  Refuses unknown / dropped symbols (testnet's symbol set is a smaller,
  rotating subset of mainnet). Catches typos at startup, not mid-stream
  as `-1121`.
- **WAF SQL-keyword scan** — refuses if the minted `clientOrderId` prefix
  contains `OR/AND/SELECT/DROP/UNION/--` (case-insensitive). Testnet's
  WAF rejects any param containing those tokens.
- **(existing) Clock skew** — refuses if drift > 2000 ms vs `/api/v3/time`.

#### Reconciler's monthly-reset tolerance

Binance wipes testnet balances roughly monthly. The reconciler detects
the reset signature (venue ≈ 0, local > 0) and downgrades from
"refuse to start" to a `[TESTNET-RESET]` warning so subsequent fills
re-anchor local state. Mainnet reconciler unchanged.

#### Pre-flight smoke test

`tests/test_binance_testnet_live.cpp` runs an end-to-end REST round-trip
when both env vars are set:

```bash
TRUETEST_TESTNET_KEY="$TRUETEST_BINANCE_API_KEY" \
TRUETEST_TESTNET_SECRET="$TRUETEST_BINANCE_API_SECRET" \
  ./build/test_engine --gtest_filter=BinanceTestnetLive.*
```

Resyncs the clock, probes `exchangeInfo`, fetches mark price, places and
cancels a non-fillable `LIMIT BUY` at half mid. Without env vars the test
prints a one-line skip notice and passes.

#### Inspecting your run with QuestDB

If you passed `--persist`, point a browser at `http://localhost:9000/`
or query `/exec` directly. See [§23](#23-questdb-persistence) for schema.

```bash
RUN=testnet_sma_smoke
curl -G "http://127.0.0.1:9000/exec" --data-urlencode \
  "query=SELECT * FROM ${RUN}_fills ORDER BY ts DESC LIMIT 20"
curl -G "http://127.0.0.1:9000/exec" --data-urlencode \
  "query=SELECT * FROM ${RUN}_rejections"
curl -G "http://127.0.0.1:9000/exec" --data-urlencode \
  "query=SELECT LAST(final_equity), LAST(total_fills), LAST(total_rejections)
   FROM runs_meta WHERE run_tag = '${RUN}'"
```

Full operational notes (gotchas, account-reset semantics, `MIN_NOTIONAL`
threshold, futures-testnet scope, etc.) in
[`docs/testnet.md`](testnet.md).

#### 16.2.3 Maker queue modeling (paper & backtest realism)

When you run paper or backtest strategies with real L2 (`--depth-stream`),
you can enable realistic passive limit order behavior using:

```bash
--maker-queue-model uniform   # recommended
--maker-queue-model front     # optimistic (cancels always help you)
--maker-queue-model back      # pessimistic (cancels never help you)
```

**How it works:**
- `QueueAwareBookAdapter` (used by `HybridExecutor` and the pure paper path)
  tracks every one of your resting limits.
- On submit it records `size_ahead` from the current L2 level.
- Every real trade print consumes the front of the level first.
- L2 shrinkage beyond observed trades is attributed to cancels using the
  selected `IQueueModel` (Uniform is the realistic middle ground).
- Your order only fills once `size_ahead` has been fully consumed.

**Requirements:**
- Must be used together with `--depth-stream` (otherwise silently falls back
  to the legacy "always at the front" behaviour).
- Only affects paper/backtest modes. Completely ignored in `--mode live`.

**Observability:**
- The rich TUI and `ConsoleDashboard` show **live quote count** and
  **average queue position** (in basis points: 0 = front of queue, 10000 = back).
- At session end a summary is printed:
  ```
    Maker queue model:
      Live passive limits: 23
      Avg queue position:  41%
  ```

See [`docs/realism.md`](realism.md#queue-modeling) for the full conceptual
explanation and comparison with the shadow-mode `--queue-model` flag.

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

### 18.4 Hedge Demo (`hedge-demo`)

Demonstrates the multi-position / per-lot architecture: opens a long leg with
its own SL/TP bracket, then `hedge_gap` bars later opens a short leg on the
same symbol with its own independent SL/TP bracket. On a spot venue the two
legs net at the portfolio level (zero exposure), but the lot table
(`portfolio::lots_`) keeps each leg's entry price, owning strategy, and exit
bracket distinct. The two brackets in the `ExitManager` fire independently
per opener.

Parameters:

- `hedge_gap` (default `5`) — bars between the long entry and the short entry
- `notional` (default `100`) — notional size of each leg
- `sl_pct` (default `0.005`) — stop loss as fraction of entry price
- `tp_pct` (default `0.01`) — take profit as fraction of entry price

```bash
./build/engine_backtest --provider local --path market_data.csv \
  --strategy hedge-demo --param hedge_gap=10 --param notional=250
```

This strategy does **not** use the legacy `set_position_open()` boolean; it
overrides `on_fill(fill, opener_order_id)` to maintain its own per-side open
counters, and returns its SL/TP brackets from `take_pending_exit_intents()`
(the new vector form of the exit-intent hook). See §18.6 and §18.8.

### 18.5 Indicators

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

### 18.6 Strategy registry

Strategies self-register via `REGISTER_STRATEGY("name", factory)` (mirroring
the provider registry). To add a new strategy:

1. Create the class implementing `IStrategy`
2. Add `REGISTER_STRATEGY` in the `.cpp` file with a factory lambda
3. The strategy becomes available by name via `--strategy <name>`

All strategies expose their indicator values via `get_indicator_values()` for
use in analytics.

Key `IStrategy` hooks for new strategies:

- `on_market` / `on_tick` / `on_l2_update` — emit an opening `order_event`.
- `take_pending_exit_intents()` — return a `vector<exit_intent>` immediately
  after each emission; the engine stamps them with the just-submitted
  opener's `order_id` and registers them with the `ExitManager`. Multiple
  intents per opener compose for TP1/TP2/SL scale-outs. The legacy
  single-optional `take_pending_exit_intent()` still works as a fallback.
- `on_fill(fill, opener_order_id)` — called after the portfolio applies a
  fill this strategy emitted. `opener_order_id == fill.order_id` means the
  fill is an opener; otherwise it's a closer referencing the original entry.
  Strategies that manage multiple concurrent entries track their lots here.
- `set_position_open(symbol, open)` — **legacy**, default no-op. The engine
  still calls it when a symbol flips between flat and non-flat on the netted
  book, but new strategies should ignore it and track per-lot state via
  `on_fill` instead.

### 18.7 Multi-strategy runs

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
- Closing orders (including strategy-emitted exits) also carry
  `order_event::opener_order_id` so the portfolio reduces the correct lot
  and fills are routed back to the originating strategy's `on_fill`.
- SL/TP brackets are enforced per opener by the `ExitManager` (see §18.8),
  not from `set_position_open()`. Each entry's bracket is independent, so
  two concurrent entries on the same (strategy, symbol) — for example the
  long and short legs of `hedge-demo` — carry distinct stops.
- `--param key=value` applies to **all** strategies; unknown params are
  silently ignored.
- Risk checks, fees, and the orderbook are global; the risk manager sees
  combined exposure.

Multi-strategy works in batch, streaming, tick, and replay loops.

### 18.8 Per-lot exit brackets (ExitManager)

Engine-side enforcement of strategy-declared SL/TP/trailing/time exits lives
in `src/exits/exit_manager.h`. Lifecycle per intent:

1. Strategy emits an opener `order_event` and returns an `exit_intent` from
   `take_pending_exit_intents()`.
2. The engine stamps the intent with the opener's `order_id` and calls
   `ExitManager::register_pending(intent)`.
3. When the opener fills, `ExitManager::on_fill(fill, opener_order_id)`
   promotes the pending intent(s) into armed brackets using the actual
   fill price (trailing reference) and qty.
4. On every subsequent tick, `ExitManager::on_price(symbol, px, ts)` returns
   a synthetic `order_event` for each armed intent whose SL / TP / trailing
   stop / time-stop crosses. Each returned order carries the original
   `opener_order_id` so the correct lot is closed.
5. When the opener is closed by the strategy's own signal path, its bracket
   is cancelled so it can't fire on a lot that's already flat.

Intents are keyed by `opener_order_id`, not by `(strategy, symbol)`. Two
concurrent entries on the same symbol therefore carry independent brackets.
Multiple intents under one opener compose for TP1/TP2/SL scale-outs
(`exit_intent::qty_fraction` must sum to ≤ 1.0; the risk layer catches
violations).

The helpers `make_long_exit_intent(symbol, entry, qty, sl_pct, tp_pct, …)`
and `make_short_exit_intent(...)` in `src/exits/exit_intent.h` compute the
SL/TP levels with the correct sign for each side — strategies should use
these rather than re-deriving the math.

The portfolio maintains a parallel `lots_` map keyed by `opener_order_id`
alongside the netted `positions_` map. A single symbol can have multiple
concurrent open lots (e.g. a long and a short with independent brackets);
the venue still sees the netted balance, but lot bookkeeping keeps each
entry's price, opening strategy, and attribution distinct. Inspect via
`portfolio::get_lots()`, `open_lots_by_symbol()`, and
`open_lots_by_strategy()`.

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

### 19.5 WebSocket data source

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

### 19.6 Multi-symbol backtesting

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
event pipeline (ring buffers, event log). The async shadow check in the
risk worker thread remains as a secondary validation layer.

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
the engine first closes all open positions via market orders, then
halts. This prevents leaving orphaned positions on a live exchange.

**Sign-aware.** Longs close with market SELL (`|qty|` shares); shorts
close with market BUY (`|qty|` shares). Flat positions are skipped.

```bash
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --max-daily-loss 500 --max-trades-per-hour 100 --risk-unwind
```

### 21.4 Live-mode startup and shutdown gates

Separate from per-order / per-fill risk limits, live runs (Binance
provider, `--mode live` via `engine_live`) pass through two provider-owned
safety gates:

- **Reconciler (startup)** — runs before the engine accepts any bar.
  Refuses to start if local cash or position drift from the exchange's
  `/api/v3/account` balance exceeds `--reconcile-tolerance-bps`.
  Defaults to ±10 bps.
- **Kill-switch (shutdown)** — runs as workers drain. Cancels all open
  orders on the symbol, queries current holdings, market-sells free
  base-asset balance within `--kill-switch-deadline-ms`. A missed
  deadline prints `WARNING: kill-switch did NOT complete within N ms
  — inspect exchange state manually.`

Neither gate fires in backtest or shadow; the engine installs
`NoopReconciler` / `NoopKillSwitch` there.

### 21.5 Ring-buffer drop policy (shadow / live)

Worker ring buffers drop events on overflow. The default policy
(`allow`) counts drops and reports them at shutdown — fine for
backtest. In **shadow and live**, main.inc overrides to `halt_on_drop`:
a drop on any *safety-critical* ring (risk / observer / risk_stats —
the ones feeding the halt flag and shadow portfolio) sets `halt_flag_`
and emits one stderr line identifying the ring and drop count.
Non-safety rings (logging / stats / mm / ws) still drop silently.

Rationale: silently dropping a fill from the risk ring under burst
load means the risk check never fires on that fill — exactly the
moment you most need it. Halting loudly is safer than trading past
a risk limit.

### 21.6 Risk in config files

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

**Drop policy** (`engine_config::drop_policy`): `allow` by default
(backtest), `halt_on_drop` in shadow and live. A drop from a
safety-critical ring (risk, observer, risk_stats) under `halt_on_drop`
halts the engine. See [§21.5](#215-ring-buffer-drop-policy-shadow--live).

---

## 23. QuestDB Persistence

Built only when `-DENABLE_QUESTDB=ON` and activated only when the
runtime `--persist` flag is supplied. Captures every order-lifecycle
event (submission, status transition, fill, rejection, cancellation,
amendment) to a local QuestDB instance for replay, analysis, and
cross-run comparison.

### When to use it

- Capture a complete audit trail of orders / fills for a backtest,
  shadow session, or live run — independent of the engine's own log
  files.
- Compare two strategy runs by SQL JOIN on `run_tag`.
- Drive ad-hoc dashboards from the QuestDB web console
  (`http://localhost:9000/`).

### CLI flags

| Flag                     | Default       | Purpose                                                |
|--------------------------|---------------|--------------------------------------------------------|
| `--persist`              | off           | Activate QuestDB writes for this session               |
| `--run-tag <tag>`        | auto-generated| Table prefix for the per-run tables                    |
| `--run-notes <text>`     | empty         | Free-form note stored in `runs_meta`                   |
| `--questdb-host <host>`  | `127.0.0.1`   | Where to find the daemon                               |
| `--questdb-ilp-port <n>` | `9009`        | InfluxDB Line Protocol ingest port (TCP)               |
| `--questdb-http-port <n>`| `9000`        | HTTP `/exec` port for DDL                              |

Auto-generated run tags follow the pattern
`run_<YYYYMMDD>_<HHMMSS>_<6 hex chars>`. User-supplied tags must match
`[A-Za-z0-9_]{1,64}`. The flags are unknown to a binary built with
`ENABLE_QUESTDB=OFF` and CLI11 will reject them with "argument was not
expected" so an accidentally-disabled build can never silently ignore
`--persist`.

### How writes happen

Two paths land rows in the per-run tables:

1. **Synchronous capture points** in the engine — `process_order` (risk
   acceptance + risk rejection), `route_order` (instrument-filter
   rejection), the fill loop, `cancel_order`, `modify_order`,
   `unwind_positions`. These calls carry full context (`opener_order_id`,
   `strategy_name`, fill `source`, rejection category) that the ring
   path doesn't have access to.
2. **`QuestDbWorker`** drains `questdb_ring_` (fed by `publish_event`)
   on its own thread and forwards each event to the same store.

`QuestdbStore` serialises both writers behind a `std::mutex`. The two
paths therefore both write rows for the same logical event, with the
sync path producing the fully-tagged record and the worker producing a
slimmer one. Consumers that want a single canonical row should
deduplicate on `order_id` and prefer the sync row (it always carries a
non-empty `strategy_name`). Dedup on the writer side is a follow-up.

### Tables

Each run creates six per-run tables prefixed with `{run_tag}_`:
`orders`, `order_status`, `fills`, `rejections`, `cancellations`,
`amendments`. One permanent `runs_meta` table indexes all runs with the
session's mode, binary name, strategy, symbol, initial/final equity,
and counters. `runs_meta` receives **two** rows per run — one at
`begin()` and one at `end()` — so consumers should
`GROUP BY run_tag` and `LAST(ended_at)` to reconstruct the closing
state. Full DDL is in `docs/db.md` Appendix A.

### Health-check behaviour (soft warning)

If the daemon is unreachable at startup the engine prints
per-step breadcrumbs from the store + a high-level WARNING from the
engine:

```
[questdb] connect(127.0.0.1:9000) failed: Connection refused
[questdb] DDL HTTP request failed (no response)
[questdb] begin() aborted: DDL step failed
  WARNING: QuestDB unreachable at 127.0.0.1:9000 — continuing with persistence DISABLED for this session.
  Start the daemon with: questdb start
  Or re-run without --persist to suppress this warning.
```

The session continues without persistence and exits 0. Hard-fail (i.e.
"refuse to start when `--persist` is set but QuestDB is down") is a
documented future TODO.

### Example queries

```sql
-- Count fills for a specific run
SELECT COUNT(*) FROM run_20260424_120000_abc123_fills;

-- Final equity across all runs of a strategy
SELECT run_tag, LAST(final_equity) AS final
  FROM runs_meta
  WHERE strategy = 'mean-reversion' AND final_equity IS NOT NULL
  GROUP BY run_tag;

-- All rejections for the latest run
SELECT * FROM mytag_rejections ORDER BY ts DESC;
```

### Local QuestDB via Docker

```bash
docker run --rm -d --name truetest-questdb \
    -p 9000:9000 -p 9009:9009 questdb/questdb:latest

# Wait until ready
until curl -fs "http://127.0.0.1:9000/exec?query=SELECT%201" >/dev/null; do
    sleep 1
done

# Run engine with persistence
./build/engine_backtest --provider local --path market_data.csv \
    --strategy mean-reversion --persist --run-tag my_run

# Browse data: http://127.0.0.1:9000/

# Stop
docker stop truetest-questdb
```

See `docs/db.md` for the full implementation plan, schema reference, and
ILP cheatsheet.

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

## 25. Checkpointing & Determinism

Two orthogonal features that pair well with QuestDB persistence
([§23](#23-questdb-persistence)) for reproducible runs: in-process
portfolio checkpoints (resume-after-crash) and seeded RNGs (byte-identical
re-runs).

### 25.1 Portfolio checkpoints (resume-after-crash)

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

### 25.2 Deterministic replay and RNG seeding

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

### 28.2 Unified connection retry (N2)

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

The callable returns `true` on success, `false` on failure. If it throws,
the exception counts as a failure and is forwarded to `on_retry`. After all
attempts are exhausted, the last exception is rethrown.

### 28.3 Binance reliability hardening (Tier 1)

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
streaming, provider-based execution, replay, and multi-symbol runs are
driven through the CLI binaries — callers that need those features today
should shell out to `engine_backtest` / `engine_shadow` / `engine_live`
rather than the shared library. The API surface is deliberately small so
it can stay stable while the internal engine evolves.

---

## 33. CI Pipeline

Defined in `.github/workflows/ci.yml`. All jobs use ccache and shared
FetchContent caching.

| Job | Compiler | Type | Extra flags | Purpose |
|---|---|---|---|---|
| `build` (matrix) | gcc-13, clang-17 | Debug, Release | `BUILD_TESTS=ON` | Core build + test across compilers and configs |
| `asan` | gcc-13 | Debug | `ENABLE_ASAN=ON ENABLE_UBSAN=ON` | Memory safety + undefined behavior |
| `binance` | gcc-13 | Release | `ENABLE_BINANCE=ON` | Binance provider compilation + tests |
| `questdb` | gcc-13 | Release | `ENABLE_QUESTDB=ON` | QuestDB persistence backend compilation + tests |
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

### 34.6 Backtest with QuestDB persistence

```bash
cmake -B build -DENABLE_QUESTDB=ON
cmake --build build

# Start a local QuestDB (one-time, see §23 for Docker recipe)
docker run --rm -d --name truetest-questdb \
  -p 9000:9000 -p 9009:9009 questdb/questdb:latest

./build/engine_backtest --provider local --path market_data.csv \
  --strategy sma --persist --run-tag sma_smoke --run-notes "first try"

# Browse: http://127.0.0.1:9000/
```

### 34.7 Binance paper trading

```bash
cmake -B build -DENABLE_BINANCE=ON
cmake --build build
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --strategy mean-reversion --backfill 1000
```

### 34.8 Binance kline streaming with tiered fees

```bash
./build/engine_shadow --provider binance --symbol ethusdt --stream kline_1m \
  --fee tiered --maker-rate 0.001 --taker-rate 0.001 --balance 100000
```

### 34.9 Binance testnet live execution

Same code path as mainnet live; the captcha is skipped because `--testnet`
is set. See [`docs/testnet.md`](testnet.md) for the full walkthrough,
account setup, refusal gates, and gotchas.

```bash
cmake -B build -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON
cmake --build build

# (one-time) start local QuestDB; see §23
docker run --rm -d --name truetest-questdb \
  -p 9000:9000 -p 9009:9009 questdb/questdb:latest

export TRUETEST_BINANCE_API_KEY=<testnet-key>
export TRUETEST_BINANCE_API_SECRET=<testnet-secret>

./build/engine_live --provider binance --symbol btcusdt --stream kline_1m \
  --testnet --live \
  --strategy sma --sma-period 14 --balance 10000 \
  --max-daily-loss 200 --max-trades-per-hour 20 --risk-unwind \
  --persist --run-tag testnet_sma_smoke
```

### 34.10 Binance mainnet live execution

```bash
./build/engine_live --provider binance --symbol btcusdt --stream kline_1m \
  --live --api-key "$KEY" --api-secret "$SECRET" \
  --strategy mean-reversion --balance 50000 \
  --risk-fraction 0.01 --sl 0.003 --tp 0.008 \
  --max-daily-loss 1000 --max-trades-per-hour 50 --risk-unwind
# Before any real order, the engine prints a red "LIVE TRADING — REAL MONEY"
# banner and asks the operator to solve a fresh random addition (two integers
# in [100, 9999]). A wrong, non-numeric, or empty answer aborts cleanly.
# Skipped on --testnet.
```

### 34.11 Record and replay Binance data

```bash
# Record
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --record btc_session.bin

# Replay
./build/engine_backtest --provider binance --symbol btcusdt --stream trade \
  --replay-data btc_session.bin --strategy sma
```

### 34.12 Binary event log: write, then replay

```bash
./build/engine_backtest --provider local --path market_data.csv \
  --strategy sma --log-events events.bin

./build/engine_backtest --replay events.bin
```

### 34.13 Replay a time slice from an event log

```bash
./build/engine_backtest --replay events.bin \
  --replay-from 1700000000000000 --replay-to 1700003600000000
```

### 34.14 Portfolio checkpointing and resume

```bash
# Run with checkpointing
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --checkpoint portfolio.ckpt --checkpoint-interval 5000

# Resume from checkpoint
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --resume portfolio.ckpt
```

### 34.15 JSON config file with CLI override

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

### 34.16 Dry run and dump config

```bash
./build/engine_backtest --config backtest.json --dry-run
./build/engine_backtest --config backtest.json --balance 100000 --dump-config
```

### 34.17 Full-featured Release build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON \
  -DENABLE_NATIVE_OPT=ON -DBUILD_TESTS=ON -DENABLE_BENCHMARKS=ON
cmake --build build -j$(nproc)
```

### 34.18 Debug build with ASAN + UBSAN

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ASAN_OPTIONS="halt_on_error=1:detect_leaks=0" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
  ctest --test-dir build --output-on-failure
```

### 34.19 Debug build with TSAN

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TSAN=ON -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### 34.20 Build with debug instrumentation (Abseil)

```bash
cmake -B build -DENABLE_DEBUG=ON
cmake --build build
./build/engine_backtest --provider local --path market_data.csv --strategy sma
# Writes truetest_debug.log with stage timers, memory, and hardware info.
```

### 34.21 Build shared library for embedding

```bash
cmake -B build -DBUILD_SHARED_LIB=ON
cmake --build build
ls build/libtruetest.so
# Use from Python: ctypes.CDLL("build/libtruetest.so")
```

### 34.22 Run benchmarks

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_BENCHMARKS=ON
cmake --build build --target truetest_benchmarks
./build/truetest_benchmarks --benchmark_min_time=1s
```

### 34.23 Threading preset override

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --thread-preset extended --spin-policy spin --seed 42
```

### 34.24 Disable CPU pinning (containers)

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --no-pin
```

### 34.25 Export results to CSV

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --output results.csv --output-format csv
# Produces results.csv (equity) + results_trades.csv (trade log)
```

### 34.26 Custom risk limits

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma \
  --balance 100000 --risk-fraction 0.01 --sl 0.002 --tp 0.005 \
  --max-daily-loss 2000 --daily-reset-hour 8 \
  --max-trades-per-hour 120 --max-orders-per-minute 30 --risk-unwind
```

### 34.27 Log rotation

```bash
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --log-events events.bin --log-file engine.log \
  --log-max-size 100 --log-keep 10
# Rotates event and text logs at 100 MB, keeps last 10 files.
```

### 34.28 CMake preset — Linux

```bash
cmake --preset linux-default
cmake --build out/build/linux-default
```

### 34.29 CMake preset — Windows Ninja

```cmd
cmake --preset windows-ninja
cmake --build out/build/windows-ninja
```

### 34.30 CMake preset — Windows Visual Studio

```cmd
cmake --preset windows-vs-2022
cmake --build out/build/windows-vs-2022 --config Release
```

### 34.31 start.sh dev mode

```bash
chmod +x start.sh
./start.sh
# Builds engine + starts Vite dev server with hot reload.
# Edit the CONFIGURATION section at the top of start.sh to change
# provider, strategy, etc.
```

### 34.32 Install via CPack

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && cpack -G TGZ
# Produces truetest-0.1.0-Linux.tar.gz with bin/ containing all three engine binaries.
```

### 34.33 Install to prefix

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/truetest
cmake --build build
cmake --install build
# Installs engine_backtest, engine_shadow, engine_live to /opt/truetest/bin/
# If BUILD_SHARED_LIB=ON: libtruetest.so to lib/, truetest_api.h to include/truetest/
```

### 34.34 Production-style live run

```bash
./build/engine_live \
  --provider binance --symbol btcusdt --stream kline_1m \
  --live --api-key "$BINANCE_API_KEY" --api-secret "$BINANCE_API_SECRET" \
  --strategy sma --sma-period 50 --balance 1000 \
  --max-daily-loss 50 --max-trades-per-hour 20 --risk-unwind \
  --checkpoint /var/lib/truetest/live.ckpt \
  --log-events /var/log/truetest/events.bin \
  --log-max-size 100 --log-keep 10 \
  --output /var/log/truetest/results.json
```

Combines tight risk caps with automatic unwind, periodic checkpointing,
rotated event logging, and a JSON analytics export — the shape of a run you
would actually leave unattended.
