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
├── CMakeLists.txt                      # build config, PostgreSQL is opt-in
├── vcpkg.json                          # only matters when ENABLE_POSTGRESQL=ON
├── market_data.csv                     # sample OHLCV data
└── BacktestEngine/
    ├── docs/
    │   └── multithreading.md           # threading design doc (not implemented yet)
    └── src/
        ├── main.cpp                    # entry point, TUI menu, source/strategy wiring
        ├── core/
        │   ├── backtest_core.h/.cpp    # event loop: market → strategy → order → fill
        │   └── event.h                 # event types: market, signal, order, fill
        ├── data/
        │   ├── data_source.h           # IDataSource interface (pure virtual)
        │   ├── csv_data_source.h/.cpp  # CSV backend (default, zero deps)
        │   ├── binary_cache_source.h/.cpp # caching decorator for any IDataSource
        │   ├── pg_data_source.h/.cpp   # PostgreSQL backend (#ifdef HAS_POSTGRESQL)
        │   ├── data_handler.h          # in-memory OHLCV column vectors
        │   └── data_loader.cpp         # load_from_csv + load_into_queue impl
        ├── execution/
        │   └── portfolio.h/.cpp        # position tracking, PnL
        ├── orderbook/
        │   └── orderbook.h/.cpp        # price-time priority matching engine
        ├── strategy/
        │   ├── strategy_interface.h    # IStrategy interface
        │   ├── mean_reversion_strategy.h/.cpp
        │   ├── sma_strategy.h/.cpp
        │   └── ma_crossover_strategy.h/.cpp
        ├── indicator/
        │   └── sma.h                   # simple moving average
        ├── market_maker/
        │   └── market_maker.h/.cpp     # initial liquidity seeding
        ├── pricing/
        │   ├── black_scholes.h/.cpp    # unused, placeholder
        │   └── perform_calculations.h/.cpp
        └── utils/
            ├── market_data.h           # market_data_bar struct
            └── log/log.cpp             # logging stub (not implemented)
```

## Build

```bash
# Default — no external dependencies, CSV-only
cmake -B build
cmake --build build

# With PostgreSQL support
cmake -B build -DENABLE_POSTGRESQL=ON
cmake --build build
```

The binary is `build/truetest`.

## Architecture decisions

### Storage is pluggable via IDataSource
All data backends implement `IDataSource::load_data(shared_ptr<data_handler>)`.
The core engine never touches storage directly. New backends (API feeds, binary
formats, etc.) implement this interface and get wired in `main.cpp`.

### PostgreSQL is opt-in, not required
Gated behind `-DENABLE_POSTGRESQL=ON`. Without it, the entire vcpkg/libpqxx
dependency chain is skipped. The `#ifdef HAS_POSTGRESQL` guard controls both
compilation and the TUI menu visibility.

### BinaryCacheSource is a decorator
It wraps any `IDataSource` and caches the result to a binary file. Currently
used to cache PostgreSQL queries, but works with any source.

### backtest_core owns no data loading
`backtest_core::run()` expects `data_handler` to already be populated. It throws
if data is empty. Loading happens before `run()` in `main.cpp`.

### Event-driven pipeline
`market_event → IStrategy → order_event → orderbook → fill_event → portfolio`
All events flow through a `std::queue<event_pointer>` in batches of 1000.

## Orphaned files (safe to delete)

- `BacktestEngine/src/data/db_connection.h` — old PostgreSQL header, replaced by pg_data_source.h
- `BacktestEngine/src/data/database_connection.cpp` — old implementation, replaced by pg_data_source.cpp

Nothing includes or compiles these anymore.

## Not yet implemented (design exists)

- **Multithreading** — see `BacktestEngine/docs/multithreading.md`. Core 0 runs
  the hot path, worker threads (logging, risk, market maker, stats) consume via
  lock-free SPSC ring buffers. Hardware detection and portable pinning are open
  concerns documented there.
- **Logging** — `utils/log/log.cpp` is a stub. Will become a Core 1 worker thread.
- **Risk checking** — will be a Core 2 worker with a halt/resume channel back to the event loop.
- **Statistical analysis** — purely observational worker on its own core.

## Conventions

- C++17 standard, enforced via `CMAKE_CXX_STANDARD_REQUIRED`
- Interfaces are prefixed with `I` (`IDataSource`, `IStrategy`)
- New optional dependencies get their own `ENABLE_*` CMake flag
- The core engine must always compile with zero external dependencies
- Source files that depend on an optional library are wrapped in `#ifdef HAS_*`
  and added via `target_sources()` inside the CMake `if()` block
