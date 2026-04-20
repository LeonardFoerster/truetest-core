# CLAUDE.md

## What this is

TrueTest — a modular C++23 engine that starts as a backtesting platform but is
designed to be reused as the foundation for different deployments: pure backtesting,
Binance spot execution, Polymarket AMM, MetaTrader EA, or anything else that
processes market data through a strategy and orderbook pipeline. Each deployment
attaches different "blocks" (storage backends, execution targets, data feeds) to
the same core.

The build produces three binaries from the same source tree —
`engine_backtest`, `engine_shadow`, `engine_live` — differing only in a
compile-time `TT_TARGET` define. Live-order paths are gated at compile time,
so only `engine_live` can ever place real orders (the others hard-reject
`--mode=live`).

## Project structure

```
hft-engine/
├── CMakeLists.txt                      # build config, multiple opt-in flags
├── CMakePresets.json                   # linux-default + Windows presets
├── cmake/
│   ├── CompilerFlags.cmake             # C++23, per-config opt, tt_apply_* helpers
│   └── Dependencies.cmake              # tt_fetch_* + tt_wire_optional_backends
├── vcpkg.json                          # only matters when ENABLE_POSTGRESQL=ON
├── Dockerfile / .dockerignore          # containerized build
├── start.sh                            # launcher script
├── .github/workflows/ci.yml            # CI pipeline
├── market_data.csv                     # sample OHLCV data
├── docs/
│   ├── flowchart.md
│   ├── 01-persistent-state.md … 05-historical-backfill.md
│   └── refactor/00-overview.md … 15-keep-as-is.md   # roadmap / refactor plans
├── web/                                # React 19 + TypeScript + Vite + Tailwind SPA
│   ├── src/
│   │   ├── App.tsx, main.tsx
│   │   ├── components/ (Chart, Sidebar, BottomPanel, TopBar, Toast)
│   │   ├── contexts/WebSocketContext.tsx
│   │   ├── services/websocket.ts
│   │   └── store/ (Engine, Market, OrderBook, Portfolio, Fill, Analytics)
│   ├── index.html                      # Vite entry
│   └── index.legacy.html               # old single-file dashboard (kept for reference)
├── tests/                              # 39 test files, ~310 cases (GoogleTest)
└── BacktestEngine/
    └── src/
        ├── main.cpp                    # entry point: TUI, CLI, provider, replay modes
        ├── api/
        │   └── truetest_api.h/.cpp     # C API for embedding (BUILD_SHARED_LIB)
        ├── core/
        │   ├── engine.h/.cpp           # event loop orchestrator (batch + streaming)
        │   ├── engine_config.h         # engine_config struct (mode, fees, threading,
        │   │                           #   risk, WS, SQLite, checkpoint, backfill,
        │   │                           #   execution constants, log rotation, provider)
        │   ├── event.h                 # event types: market, order, fill, tick
        │   ├── event_json.h            # snprintf-based JSON for event types
        │   ├── event_log.h             # binary event log (write + replay, zstd)
        │   ├── checkpoint.h            # portfolio-state snapshot format
        │   └── tt_target.h             # compile-time target id (TT_TARGET), default
        │                               #   mode, target_allows_live_orders() gate
        ├── data/
        │   ├── data_source.h           # IDataSource interface
        │   ├── csv_data_source.h/.cpp  # CSV OHLCV backend (default, zero deps)
        │   ├── tick_csv_data_source.h/.cpp # tick-level CSV backend
        │   ├── binary_cache_source.h/.cpp # caching decorator for any IDataSource
        │   ├── pg_data_source.h/.cpp   # PostgreSQL backend (#ifdef HAS_POSTGRESQL)
        │   ├── websocket_data_source.h/.cpp # WebSocket feed (#ifdef HAS_LIVE_DATA)
        │   ├── sqlite_store.h/.cpp     # trade/portfolio/equity persistence (#ifdef HAS_SQLITE)
        │   ├── data_handler.h          # in-memory OHLCV column vectors
        │   └── data_loader.cpp         # load_from_csv + load_into_queue impl
        ├── execution/
        │   ├── execution_adapter.h    # IExecutionAdapter, LocalBookAdapter
        │   ├── order_tracker.h        # lifecycle state tracking
        │   ├── portfolio.h/.cpp       # position tracking, PnL
        │   ├── fee_model.h            # IFeeModel (Zero, Fixed, Tiered)
        │   └── latency_model.h        # execution latency simulation
        ├── orderbook/
        │   ├── orderbook.h/.cpp        # price-time priority matching engine
        │   ├── orderbook_registry.h    # multi-symbol orderbook management
        │   └── fill_model.h            # partial-fill probability modeling
        ├── strategy/
        │   ├── strategy_interface.h    # IStrategy interface
        │   ├── strategy_registry.h     # factory + REGISTER_STRATEGY macro
        │   ├── strategy_factory.h
        │   ├── mean_reversion_strategy.h/.cpp
        │   ├── sma_strategy.h/.cpp
        │   └── ma_crossover_strategy.h/.cpp
        ├── indicator/
        │   ├── sma.h
        │   ├── ema.h
        │   ├── rsi.h
        │   └── bollinger.h
        ├── analytics/
        │   ├── analytics.h/.cpp        # Welford online algo, Sharpe/Sortino/drawdown/win rate
        │   ├── bar_aggregator.h        # tick-to-bar aggregation
        │   └── shadow_tracker.h        # shadow-vs-exchange fill comparison (shadow mode)
        ├── risk/
        │   └── risk_manager.h/.cpp     # pre/post-fill checks, halt signaling
        ├── threading/
        │   ├── thread_preset.h         # 5 presets: inline, light, standard, full, extended
        │   ├── thread_config.h         # CPU affinity detection + pinning
        │   ├── spin_policy.h           # spin / yield / adaptive backoff
        │   ├── worker.h                # Worker base class
        │   ├── ring_buffer.h           # lock-free SPSC ring buffer
        │   ├── logging_worker.h        # event logging (text + binary sinks)
        │   ├── risk_worker.h           # shadow portfolio + halt flag
        │   ├── stats_worker.h          # analytics accumulation + snapshots
        │   ├── observer_worker.h       # combined observer (light preset)
        │   ├── risk_stats_worker.h     # combined risk+stats (standard preset)
        │   ├── market_maker_worker.h   # MM replenish orders (extended preset)
        │   ├── ws_worker.h             # WebSocket UI broadcaster (#ifdef HAS_WEB_UI)
        │   └── http_handler.h          # HTTP helpers for WS handshake / control
        ├── providers/
        │   ├── provider.h              # IProvider (lifecycle, configure, on_mid_price)
        │   ├── provider_registry.h     # factory registry + REGISTER_PROVIDER macro
        │   ├── transport.h             # IDataTransport (batch + streaming)
        │   ├── parser.h                # IDataParser<T> template
        │   ├── data_bridge.h           # transport + parser orchestrator
        │   ├── provider_event.h        # normalized event variant (bar, tick, l2, status)
        │   ├── provider_convert.h      # type conversion helpers
        │   ├── provider_sink.h         # event sink functions
        │   ├── prepend_transport.h     # decorator that injects lines before delegating
        │   ├── local/                  # file-based data (CSV bar + tick)
        │   │   ├── local_provider.h
        │   │   ├── local_register.cpp
        │   │   ├── file_transport.h
        │   │   └── csv_parser.h
        │   ├── binance/                # live spot market (#ifdef HAS_BINANCE)
        │   │   ├── binance_provider.h
        │   │   ├── binance_register.cpp
        │   │   ├── binance_transport.h
        │   │   ├── binance_parser.h
        │   │   ├── binance_executor.h         # paper + live REST order submission
        │   │   ├── binance_auth.h             # HMAC-SHA256 signing
        │   │   ├── binance_rest_client.h      # signed REST client
        │   │   ├── binance_backfill.h         # historical klines via REST
        │   │   ├── hybrid_executor.h          # paper-market + book-limit fills
        │   │   ├── binance_combined_transport.h / _parser.h  # multi-stream
        │   │   ├── binance_depth_parser.h     # L2 depth
        │   │   ├── binance_recorder.h         # record live WS to file
        │   │   └── binance_replay_transport.h # replay recorded WS file
        │   ├── metatrader/             # planned: EA bridge via named pipe/socket
        │   └── polymarket/             # planned: AMM execution
        ├── market_maker/
        │   └── market_maker.h/.cpp     # liquidity seeding
        ├── types/
        │   ├── order_id.h              # global order ID generator
        │   ├── price.h                 # fixed-point price representation
        │   ├── object_pool.h           # pre-allocated event pool
        │   └── aliases.h               # type aliases
        ├── utils/
        │   ├── log/logger.h
        │   └── retry.h
        └── debug/                      # (#ifdef HAS_DEBUG)
            ├── stage_timer.h/.cpp
            ├── memory_info.h/.cpp
            ├── hardware_info.h/.cpp
            ├── thread_stats.h
            ├── ring_stats.h
            ├── copy_tracker.h
            ├── debug_log.h
            └── debug_report.h/.cpp     # aggregate end-of-run report
```

## Build

```bash
# Default — CSV + SQLite persistence on, no network deps
cmake -B build
cmake --build build

# With optional features
cmake -B build \
  -DENABLE_POSTGRESQL=ON \    # PostgreSQL via libpqxx (auto-fetches vcpkg)
  -DENABLE_WEB_UI=ON \        # WebSocket dashboard (requires Boost headers)
  -DENABLE_BINANCE=ON \       # Binance live streaming + REST execution (Boost.Beast + OpenSSL)
  -DENABLE_LIVE_DATA=ON \     # Generic WebSocket data source
  -DENABLE_DEBUG=ON \         # Performance instrumentation (Abseil)
  -DENABLE_SQLITE=OFF \       # Disable SQLite persistence (default ON)
  -DENABLE_TSAN=ON \          # ThreadSanitizer (mutually exclusive with ASAN/UBSAN)
  -DENABLE_ASAN=ON \          # AddressSanitizer
  -DENABLE_UBSAN=ON \         # UndefinedBehaviorSanitizer
  -DENABLE_NATIVE_OPT=ON \    # -march=native + unroll on engine_live only (Release)
  -DENABLE_BENCHMARKS=ON \    # Google Benchmark suite
  -DBUILD_SHARED_LIB=ON \     # libtruetest shared library + C API (src/api/truetest_api.h)
  -DBUILD_TESTS=ON            # GoogleTest suite (~310 cases; a few EngineStreaming
                              #   streaming tests crash at teardown — known issue)
cmake --build build

# Run — three binaries, same source tree, distinct TT_TARGET at compile time
./build/engine_backtest                                   # interactive TUI, backtest default
./build/engine_backtest --provider local --path market_data.csv --strategy sma
./build/engine_shadow   --provider binance --symbol btcusdt --stream trade --web-ui
./build/engine_backtest --replay event_log.bin
./build/engine_live     --provider binance --symbol btcusdt --stream kline_1m \
  --live --api-key KEY --api-secret SECRET                # REAL orders (confirmation prompt)
```

Each binary links the shared `engine_core` OBJECT library and differs only
in its `TT_TARGET` define. `--mode=live` is rejected by any binary whose
`target_allows_live_orders()` returns `false` (i.e. everything except
`engine_live`). The shared library (when `BUILD_SHARED_LIB=ON`) is
`build/libtruetest.so`, C header at `BacktestEngine/src/api/truetest_api.h`.

## Architecture decisions

### Provider system (transport + parser + executor)
All external data/execution flows through the `IProvider` interface. Providers
self-register via `REGISTER_PROVIDER()` macro at static init. Each provider owns:
- `IDataTransport` — where data comes from (file, WebSocket, pipe)
- `IDataParser<T>` — how to parse it (CSV, JSON, binary)
- `IExecutionAdapter` — how to submit orders (local orderbook, exchange API)

`IProvider` also exposes `configure(engine_config&)`, `on_mid_price(sym, px)`,
and `lifecycle_state()` so provider-specific wiring (backfill, hybrid executor
book-seeding, WebSocket state) stays out of the core engine.

`DataBridge<T>` orchestrates transport + parser for both batch and streaming modes.
`PrependTransport` is a decorator that yields a fixed set of lines before
delegating to an inner transport (used by `BinanceProvider` to inject historical
backfill bars as synthetic kline JSON, invisible to the engine).

### Strategies self-register too
`REGISTER_STRATEGY()` in `strategy/strategy_registry.h` mirrors the provider
registry. `sma`, `mean-reversion`, and `ma-crossover` register at static init.
`main.cpp` supports multi-strategy mode via `--strategy sma,mean-reversion`.

### Storage is pluggable via IDataSource
All data backends implement `IDataSource::load_data(shared_ptr<data_handler>)`.
The core engine never touches storage directly.

### Persistence backends are opt-in
- PostgreSQL via `-DENABLE_POSTGRESQL=ON` (`HAS_POSTGRESQL`) — auto-fetches vcpkg
  if `pg_config` is not on PATH.
- SQLite via `-DENABLE_SQLITE=ON` (default on, `HAS_SQLITE`) — trades, portfolio,
  and equity snapshots persisted to the path given by `--db-path`.

### BinaryCacheSource is a decorator
Wraps any `IDataSource` and caches results to a binary file.

### Engine owns no data loading
`engine::run()` expects `data_handler` to already be populated. Loading happens
before `run()` in `main.cpp`. Streaming mode (`run_streaming()`) receives records
via `DataBridge` callback. Replay mode (`run_replay()`) reads a binary event log.

### Event-driven pipeline
`market_event → IStrategy → order_event → orderbook / provider_adapter → fill_event → portfolio`
Hot-path events use pre-allocated object pools to avoid heap pressure.

### Multithreaded worker architecture
Core 0 runs the hot path. Worker threads consume events via lock-free SPSC ring
buffers (65536 slots). Five auto-detected presets scale with physical core count:

| Preset | Cores | Workers |
|--------|-------|---------|
| inline | 1-2 | none (single-threaded) |
| light | 3 | ObserverWorker (combined) |
| standard | 4-5 | LoggingWorker + RiskStatsWorker |
| full | 6-7 | LoggingWorker + RiskWorker + StatsWorker |
| extended | 8+ | + MarketMakerWorker |

CPU affinity pinning via `sched_setaffinity` (Linux). Each worker holds a shadow
copy of portfolio/analytics to avoid data races. Worker spin policy is
configurable (`spin`, `yield`, `adaptive`).

### Portfolio checkpointing
When `checkpoint_path` is set, the engine writes a binary snapshot of portfolio
state every N events (default 10k). Setting `resume_checkpoint_path` at
construction pre-populates the portfolio from the referenced file, enabling
resume-after-crash workflows.

### WebSocket UI (bidirectional)
When `--web-ui` is passed, a Boost.Beast WebSocket server (default port 8765,
per-message deflate negotiated) broadcasts all events as JSON to connected
browsers. The frontend under `web/` is a React 19 + TypeScript + Vite + Tailwind
SPA using `lightweight-charts`; build with `npm run build` in `web/` and serve
`web/dist/`. The legacy single-file dashboard is preserved as
`web/index.legacy.html`. Clients send JSON commands back to the engine (start,
pause, stop, order, set_timeframe, set_symbol, set_strategy) via `ws_command`
structs polled by the engine.

### C API for embedding
`src/api/truetest_api.h` exposes an opaque handle + JSON-config surface
(`tt_create_engine`, `tt_run`, `tt_get_results`, `tt_last_error`, `tt_destroy`)
intended for Python (ctypes/cffi) or Node.js (ffi-napi) host processes. Built
as `libtruetest.so` when `-DBUILD_SHARED_LIB=ON`.

## Implemented providers

### Local (always available)
File-based CSV provider. Supports bar (OHLCV) and tick formats. Batch mode only.

### Binance (ENABLE_BINANCE=ON)
Live WebSocket streaming from Binance spot market. Supports `trade`, `kline_*`,
combined, and depth (L2) streams. Parser is pure C++ (no JSON library on the
hot path — only nlohmann/json is linked, and only for static config files).
Historical-bar backfill via REST, injected into the live stream through
`PrependTransport`.

Execution modes:
- **Paper** (default): orders logged, fills simulated from last price.
- **Hybrid**: paper market orders + local-book limit fills (default for
  backtest/shadow/paper modes). Owns synthetic book-seeding around the mid
  price.
- **Live**: signed REST order submission against `/api/v3/order` via
  `BinanceRestClient`. `poll_live_fills()` polls order status for fills. Cancel
  and modify are wired. Requires `--live` flag, `--api-key`, `--api-secret`,
  and explicit "YES" confirmation.

Live WS recording + replay: `BinanceRecorder` captures a live stream to file;
`BinanceReplayTransport` replays it as a transport — useful for deterministic
testing against real exchange data.

## Not yet implemented

- **MetaTrader provider** — EA bridge via named pipe or socket. README stub only
  at `providers/metatrader/`.
- **Polymarket provider** — WebSocket/API client for AMM. README stub only at
  `providers/polymarket/`.
- **Risk resume** — `halt_flag_` stops the engine but there's no resume channel.
- **Generic ExchangeAdapter** — Binance is the only live venue adapter today.

## Conventions

- C++23 standard, enforced via `CMAKE_CXX_STANDARD_REQUIRED` in
  `cmake/CompilerFlags.cmake`
- Interfaces are prefixed with `I` (`IDataSource`, `IStrategy`, `IProvider`)
- New optional dependencies get their own `ENABLE_*` CMake flag + `HAS_*` define
- Optional deps are wired into `engine_core` (OBJECT library) exactly once via
  `tt_wire_optional_backends()` in `cmake/Dependencies.cmake`, with PUBLIC
  link / compile-definition scope so `HAS_*` reaches `main.cpp` in every
  executable that links `engine_core`
- The core engine must always compile with zero external dependencies; no
  `HAS_*` guards are allowed in `core/engine.{h,cpp}` or `core/engine_config.h`
- Source files that depend on an optional library are wrapped in `#ifdef HAS_*`
  and added via `target_sources()` inside `tt_wire_optional_backends`
- Runtime mode switching (`--mode backtest|shadow|live`) is kept ONLY at the
  argument-parsing edge; everywhere else, the compile-time `TT_TARGET` id
  from `core/tt_target.h` (and helpers like `target_allows_live_orders()`)
  is the source of truth
- JSON on the hot path is hand-rolled (snprintf for serialization, string
  extraction for parsing). `nlohmann/json` is linked only for static config
  files in `main.cpp` and the C API
- Lock-free SPSC rings for all inter-thread communication
- Object pools for hot-path event allocation
