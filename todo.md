# TrueTest SaaS Transformation — Implementation Checklist

Each item is a self-contained task with instructions for Claude Code. Execute them roughly in order — later items may depend on earlier ones. After each task, run `cmake --build build` and `ctest` (if `BUILD_TESTS=ON`) to confirm nothing broke.

---

## A. Build System & Compilation Hardening

- [ ] **A1. Add LTO and Release optimization profiles to CMakeLists.txt**
  Add a `CMAKE_BUILD_TYPE`-aware block: `-O3 -DNDEBUG -flto` for Release, `-O2 -g` for RelWithDebInfo, `-O0 -g -fsanitize=address,undefined` for Debug. Add `ENABLE_ASAN` and `ENABLE_UBSAN` CMake flags alongside the existing `ENABLE_TSAN`. Guard them as mutually exclusive with TSAN.
  *Context: CMakeLists.txt is at the root. Current build has no optimization flags. TSAN flag already exists. Do not touch any `target_sources()` blocks or `ENABLE_*` feature flags.*

- [ ] **A2. Add a `cmake --install` target and portable binary packaging**
  Add `install(TARGETS truetest RUNTIME DESTINATION bin)` and `install(FILES web/index.html DESTINATION share/truetest/web)`. Add a `CPACK` section for generating `.tar.gz` and `.deb` packages. This enables deployment without building from source.
  *Context: CMakeLists.txt defines the `truetest` target. The web/ directory contains the UI. No install rules exist yet.*

- [ ] **A3. Add a Dockerfile for reproducible builds and deployment**
  Create `Dockerfile` (multi-stage): stage 1 builds with all deps (Boost, OpenSSL, SQLite3), stage 2 copies only the binary + web assets into a minimal runtime image (e.g., `debian:bookworm-slim`). Expose port 8765. Default CMD runs `./truetest --web-ui`. Add `.dockerignore` excluding `build/`, `.git/`, `.idea/`.
  *Context: Binary is `build/truetest`. Web UI files are in `web/`. WebSocket default port is 8765. Build needs CMake, g++/clang++, Boost headers, OpenSSL dev, SQLite3 dev. Enable BINANCE, WEB_UI, SQLITE in the Docker build.*

- [ ] **A4. Add CI with GitHub Actions: build matrix + test + sanitizer runs**
  Create `.github/workflows/ci.yml`. Matrix: {gcc-13, clang-17} x {Debug, Release}. Steps: checkout, install deps (apt), cmake configure with `BUILD_TESTS=ON`, build, `ctest --output-on-failure`. Add a separate TSAN job (Debug + ENABLE_TSAN). Cache `build/` and vcpkg.
  *Context: Tests use GoogleTest. Build flags are documented in CLAUDE.md. The test binary is built alongside `truetest` when `BUILD_TESTS=ON`.*

---

## B. Configuration & CLI

- [ ] **B1. Replace manual `strcmp` CLI parsing with a proper argument parser**
  Integrate `CLI11` (header-only, fetch via `FetchContent`) or write a minimal `ArgParser` class. Migrate all `--flag` parsing in `main.cpp` (lines ~50-300) to structured argument definitions. Add `--help` output. Validate mutually exclusive flags (e.g., `--provider` vs TUI mode). Reject unknown flags with an error instead of silently ignoring them.
  *Context: `main.cpp` is the only file that does CLI parsing. It uses a long chain of `std::strcmp`. All current flags are documented in CLAUDE.md under "CLI Flags". Do not change any flag names or behavior — only the parsing mechanism.*

- [ ] **B2. Add YAML/JSON config file support (`--config config.yml`)**
  Add a config file loader that reads a YAML or JSON file and populates `engine_config`. CLI flags override config file values. Use a lightweight header-only library (e.g., `nlohmann/json` single-header or `yaml-cpp` via FetchContent). Support all current `engine_config` fields. Add a `--dump-config` flag that prints the resolved config as JSON to stdout.
  *Context: `engine_config.h` defines all configuration. `main.cpp` currently sets config fields from CLI args. The config struct has: mode, balance, fees, risk limits, threading preset, WS port, log paths, seed, pin overrides. Do not change `engine_config.h` layout — the loader writes into the existing struct.*

- [ ] **B3. Add `--dry-run` mode that validates config and exits without running**
  After config resolution (CLI + file), print a summary (mode, strategy, data source, threading preset, risk limits) and exit with code 0 if valid, 1 if not. Useful for CI and deployment validation.
  *Context: Insert this check in `main.cpp` after all config is assembled but before `engine.run()` is called. Do not modify `engine.h/.cpp`.*

---

## C. Data Pipeline Robustness

- [ ] **C1. Add data validation to `data_handler` — reject corrupt/out-of-range records**
  In `data_handler.h`, after parsing each row, validate: `high >= low`, `open/high/low/close > 0`, `volume >= 0`, timestamps are monotonically increasing (for ticks). Log a warning with row number and skip invalid rows instead of silently inserting bad data. Add a `validation_errors()` count method.
  *Context: `data_handler.h` stores OHLCV vectors and tick vectors. CSV parsing happens in `load_from_csv()` (data_loader.cpp) and `tick_csv_data_source.cpp`. Currently, parse errors are silently swallowed in empty catch blocks. Only add validation — do not restructure the data storage.*

- [ ] **C2. Add format versioning and checksum to `binary_cache_source`**
  Prepend a 16-byte header to binary cache files: 4-byte magic (`TTBC`), 2-byte format version (start at 1), 2-byte reserved, 8-byte CRC64 of the payload. On read, verify magic + version + checksum. Reject incompatible versions with a clear error. Bump version if struct layout changes.
  *Context: `binary_cache_source.h/.cpp` uses raw `write()`/`read()` on `std::fstream`. It wraps any `IDataSource` as a caching decorator. Current format has no header — just raw bytes. Old cache files will be incompatible after this change; that's acceptable.*

- [ ] **C3. Add compression to binary event logs (`event_log.h`)**
  Integrate zstd (header-only or FetchContent). Wrap `EventLogger::write()` to compress each event block. `EventReplayer` decompresses on read. Add a `--compress-log` CLI flag (default on). Maintain backward compatibility: detect uncompressed files by checking the first byte (current format starts with event type enum, never `0x28` which is zstd magic).
  *Context: `event_log.h` defines `EventLogger` (write) and `EventReplayer` (read). Binary format is `[type:u8][size:u32][payload]`. Event logs can grow to hundreds of MB for long backtests. Do not change the event struct layout — only wrap the I/O layer.*

- [ ] **C4. Add time-based indexing to binary event logs for seek-by-timestamp**
  Append an index block at the end of the log file: sorted array of `(timestamp_us, file_offset)` pairs, one per N events (e.g., every 1000). Write a footer with the index offset. `EventReplayer` can then binary-search for a start time instead of scanning from the beginning. Add `--replay-from <timestamp>` and `--replay-to <timestamp>` CLI flags.
  *Context: `event_log.h` already has replay support via `EventReplayer`. Currently, replay reads sequentially from byte 0. The replay mode is invoked via `--replay <path>` in main.cpp. Replay currently hardcodes Mean Reversion strategy — that's a separate issue (see B1).*

- [ ] **C5. Implement the `websocket_data_source` message parser (currently a stub)**
  In `websocket_data_source.cpp`, implement `on_message()` to parse JSON market events. Support at minimum: `{"type":"tick","symbol":"...","price":...,"qty":...,"ts":...}` and `{"type":"bar","symbol":"...","o":...,"h":...,"l":...,"c":...,"v":...,"ts":...}`. Use the same hand-rolled JSON extraction pattern used in `BinanceParser`. Route parsed data through the existing callback.
  *Context: `websocket_data_source.h/.cpp` is gated behind `#ifdef HAS_LIVE_DATA`. The `on_message()` method currently has `(void)payload` — a complete no-op. The class already has reconnect logic, heartbeat, and callback plumbing. Only the parsing is missing.*

---

## D. Execution & Order Management

- [ ] **D1. Add explicit order cancellation API**
  Add `cancel_order(order_id) -> bool` to `IExecutionAdapter` and implement it in `LocalBookAdapter` (calls `orderbook::cancel()`), `BinanceExecutor` (REST DELETE), and the `ExchangeAdapter` stub. Add a `cancel` event type to `event.h`. Engine should emit cancel events through the ring buffers so workers and UI are notified.
  *Context: `execution_adapter.h` defines the adapter interface. `orderbook.h` already has cancel logic (marks nodes as ghost). Currently there is no way to cancel an order through the engine API — only DAY orders auto-expire. The event pipeline is: engine -> ring buffers -> workers. Add `cancel` alongside existing event types in `event.h`.*

- [ ] **D2. Add order state tracking (pending/open/filled/cancelled/rejected)**
  Add an `order_status` enum and an `OrderTracker` class that maintains a `std::unordered_map<order_id, order_status>`. Update status on: submission (pending), orderbook acceptance (open), fill (filled/partially_filled), cancel (cancelled), risk rejection (rejected). Expose `get_order_status(id)` and `get_open_orders()`. Wire into engine's event loop.
  *Context: Currently orders are fire-and-forget — no tracking after submission. `order_id.h` provides the ID type. The engine processes orders in `engine.cpp`'s main loop. Add `OrderTracker` as a new file in `execution/`. Do not modify `event.h` event structs — track state separately.*

- [ ] **D3. Add partial fill support**
  Modify `fill_event` in `event.h` to include `remaining_qty` and `fill_id` (to distinguish multiple fills for one order). Update `LocalBookAdapter::poll_fills()` to emit multiple fills when the orderbook partially matches. Update `portfolio.cpp` to handle incremental position changes from partial fills. Update analytics to track partial fills correctly.
  *Context: `fill_event` currently represents a complete fill. The orderbook already supports partial matching internally (`get_reamaining_quantity()` — note the typo). `portfolio.cpp` processes fills in `on_fill()`. `analytics.cpp` records fills for trade stats. Change must be backward-compatible with existing event log format — add new fields at the end of the binary payload.*

- [ ] **D4. Fix the `get_reamaining_quantity()` typo across the codebase**
  Rename `get_reamaining_quantity()` to `get_remaining_quantity()` in `orderbook.h` and all call sites. Use find-and-replace across the entire `BacktestEngine/src/` directory. Update tests too.
  *Context: Typo exists in `orderbook.h/.cpp` and propagates to `execution_adapter.h` and tests. This is a pure rename — no logic changes.*

- [ ] **D5. Parameterize hardcoded execution constants**
  Move these to `engine_config`: market order aggression factor (currently 1.1/0.9 in `LocalBookAdapter`), quantity scale (currently 1e8), RNG seed for fill model (currently 42), and the spread step formula (`mid * 0.0001`). Add corresponding CLI flags.
  *Context: Hardcoded values are in `execution_adapter.h` (LocalBookAdapter) and `engine.cpp` (spread step). `engine_config.h` is the central config struct. Add new fields with the current values as defaults so behavior doesn't change without explicit override.*

---

## E. Orderbook & Matching Engine

- [ ] **E1. Route L2 snapshot/update events from providers to the orderbook**
  In `provider_sink.h`, implement the L2 event routing that is currently marked TODO. When an `l2_snapshot` arrives, call `orderbook::apply_snapshot()`. When an `l2_update` arrives, call `orderbook::apply_update()`. Both methods already exist in `orderbook.h`. Wire this through `engine.cpp`'s streaming event handler.
  *Context: `provider_sink.h` has a visitor for provider events. `l2_snapshot` and `l2_update` types exist in `provider_event.h`. The orderbook methods `apply_snapshot()` and `apply_update()` exist in `orderbook.h/.cpp`. The plumbing between provider events and engine/orderbook is missing.*

- [ ] **E2. Add order modification (amend price/qty) to the orderbook**
  Add `modify_order(order_id, new_price, new_qty) -> bool` to `orderbook.h`. Implementation: cancel existing order, re-insert with same ID at new price/qty (loses time priority — standard behavior). Expose through `IExecutionAdapter` as `modify_order()`. Add an `amend` event type or reuse `order` with an `amend` flag.
  *Context: `orderbook.h` supports add and cancel but not modify. The node pool uses slab allocation with ghost marking for cancels. Re-inserting after cancel is safe. Do not change the matching algorithm.*

---

## F. Strategy Framework

- [ ] **F1. Add runtime strategy parameter configuration**
  Add a `set_param(string key, double value)` method to `IStrategy`. Each strategy defines its accepted params in a `get_param_schema() -> vector<param_def>` method (name, type, default, min, max, description). Wire this to CLI via `--param key=value` (repeatable) and to config file. Update `main.cpp` strategy construction to apply params after instantiation.
  *Context: `strategy_interface.h` defines `IStrategy`. Current strategies have fixed parameters set in constructors. `mean_reversion_strategy` takes period/equity/risk_fraction. `sma_strategy` takes period. Do not change constructor signatures — add `set_param` as a post-construction configuration step.*

- [ ] **F2. Add EMA, RSI, and Bollinger Bands indicators**
  Create `indicator/ema.h` (exponential moving average), `indicator/rsi.h` (relative strength index, 14-period default), `indicator/bollinger.h` (20-period SMA +/- 2 std devs). Each follows the same pattern as `sma.h`: `update(double price) -> std::optional<double>`, `ready() -> bool`, configurable period. Add unit tests for each.
  *Context: `indicator/sma.h` is the only indicator. It uses a rolling queue + sum. New indicators go in the same directory. Strategies reference indicators via member variables. Tests are in `BacktestEngine/tests/`. Follow the SMA test pattern.*

- [ ] **F3. Add a strategy factory with name-based registration**
  Create `strategy/strategy_registry.h` mirroring the `REGISTER_PROVIDER` pattern from `provider_registry.h`. Strategies self-register with a name and factory lambda. `main.cpp` looks up strategies by `--strategy <name>` instead of hardcoded if/else chains. This enables plugin-like strategy loading.
  *Context: `provider_registry.h` implements the exact pattern needed: static map, `REGISTER_PROVIDER` macro, `create()` lookup. Strategy selection in `main.cpp` is currently a manual if/else. Do not remove existing strategy classes — just register them.*

- [ ] **F4. Consolidate duplicate SMA and MA Crossover strategies**
  `sma_strategy` and `ma_crossover_strategy` are functionally identical (both buy when price > SMA, sell when price < SMA, same logic). Merge into a single `SMAStrategy` with configurable parameters. Remove `ma_crossover_strategy.h/.cpp`. Update any references in `main.cpp` and tests. If MA Crossover should differ (e.g., fast/slow SMA cross), implement that distinction.
  *Context: Both files are in `strategy/`. Compare them side by side — they have the same logic. The name "MA Crossover" implies two MAs crossing, but the implementation uses one. Either make it a true crossover (fast SMA crosses slow SMA) or delete it.*

---

## G. Analytics & Reporting

- [ ] **G1. Add rolling window metrics (rolling Sharpe, rolling drawdown)**
  Extend `analytics.h` to compute Sharpe and max drawdown over a configurable trailing window (default: 252 bars for daily, configurable via `engine_config`). Store in a circular buffer. Expose `rolling_sharpe()` and `rolling_max_drawdown()` alongside the existing cumulative metrics. Include in analytics snapshots and WebSocket broadcasts.
  *Context: `analytics.h/.cpp` computes cumulative metrics using Welford's algorithm. The equity curve is stored as `vector<equity_point>`. Rolling metrics need a fixed-size window over recent equity points. Do not replace cumulative metrics — add rolling ones alongside.*

- [ ] **G2. Add per-symbol and per-strategy performance attribution**
  Add breakdown tracking to `analytics.h`: per-symbol PnL, win rate, trade count; per-strategy (when multiple strategies run). Store in `unordered_map<string, sub_analytics>` where `sub_analytics` tracks the same core metrics. Expose in reports and snapshots.
  *Context: Currently analytics is global — one set of metrics for the entire run. `fill_event` already carries symbol. Strategy name is not on events — add a `strategy_name` field to `order_event` in `event.h` to enable attribution. Keep the global analytics intact.*

- [ ] **G3. Add structured JSON/CSV result export at end of backtest**
  After `engine.run()` completes, write a `results.json` file containing: final equity, total return, Sharpe, Sortino, max drawdown, win rate, trade count, profit factor, and the full equity curve. Add `--output <path>` CLI flag (default: stdout). Add `--output-format json|csv` flag.
  *Context: `analytics.h` already has `generate_report()` that prints to stdout and `export_equity_csv()`/`export_trades_csv()`. Add a new `export_json()` method. Wire the `--output` flag in `main.cpp` after engine run completes. Use the existing snprintf JSON pattern from `event_json.h`.*

- [ ] **G4. Add benchmark comparison (buy-and-hold, risk-free rate)**
  Extend analytics to track a buy-and-hold benchmark alongside strategy equity. Compute: alpha, beta, information ratio, tracking error. The benchmark invests 100% at the first price and holds. Add `--risk-free-rate <annual>` CLI flag (default 0.0) for excess return calculations in Sharpe/Sortino.
  *Context: `analytics.h` already tracks `first_price_` and computes a basic buy-and-hold return. Extend this to a full equity curve for the benchmark. Sortino currently uses total downside — change to downside relative to risk-free rate. Do not break existing Sharpe/Sortino values when risk-free rate is 0.*

---

## H. Risk Management

- [ ] **H1. Make risk checks pre-order instead of post-order**
  Move the risk check from after order submission to before. In `engine.cpp`, call `risk_manager.check_order()` before passing the order to the execution adapter. If rejected, emit a `rejection` event (new event type) with reason string. Currently risk checks happen in the risk worker thread (async, after the fact).
  *Context: `engine.cpp` processes orders in the main loop. `risk_manager.h` has `check_order()` that returns pass/reject/halt. Currently the check runs in `risk_worker.h` on a shadow portfolio (delayed). The synchronous pre-check in the engine loop provides real-time rejection. Keep the async shadow check in the risk worker as a secondary validation layer.*

- [ ] **H2. Add time-based risk limits (daily loss limit, max trades per hour)**
  Add to `risk_limits` struct in `engine_config.h`: `max_daily_loss` (resets at configurable hour, default midnight UTC), `max_trades_per_hour`, `max_orders_per_minute`. Implement tracking in `risk_manager.h/.cpp` using timestamp-aware counters. Add corresponding CLI flags.
  *Context: `risk_manager.h` currently checks: max drawdown, max single loss, max open orders, max position value, max exposure. None are time-windowed. `risk_limits` is a nested struct in `engine_config.h`. Add new fields with sensible defaults (e.g., daily loss = no limit by default).*

- [ ] **H3. Add automatic position unwinding on risk halt**
  When `risk_manager` triggers a halt, instead of just stopping the engine, emit market sell orders for all open positions (graceful unwind). Add a `risk_action::unwind` alongside `halt`. The engine processes unwind by iterating `portfolio.get_positions()` and submitting closing orders. Add `--risk-unwind` CLI flag to enable (default: halt only, for backward compat).
  *Context: `risk_manager.h` returns `risk_action` enum (pass/reject/halt). `portfolio.h` has `get_positions()` returning all open positions. Engine halt is controlled by `halt_flag_` atomic. The unwind should happen in the engine main loop, not in a worker thread, to avoid race conditions.*

---

## I. Threading & Performance

- [ ] **I1. Pin the event loop to Core 0 in multithreaded mode**
  In `engine.cpp`, at the start of `run()` / `run_streaming()`, if threading preset is not `inline`, call `thread_config::pin_to_core(0)` (or the user-specified `pin_event_loop` core) on the current thread. The function exists in `thread_config.h` but is never called for the main thread.
  *Context: `thread_config.h` has `pin_current_thread(core_id)` using `pthread_setaffinity_np`. Worker threads are already pinned via their `start()` methods. The engine's main loop runs on the calling thread and is never pinned. This is a one-line addition at the top of `run()`.*

- [ ] **I2. Replace busy-wait in worker threads with exponential backoff**
  In `worker.h`, change the `run()` loop: after a failed `try_pop()`, spin for N iterations (e.g., 64), then `_mm_pause()` for M iterations (e.g., 256), then `std::this_thread::yield()`. This reduces CPU waste from 100% to near-zero when idle while maintaining low latency when events flow. Add a `--spin-policy spin|yield|adaptive` CLI flag.
  *Context: `worker.h` defines the base `Worker::run()` template. Currently it calls `try_pop()` in a tight loop with no backoff. All worker subclasses inherit this loop. The change is in one place (`worker.h`). Do not add condition variables — they add syscall overhead that defeats the purpose of lock-free rings.*

- [ ] **I3. Add ring buffer watermark alerts and backpressure metrics**
  Add to `ring_buffer.h`: `high_watermark()` returning the max observed occupancy, `drop_count()` for DropOldest policy, and a callback `on_watermark(threshold, callback)` that fires when occupancy exceeds a threshold (e.g., 75%). Expose these in the debug report and WebSocket UI. This provides early warning before drops occur.
  *Context: `ring_buffer.h` is the lock-free SPSC ring. It has `occupancy()` but no tracking of max occupancy or drops. `debug/ring_stats.h` tracks some of this externally when `HAS_DEBUG` is on. Make the watermark tracking always-on (cheap: one atomic max update per push) and the callback opt-in.*

- [ ] **I4. Batch SQLite inserts into transactions**
  In `engine.cpp`, wrap SQLite writes in transactions: begin transaction, insert up to 100 rows, commit. Currently each equity point and trade is a separate INSERT (one transaction per row = ~100x slower than batched). Add a `SqliteBatcher` helper class that flushes on batch size or time interval.
  *Context: SQLite writes happen in `engine.cpp` wherever `db_` (the SQLite handle) is used. Search for `db_->` calls. The current pattern is individual INSERT statements. SQLite has 1000x throughput difference between individual and batched inserts. Do not change the schema — only wrap existing inserts in transactions.*

---

## J. WebSocket UI & API

- [ ] **J1. Add JSON schema validation for incoming WebSocket commands**
  In `ws_worker.h`, replace the hand-rolled string extraction for client commands with structured parsing. Define expected schemas for each command type (start, stop, order, set_strategy, etc.). Reject malformed commands with an `error_to_json()` response instead of silently ignoring them. Log rejected commands.
  *Context: `ws_worker.h` parses commands like `{"cmd":"order","side":"buy",...}` using `find()`/`substr()` string operations. This is brittle and accepts malformed JSON. Use the same hand-rolled extraction pattern but add field-presence checks and type validation. Do not add a JSON library — stay consistent with the no-external-JSON-lib convention.*

- [ ] **J2. Add WebSocket message compression (per-message deflate)**
  Enable Boost.Beast's per-message deflate extension in `ws_worker.h`. This typically reduces JSON message size by 60-80%. Add a `--ws-compress` CLI flag (default on). The extension is negotiated in the WebSocket handshake — clients that don't support it fall back to uncompressed.
  *Context: `ws_worker.h` uses `boost::beast::websocket::stream`. Beast supports permessage-deflate via `stream.set_option(permessage_deflate{})`. This is a configuration change on the stream object, not a code restructure. Guard behind `#ifdef HAS_WEB_UI`.*

- [ ] **J3. Add REST API alongside WebSocket for backtest submission and results**
  Add an HTTP endpoint handler to the existing Boost.Beast server in `ws_worker.h` (or a new `http_handler.h`). Routes: `POST /api/backtest` (submit config JSON, returns run ID), `GET /api/backtest/<id>/status`, `GET /api/backtest/<id>/results`. This enables programmatic access without WebSocket. Use the existing `engine_config` parsing from B2.
  *Context: The Beast server in `ws_worker.h` already handles HTTP upgrade to WebSocket. Before the upgrade, it can serve HTTP requests. Add a router that checks the request path. For backtest submission, spawn the engine in a separate thread and track by ID. This is foundational for the SaaS API layer.*

- [ ] **J4. Add session-specific event filtering for WebSocket clients**
  Allow clients to subscribe to specific event types and symbols. On connect, clients send `{"cmd":"subscribe","events":["fill","tick"],"symbols":["BTCUSDT"]}`. The WS worker filters outbound events per session. Default: all events (backward compat). This reduces bandwidth for clients that only care about specific data.
  *Context: `ws_worker.h` broadcasts all events to all connected clients. Each client has a `WsSession` with its own ring buffer. Add a `filter` struct to `WsSession` (set of event types + set of symbols). Check filter before pushing to session ring.*

---

## K. Data Persistence & State

- [ ] **K1. Add backtest run metadata persistence (SQLite)**
  Create a `runs` table: `(run_id TEXT PK, started_at INT, ended_at INT, config_json TEXT, status TEXT, final_equity REAL, sharpe REAL, max_drawdown REAL, trade_count INT)`. Insert a row at engine start, update at engine end. Add `GET /api/runs` endpoint (from J3) to list past runs. This enables run history and comparison.
  *Context: SQLite is already used in `engine.cpp` for equity/trade storage (`ENABLE_SQLITE`, on by default). The `db_path` config field points to the SQLite file. Add the new table alongside existing ones. Use the same db handle.*

- [ ] **K2. Add deterministic replay guarantee — seed all RNG sources**
  Audit all `std::mt19937` / `std::rand()` usage. Ensure every RNG is seeded from `engine_config.seed`. Currently: `LocalBookAdapter` uses seed 42 (hardcoded), `StochasticLatencyModel` uses seed 42, `MarketMaker` uses `std::rand()`. Route all through `engine_config.seed`. When `seed == 0`, use `std::random_device`. Add a test that runs the same backtest twice with the same seed and asserts identical results.
  *Context: `engine_config.h` has a `seed` field. `main.cpp` passes `--seed` from CLI. But individual components ignore it and use their own hardcoded seeds. Search for `mt19937`, `rand()`, `random_device` across all source files.*

- [ ] **K3. Add portfolio state snapshots for resume-after-crash**
  Every N events (configurable, default 10000), serialize the full engine state to a checkpoint file: portfolio positions, cash, analytics state, orderbook state, event counter. On startup, if `--resume <checkpoint>` is passed, restore state and continue from the last event. Use the binary serialization pattern from `event_log.h`.
  *Context: `portfolio.h` has all position state. `analytics.h` has equity curve and metric accumulators. The engine processes events sequentially with a counter. Checkpoint = snapshot of these at a known event index. Store as a binary file alongside the event log. This is important for long-running live/shadow mode sessions.*

---

## L. Observability & Debugging

- [ ] **L1. Add structured logging with severity levels (always-on, not just HAS_DEBUG)**
  Create `utils/logger.h` with a minimal logging interface: `LOG_INFO(fmt, ...)`, `LOG_WARN(fmt, ...)`, `LOG_ERROR(fmt, ...)`. Output: `[timestamp] [level] [component] message`. Route to stderr by default, file if `--log-file` is set. Replace all `std::cerr <<` and `fprintf(stderr, ...)` calls across the codebase. This is separate from the event logging in `LoggingWorker` (which logs trading events, not operational events).
  *Context: Current operational logging is ad-hoc: `std::cerr`, `printf`, `fprintf(stderr, ...)` scattered everywhere. `debug/debug_log.h` exists but only works with `HAS_DEBUG` and requires Abseil. The new logger must be zero-dependency and always available. Do not touch `LoggingWorker` — it handles trading event logs, not operational logs.*

- [ ] **L2. Add Prometheus-compatible metrics endpoint**
  Add `GET /metrics` HTTP endpoint (from the Beast server, see J3) exposing: events_processed_total, orders_submitted_total, fills_total, ring_buffer_occupancy, ring_buffer_drops_total, equity_current, drawdown_current, latency_p50/p99 (if available), active_websocket_sessions. Use Prometheus text exposition format.
  *Context: Most of these metrics already exist in various places: analytics tracks fills/orders, ring_buffer has occupancy, debug/ has latency stats. This task collects them into a single HTTP endpoint. No external library needed — Prometheus text format is trivial (`metric_name value\n`). Guard behind `#ifdef HAS_WEB_UI` since it uses the HTTP server.*

- [ ] **L3. Add event log rotation and retention policy**
  When `--log-events <path>` is used, rotate the binary log file when it exceeds a size threshold (default 100MB, configurable via `--log-max-size`). Keep N rotated files (default 5, configurable via `--log-keep`). Naming: `events.bin`, `events.bin.1`, `events.bin.2`, etc. Apply the same rotation to text logs (`--log-text`).
  *Context: `event_log.h` writes to a single file via `EventLogger`. The file grows unbounded. Rotation must happen in `LoggingWorker` (which owns the logger) or in the logger itself. Do not rotate mid-transaction — finish the current event write, then rotate.*

---

## M. Multi-Symbol & Multi-Strategy

- [ ] **M1. Support running multiple strategies simultaneously**
  Modify `engine.cpp` to hold a `vector<unique_ptr<IStrategy>>` instead of a single strategy pointer. On each market event, iterate all strategies and collect their orders. Tag each order with the originating strategy name (add `strategy_name` to `order_event` if not done in G2). Portfolio tracks combined positions; analytics tracks per-strategy (see G2).
  *Context: `engine.h` currently holds `std::unique_ptr<IStrategy> strategy_`. The event loop calls `strategy_->on_market()`. Strategies return `optional<order_event>`. With multiple strategies, collect all orders into a vector and process them in sequence. The orderbook handles them normally. Risk checks apply to the combined portfolio.*

- [ ] **M2. Support concurrent multi-symbol backtesting**
  Allow `--symbol SYM1,SYM2,...` to specify multiple symbols. The engine processes events for all symbols (interleaved by timestamp). Each symbol gets its own orderbook (via `OrderbookRegistry`, already implemented). Strategies receive events for all symbols and can maintain per-symbol state (they already use `unordered_map<string, ...>` internally).
  *Context: `OrderbookRegistry` already manages per-symbol orderbooks. `IStrategy` base class has per-symbol position tracking. The missing piece is data loading: `data_handler` loads one CSV. For multi-symbol, either accept multiple `--path` flags or a directory of CSVs. For Binance, support multiple `--symbol` values each opening a stream.*

---

## N. Error Handling & Resilience

- [ ] **N1. Add graceful worker recovery instead of engine halt on worker exception**
  In `worker.h`, catch exceptions in the `run()` loop, log the error (via L1 logger), increment an error counter, and continue processing. After N consecutive errors (configurable, default 5), then set the halt flag. This prevents a single malformed event from killing the entire engine.
  *Context: `worker.h`'s `run()` template catches exceptions and immediately sets `halt_flag_`. Workers process events from ring buffers. A parse error or analytics edge case shouldn't halt live trading. The engine's main loop checks `halt_flag_` each iteration. Add `max_consecutive_errors` to worker config.*

- [ ] **N2. Add input sanitization for WebSocket command strings**
  In `ws_worker.h`, before processing any client command, validate: message length < 4KB, no null bytes, required fields present, numeric fields are actually numeric, string fields don't contain control characters. Reject with a structured error response. This prevents crashes from malformed client input.
  *Context: `ws_worker.h` processes raw strings from WebSocket clients. Current parsing uses `find()`/`substr()` without bounds checking. A crafted message could cause `std::out_of_range` or unexpected behavior. This is a system boundary — validate everything.*

- [ ] **N3. Add connection retry with exponential backoff for all external connections**
  Create a `utils/retry.h` template: `retry_with_backoff(callable, max_attempts, initial_delay, max_delay) -> result`. Apply to: Binance WebSocket connect (already has basic retry), Binance REST API calls, PostgreSQL connection, and the generic `websocket_data_source`. Unify the retry pattern instead of each component implementing its own.
  *Context: `BinanceTransport` has retry logic (5 attempts, exponential backoff). `websocket_data_source.cpp` has its own backoff. `pg_data_source` has no retry. Unify into one utility. Do not change the existing Binance retry behavior — just extract it into the shared utility and have Binance use it too.*

---

## O. Testing & Quality

- [ ] **O1. Add integration tests for the full engine pipeline**
  Create `tests/test_engine_integration.cpp`. Test: load CSV data -> run engine with SMA strategy -> verify final equity is deterministic (with fixed seed). Test: submit order -> verify fill event emitted -> verify portfolio updated. Test: risk limit hit -> verify halt. These tests exercise the full pipeline, not individual units.
  *Context: Existing tests are unit tests (per-component). The engine's `run()` method is never tested end-to-end. Create a small test CSV fixture (10-20 bars). Construct `engine_config` programmatically (no CLI parsing). Call `engine.run()` and inspect `portfolio`/`analytics` state afterward.*

- [ ] **O2. Add a backtest regression test suite with golden files**
  Create a `tests/golden/` directory with: input CSV, config JSON, expected `results.json` (from G3). A test loads the input, runs the backtest with the config, and diffs the output against the golden file. Any code change that alters backtest results will fail this test, catching unintended behavior changes.
  *Context: Depends on G3 (JSON export) and K2 (deterministic seeding). The golden file contains exact numeric values (equity, Sharpe, etc.). Use fixed seed, fixed config. Store 2-3 golden sets covering different strategies and data.*

- [ ] **O3. Add performance benchmarks with Google Benchmark**
  Create `benchmarks/` directory. Benchmark: orderbook insert+match throughput (orders/sec), ring buffer push+pop throughput, event serialization throughput, SMA indicator update throughput, full engine throughput (events/sec on a 100K bar dataset). Integrate with CMake via `ENABLE_BENCHMARKS` flag. This establishes baseline performance for optimization work.
  *Context: Google Benchmark can be fetched via `FetchContent` like GoogleTest. Benchmarks are separate from tests — they measure throughput, not correctness. The orderbook and ring buffer are the hot-path components. Use `benchmark::DoNotOptimize` to prevent dead code elimination.*

---

## P. API & Programmatic Access

- [ ] **P1. Add a C API / shared library build target for embedding**
  Add a `BUILD_SHARED_LIB` CMake flag that builds `libtruetest.so`. Expose a C API in `api/truetest_api.h`: `tt_create_engine(config_json) -> handle`, `tt_run(handle)`, `tt_get_results(handle) -> json`, `tt_destroy(handle)`. This enables embedding TrueTest in Python (via ctypes/cffi), Node.js (via ffi-napi), or other languages.
  *Context: Currently only a CLI binary is built. The engine is fully contained in C++ classes. The C API wraps: config parsing (from B2), engine construction, run, result extraction (from G3). Use `extern "C"` and opaque handles. Keep the CLI binary as the primary interface — the shared lib is an alternative.*

- [ ] **P2. Add a Python wrapper for the C API**
  Create `python/truetest.py` using `ctypes` to wrap the C API from P1. Provide: `Engine(config_dict)`, `engine.run()`, `engine.results -> dict`. Add a `pip install -e .` setuptools config. This is the primary programmatic interface for quants who want to script backtests.
  *Context: Depends on P1. The Python wrapper is thin — just ctypes bindings + dict<->JSON conversion. Include a `python/example.py` showing: load config, run backtest, print Sharpe ratio. Do not add numpy/pandas dependencies — keep it minimal.*

---

*Total: 42 tasks. Execute in order within each section. Sections A-D are foundational and should be done first. Sections E-H build on them. Sections I-P can be parallelized.*
