# CLAUDE.md

## What this is

TrueTest — a modular C++17 engine that starts as a backtesting platform but is
designed to be reused as the foundation for different deployments: pure backtesting,
Polymarket execution, MetaTrader EA, or anything else that processes market data
through a strategy and orderbook pipeline. Each deployment attaches different
"blocks" (storage backends, execution targets, data feeds) to the same core.

## Project structure

```
hft-engine/
├── CMakeLists.txt                      # build config, multiple opt-in flags
├── vcpkg.json                          # only matters when ENABLE_POSTGRESQL=ON
├── market_data.csv                     # sample OHLCV data
├── web/
│   └── index.html                      # live WebSocket dashboard (single-file)
└── BacktestEngine/
    ├── docs/
    │   └── multithreading.md           # threading design doc (implemented)
    └── src/
        ├── main.cpp                    # entry point: TUI, CLI, provider, replay modes
        ├── core/
        │   ├── engine.h/.cpp           # event loop orchestrator (batch + streaming)
        │   ├── engine_config.h         # engine_config struct (mode, fees, threading, risk, WS)
        │   ├── event.h                 # event types: market, order, fill, tick
        │   ├── event_json.h            # snprintf-based JSON for all event types
        │   └── event_log.h             # binary event log (write + replay)
        ├── data/
        │   ├── data_source.h           # IDataSource interface (pure virtual)
        │   ├── csv_data_source.h/.cpp  # CSV OHLCV backend (default, zero deps)
        │   ├── tick_csv_data_source.h/.cpp # tick-level CSV backend
        │   ├── binary_cache_source.h/.cpp # caching decorator for any IDataSource
        │   ├── pg_data_source.h/.cpp   # PostgreSQL backend (#ifdef HAS_POSTGRESQL)
        │   ├── websocket_data_source.h/.cpp # WebSocket feed (#ifdef HAS_LIVE_DATA)
        │   ├── data_handler.h          # in-memory OHLCV column vectors
        │   └── data_loader.cpp         # load_from_csv + load_into_queue impl
        ├── execution/
        │   ├── execution_adapter.h     # IExecutionAdapter, LocalBookAdapter, ExchangeAdapter stub
        │   ├── portfolio.h/.cpp        # position tracking, PnL
        │   ├── fee_model.h             # IFeeModel (Zero, Fixed, Tiered)
        │   └── latency_model.h         # execution latency simulation
        ├── orderbook/
        │   ├── orderbook.h/.cpp        # price-time priority matching engine
        │   ├── orderbook_registry.h    # multi-symbol orderbook management
        │   └── fill_model.h            # partial fill probability modeling
        ├── strategy/
        │   ├── strategy_interface.h    # IStrategy interface
        │   ├── mean_reversion_strategy.h/.cpp
        │   ├── sma_strategy.h/.cpp
        │   └── ma_crossover_strategy.h/.cpp
        ├── analytics/
        │   ├── analytics.h/.cpp        # Welford online algo, Sharpe/Sortino/drawdown/win rate
        │   └── bar_aggregator.h        # tick-to-bar aggregation
        ├── risk/
        │   └── risk_manager.h/.cpp     # pre/post-fill checks, halt signaling
        ├── threading/
        │   ├── thread_preset.h         # 5 presets: inline, light, standard, full, extended
        │   ├── thread_config.h         # CPU affinity detection + pinning
        │   ├── worker.h                # Worker base class
        │   ├── ring_buffer.h           # lock-free SPSC ring buffer
        │   ├── logging_worker.h        # event logging (text + binary sinks)
        │   ├── risk_worker.h           # shadow portfolio + halt flag
        │   ├── stats_worker.h          # analytics accumulation + snapshots
        │   ├── observer_worker.h       # combined observer (light preset)
        │   ├── risk_stats_worker.h     # combined risk+stats (standard preset)
        │   ├── market_maker_worker.h   # MM replenish orders (extended preset)
        │   └── ws_worker.h             # WebSocket UI broadcaster (#ifdef HAS_WEB_UI)
        ├── providers/
        │   ├── provider.h              # IProvider interface
        │   ├── provider_registry.h     # factory registry + REGISTER_PROVIDER macro
        │   ├── transport.h             # IDataTransport (batch + streaming)
        │   ├── parser.h                # IDataParser<T> template
        │   ├── data_bridge.h           # transport + parser orchestrator
        │   ├── provider_event.h        # normalized event variant (bar, tick, l2, status)
        │   ├── provider_convert.h      # type conversion helpers
        │   ├── provider_sink.h         # event sink functions
        │   ├── local/                  # file-based data (CSV bar + tick)
        │   ├── binance/                # live spot market (#ifdef HAS_BINANCE)
        │   ├── metatrader/             # planned: EA bridge via named pipe/socket
        │   └── polymarket/             # planned: AMM execution
        ├── indicator/
        │   └── sma.h                   # simple moving average
        ├── market_maker/
        │   └── market_maker.h/.cpp     # liquidity seeding
        ├── types/
        │   ├── order_id.h              # global order ID generator
        │   ├── price.h                 # fixed-point price representation
        │   ├── object_pool.h           # pre-allocated event pool
        │   └── aliases.h               # type aliases
        └── debug/                      # stage timer, memory sampler, ring stats (#ifdef HAS_DEBUG)
```

## Build

```bash
# Default — no external dependencies, CSV-only
cmake -B build
cmake --build build

# With optional features
cmake -B build \
  -DENABLE_POSTGRESQL=ON \    # PostgreSQL via libpqxx (auto-fetches vcpkg)
  -DENABLE_WEB_UI=ON \        # WebSocket dashboard (requires Boost headers)
  -DENABLE_BINANCE=ON \       # Binance live streaming (Boost.Beast + OpenSSL)
  -DENABLE_LIVE_DATA=ON \     # Generic WebSocket data source
  -DENABLE_DEBUG=ON \          # Performance instrumentation (Abseil)
  -DENABLE_TSAN=ON \           # ThreadSanitizer
  -DBUILD_TESTS=ON             # GoogleTest suite (283 tests, 2 known crashers in EngineStreaming tick tests)
cmake --build build

# Run
./build/truetest                                        # interactive TUI
./build/truetest --provider local --path market_data.csv --strategy sma
./build/truetest --provider binance --symbol btcusdt --stream trade --web-ui
./build/truetest --replay event_log.bin
```

The binary is `build/truetest`.

## Architecture decisions

### Provider system (transport + parser + executor)
All external data/execution flows through the `IProvider` interface. Providers
self-register via `REGISTER_PROVIDER()` macro at static init. Each provider owns:
- `IDataTransport` — where data comes from (file, WebSocket, pipe)
- `IDataParser<T>` — how to parse it (CSV, JSON, binary)
- `IExecutionAdapter` — how to submit orders (local orderbook, exchange API)

`DataBridge<T>` orchestrates transport + parser for both batch and streaming modes.

### Storage is pluggable via IDataSource
All data backends implement `IDataSource::load_data(shared_ptr<data_handler>)`.
The core engine never touches storage directly.

### PostgreSQL is opt-in, not required
Gated behind `-DENABLE_POSTGRESQL=ON`. The `#ifdef HAS_POSTGRESQL` guard controls
both compilation and TUI menu visibility.

### BinaryCacheSource is a decorator
Wraps any `IDataSource` and caches results to a binary file.

### Engine owns no data loading
`engine::run()` expects `data_handler` to already be populated. Loading happens
before `run()` in `main.cpp`. Streaming mode (`run_streaming()`) receives records
via `DataBridge` callback.

### Event-driven pipeline
`market_event → IStrategy → order_event → orderbook → fill_event → portfolio`
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
copy of portfolio/analytics to avoid data races.

### WebSocket UI (bidirectional)
When `--web-ui` is passed, a Boost.Beast WebSocket server (default port 8765)
broadcasts all events as JSON to connected browsers. `web/index.html` renders a
live dashboard with equity curve, fills, positions, and analytics. Clients can
send JSON commands back to the engine (start, pause, stop, order, set_timeframe,
set_symbol, set_strategy) via `ws_command` structs polled by the engine.

## Implemented providers

### Local (always available)
File-based CSV provider. Supports bar (OHLCV) and tick formats. Batch mode only.

### Binance (ENABLE_BINANCE=ON)
Live WebSocket streaming from Binance spot market. Supports `trade` and `kline_1m`
streams. Parser is pure C++ (no JSON library). Executor has paper mode (working)
and live mode (stub — REST API submission not implemented).

## Not yet implemented

- **Binance live execution** — REST API order submission. Paper mode works, live
  mode is a stub in `binance_executor.h`.
- **MetaTrader provider** — EA bridge via named pipe or socket. README stub only
  at `providers/metatrader/`.
- **Polymarket provider** — WebSocket/API client for AMM. README stub only at
  `providers/polymarket/`.
- **Risk resume** — `halt_flag_` stops the engine but there's no resume channel.
- **ExchangeAdapter** — generic live exchange execution stub in `execution_adapter.h`.

## Conventions

- C++17 standard, enforced via `CMAKE_CXX_STANDARD_REQUIRED`
- Interfaces are prefixed with `I` (`IDataSource`, `IStrategy`)
- New optional dependencies get their own `ENABLE_*` CMake flag + `HAS_*` define
- The core engine must always compile with zero external dependencies
- Source files that depend on an optional library are wrapped in `#ifdef HAS_*`
  and added via `target_sources()` inside the CMake `if()` block
- No external JSON library — snprintf for serialization, hand-rolled extraction for parsing
- Lock-free SPSC rings for all inter-thread communication
- Object pools for hot-path event allocation
