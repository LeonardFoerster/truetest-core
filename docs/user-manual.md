# TrueTest User Manual

**TrueTest (hft-engine)** — A high-performance, modular C++23 trading engine supporting backtesting, shadow (paper) trading, and live execution from a single source tree.

This document serves as the primary operator-facing manual and technical overview. It covers architecture, installation, configuration, usage, risk management, safety features, performance characteristics, and commercial readiness recommendations.

---

# Project Overview

TrueTest (hft-engine) is a modular, high-performance C++23 trading engine that implements backtesting, shadow (paper) trading, and live execution from a single shared codebase. The engine produces three specialized binaries (`engine_backtest`, `engine_shadow`, `engine_live`) via a compile-time `TT_TARGET` gate that completely removes the ability to place real orders from non-live builds.

**Core value proposition**: A production-grade, auditable foundation for algorithmic trading that reuses the exact same hot path, risk engine, analytics, and order lifecycle for reproducible backtests, divergence-aware shadow runs, and real-money execution. Designed for serious retail and semi-professional quant traders who need C++ performance, realistic microstructure modeling, and institutional-grade safety nets without building infrastructure from scratch.

**Key technical highlights**:
- C++23, zero-allocation hot path using `ObjectPool` and lock-free SPSC `RingBuffer` (64k slots)
- Three compile-time targets with dead-code elimination for live-order safety
- Pluggable `IProvider` / `IStrategy` / `IExecutionAdapter` architecture
- Advanced Binance USDT-M futures support (dead-man's switch, kill switch, position reconciler, bracket adapter with `reduceOnly` + `closePosition`)
- Rich models for latency, impact, queue position, fees, and realistic fills
- Optional high-resolution persistence via QuestDB ILP + binary event logs (zstd compressed)
- Lock-free multi-threaded worker architecture with CPU pinning and configurable spin policies
- Rich ncurses tabbed TUI (shadow/live) and ANSI dashboard (backtest)
- Monte Carlo simulation engine for stochastic backtesting, strategy robustness testing, and risk-distribution analysis (GBM paths, deterministic multi-trial campaigns, object reuse, experimental parallelism)

# Architecture & Design

```
Market Data
    │
    ▼
┌──────────────────────┐
│ IProvider / IDataSource│  (local CSV, Binance WS+REST, replay)
│  (Transport + Parser) │
└──────────┬───────────┘
           │ ProviderEvent / market_event / tick_event / l2_update
           ▼
┌─────────────────────────────────────────────────────────────┐
│                      Engine Core (engine.cpp)                │
│  • ObjectPool allocation                                     │
│  • Strategy dispatch (on_market / on_tick / on_l2)           │
│  • Pre-trade RiskManager + venue IRiskCheck                  │
│  • ExecutionBridge → IExecutionAdapter                       │
│  • Fill path → Portfolio + ExitManager + Analytics           │
│  • Post-fill risk + shadow divergence tracking               │
└──────────┬──────────────────────────────────────────────────┘
           │ event_pointer (lock-free)
           ▼
┌─────────────────────────────────────────────────────────────┐
│                  Lock-free RingBuffer (65536)                │
│                  (multiple rings: main, risk, stats, mm, etc)│
└──────────┬──────────────────────────────────────────────────┘
           │
    ┌──────┴──────┬──────────┬──────────┬──────────┬──────────┐
    ▼             ▼          ▼          ▼          ▼          ▼
LoggingWorker  RiskWorker  StatsWorker Observer  MMWorker   (optional)
    │             │          │          │          │
    ▼             ▼          ▼          ▼          ▼
File/QuestDB   Halt logic  Metrics   TUI/Dash   Quote mgmt
```

**Main components**:
- **Core engine** (`src/engine/`): Orchestrates the event loop, worker threads, and lifecycle.
- **Providers** (`src/providers/`): Data + execution boundary. `local` for CSV/tick replay; `binance` and `binance-futures` for live streaming + REST execution; `synthetic` for on-demand GBM path generation (standalone or via Monte Carlo campaigns).
- **Strategies** (`src/strategy/`): Pluggable via `REGISTER_STRATEGY` macro. Can emit `order_event` and `exit_intent` vectors for brackets.
- **Execution layer** (`src/execution/`): `IExecutionAdapter`, `Portfolio`, `OrderTracker`, realistic models (`FeeModel`, `FillModel`, `LatencyModel`, `ImpactModel`, `QueuePositionModel`).
- **Order book & matching** (`src/orderbook/`): `Orderbook` + `FillModel` for backtest realism.
- **Risk & exits** (`src/risk/`, `src/exits/`): `RiskManager`, `FuturesRiskCheck`, `ExitManager`, `BracketAdapter`.
- **Workers & threading** (`src/threading/`): Lock-free ring buffers, `Worker` base, watchdog, CPU affinity presets.
- **Analytics & UI** (`src/analytics/`, `src/ui/`): Real-time metrics, adverse selection, report generation, tabbed ncurses dashboard with panels for positions, orders, L2, risk, brackets, etc.
- **Safety systems** (Binance futures): `BinanceFuturesReconciler`, `DeadMansSwitch`, `KillSwitch`, user-data WebSocket as source of truth.

**Data flow**:
1. Data source emits raw events → parser produces `market_event` / `tick_event` / `l2_update_event`.
2. Engine dispatches to primary + additional strategies.
3. Strategy returns `order_event` → RiskManager + venue `IRiskCheck` (pre-trade) → `IExecutionAdapter`.
4. Adapter produces `fill_event` (synthetic in backtest/shadow, real via REST+user-data WS in live).
5. Fill updates `Portfolio`, triggers `ExitManager` bracket placement, updates `Analytics`, posts to rings.
6. Workers consume rings asynchronously (logging, risk stats, TUI, market making).

# Core Features

**Backtesting**:
- Local CSV (OHLCV and tick-level) and binary cache replay via `local` provider
- Deterministic replay from recorded event logs (`--replay`, `--replay-from/--to`)
- Configurable realism models (latency, impact, queue position, fees, fill simulation)
- Golden regression tests + full event logging for audit
- Walked-book impact using real L2 depth when available

**Shadow / Paper Trading**:
- Real-time Binance mainnet streaming (trade, kline, depth) with `TradeTapeShadowAdapter`
- Divergence tracking between simulated and real fills (`shadow_tracker`)
- Records raw tapes for later deterministic replay
- Optional QuestDB persistence with `run_tag` for post-session analysis
- No real orders sent; full visibility into what would have happened

**Live Trading** (compile-time gated to `engine_live` only):
- Full Binance spot and USDT-M futures REST + WebSocket execution
- `HybridExecutor` and `BinanceRestOrderTransport` for order submission
- User-data WebSocket as authoritative source of truth (ORDER_TRADE_UPDATE)
- Position/order reconciliation at startup against `/fapi/v2/positionRisk`
- Automatic bracket placement using futures `closePosition=true` + `reduceOnly`
- Dead-man's switch (crash protection with countdown)
- Kill switch (emergency flatten with deadline)
- Pre-trade venue risk caps (`--max-notional`, `--max-leverage`, liquidation distance)
- Live-money math confirmation gate + red warning banner
- Rate limiter and time sync

**Stochastic Backtesting (Monte Carlo)**:
- Synthetic GBM path generation via the `synthetic` provider (usable standalone with `--provider synthetic --mc-params "..."` or as part of full campaigns)
- Full multi-trial campaigns with deterministic per-trial seeding via `--monte-carlo --mc-trials N --mc-model gbm --strategy ...`
- Reuses the complete existing strategy, engine, realism models, analytics, ExitManager, and QuestDB surfaces for each trial
- Performance features: object reuse between trials (`--mc-reuse-objects`) and experimental parallel execution (`--mc-parallel`, recommended only with `--thread-preset inline`)
- Reporter produces per-trial + aggregate P&L, Sharpe, max drawdown, win rate, etc. (text + compact JSON)
- Strong caveats: synthetic L2 is stylized (constant spread + noise), no automatic calibration from historical data, parallel mode has non-deterministic ordering and threading restrictions, QuestDB support is currently campaign-summary only

**Risk Management**:
- `RiskManager`: max position, daily loss, trade frequency, unrealized loss
- Venue-specific `IRiskCheck` (futures liquidation distance, notional caps)
- Post-fill risk stats worker
- `ring_drop_policy::halt_on_drop` for safety-critical rings in shadow/live

**Other advanced features**:
- Exit intents / bracket management (SL/TP/trailing per entry, scale-outs)
- Market maker worker and adverse selection tracker
- Multi-strategy mode (`--strategy sma,mean-reversion,structure-continuation`)
- CPU pinning + configurable threading presets (inline/light/standard/full/extended)
- Binary event log (zstd) + operational text logs with rotation
- Rich tabbed TUI (shadow/live) with 10+ specialized panels
- C API (`src/api/`) for embedding / language bindings
- ~310 unit tests + golden regression suite

# Installation & Compilation

**Requirements**:
- C++23 compiler (GCC 13+, Clang 16+, MSVC 2022+)
- CMake ≥ 3.22
- vcpkg (recommended) or system packages for optional dependencies
- Linux (primary), macOS, Windows (MSVC) supported

**Core dependencies** (always built):
- CLI11, nlohmann-json, zstd (via vcpkg or FetchContent)

**Optional features** (enable via CMake flags):
- `ENABLE_BINANCE`: Binance WS/REST (Boost.Beast + OpenSSL)
- `ENABLE_QUESTDB`: QuestDB ILP persistence
- `ENABLE_DEBUG`: StageTimer + memory instrumentation
- `ENABLE_NATIVE_OPT`: `-march=native` + aggressive opts (live binary only)
- `BUILD_TESTS`, `ENABLE_BENCHMARKS`, `BUILD_SHARED_LIB`

**Build steps**:

```bash
# Minimal (CSV backtesting only, no network)
cmake -B build
cmake --build build -j

# Full-featured (Binance + QuestDB + debug + native opt)
cmake -B build -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON \
               -DENABLE_DEBUG=ON -DENABLE_NATIVE_OPT=ON \
               -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target engine_backtest engine_shadow engine_live

# With tests
cmake -B build -DBUILD_TESTS=ON -DENABLE_BINANCE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**Output binaries**:
- `./build/engine_backtest`
- `./build/engine_shadow`
- `./build/engine_live`

**Sanitizers**: `ENABLE_ASAN`, `ENABLE_TSAN`, `ENABLE_UBSAN` (mutually exclusive).

# Configuration & Setup

**Primary interface**: Command-line flags (CLI11). No mandatory config file, but strategies accept `--param key=value` and JSON strategy config is supported in some paths.

**Key configuration categories**:
- `--provider local|binance|binance-futures` + `--path`, `--symbol`, `--stream`, `--depth-stream`
- `--strategy name` (or comma-separated list) + `--param`
- Realism models: `--fee`, `--fill-model`, `--latency-model`, `--impact-model`, `--queue-model`
- Threading: `--thread-preset`, `--no-pin`, `--spin-policy`
- Risk: `--max-position`, `--daily-loss-limit`, `--max-trades-per-hour`, futures-specific caps
- Persistence: `--persist`, `--questdb-host`, `--log-events`, `--record`
- Replay: `--replay`, `--replay-from`, `--replay-to`
- Live safety: `--dead-man-countdown-ms`, `--kill-on-drop`, API key/secret via env or flags

**External services**:
- Binance API key/secret (read-only for shadow, trading permissions for live)
- QuestDB (optional, for high-resolution analytics)
- For futures: account must be in **one-way mode**

**Environment**:
- `TRUETEST_*` variables for some overrides
- Credentials can be provided via CLI or environment (see `scripts/check-credentials.sh`)

## QuestDB Persistence

TrueTest supports optional high-resolution persistence to a QuestDB instance using the InfluxDB Line Protocol (ILP) for ingestion (default port 9009) and HTTP for schema/DDL operations (default port 9000). Build support is enabled via the CMake flag `-DENABLE_QUESTDB=ON`.

At runtime, persistence is activated with `--persist --run-tag <name>` (optionally combined with `--persist-strict` for hard-fail semantics and automatic local ILP fallback file writing on outages). The engine performs time-based flushing (default ~150 ms cadence, configurable via `--questdb-flush-ms`) from the main reporting loops in addition to count-based batching. A minimal but effective health surface is exposed in the TUI (connected state, pending lines, fallback lines written, age since last successful flush).

Per-run tables (e.g. `{run_tag}_orders`, `{run_tag}_fills`, `{run_tag}_events`, `{run_tag}_rejections`) are created automatically with `PARTITION BY DAY` and a designated timestamp column. A shared `runs_meta` table (now using WEEK partitioning) records campaign summaries, including rich analytics fields such as max drawdown, Sharpe, Sortino, profit factor, and win rate written on shutdown. A generic `_events` table (Phase 3) enables capture of strategy decisions, risk actions, and other logic beyond pure order lifecycle.

QuestDB is explicitly a secondary, queryable analytics and observability store. The binary zstd-compressed event log (`--record`) is the authoritative, durable audit trail. In non-strict mode, QuestDB unavailability at startup causes graceful degradation (persistence is disabled for the session with a warning). In strict mode, startup or persistent write failures cause a hard exit.

For operational details, golden queries, retention/TTL recommendations, soak testing with failure injection, and post-run reconciliation, see `docs/db.md` and `docs/questdb-multi-week-hardening-guide.md`.

# Usage Examples

**1. Simple backtest (local CSV, SMA strategy)**

```bash
./build/engine_backtest \
    --provider local \
    --path market_data.csv \
    --strategy sma \
    --sma-period 20 \
    --initial-balance 10000 \
    --fee tiered --maker-rate 0.0002 --taker-rate 0.0004
```

Expected output: ANSI dashboard showing equity curve, trade count, win rate, final P&L, and a generated report (if configured). Deterministic results with same seed.

**2. Shadow (paper) trading session on real Binance mainnet**

```bash
./build/engine_shadow \
    --provider binance-futures \
    --symbol btcusdt \
    --stream kline_1m \
    --depth-stream depth20@100ms \
    --strategy mean-reversion \
    --persist --run-tag shadow_btc_$(date +%F) \
    --log-events logs/shadow_$(date +%F).bin \
    --record tapes/btc_$(date +%F).txt \
    --thread-preset standard
```

Records raw tape while running live shadow fills via trade tape. Compares simulated vs real microstructure. Safe — no orders sent.

**3. Live trading session (futures, with full safety)**

```bash
./build/engine_live \
    --provider binance-futures \
    --symbol btcusdt \
    --stream trade \
    --depth-stream depth20@100ms \
    --strategy ma-crossover \
    --mode live \
    --api-key $BINANCE_API_KEY \
    --api-secret $BINANCE_API_SECRET \
    --dead-man-countdown-ms 15000 \
    --max-notional 5000 \
    --min-liq-distance-pct 1.5 \
    --initial-balance 5000 \
    --risk-unwind
```

**Critical**: Only `engine_live` binary can submit real orders. Operator must correctly solve a random math problem after seeing a prominent red "LIVE TRADING — REAL MONEY" warning. All other binaries hard-reject live order paths at compile time.

# Performance & Benchmarks

**Observed characteristics from architecture and instrumentation**:
- Hot path uses pre-allocated `ObjectPool` events; no `new`/`malloc` on critical path.
- Lock-free SPSC rings (65536 slots) for inter-thread handoff; workers spin/yield/adaptive.
- CPU pinning + thread presets reduce jitter.
- LTO + `-O3` in Release; optional `-march=native` on live binary.
- StageTimer (ENABLE_DEBUG) provides per-stage microsecond breakdown for profiling.
- Binary event logging with zstd compression adds minimal overhead when enabled.

**Typical throughput**: Capable of handling full-depth Binance streams (trade + 20-level depth @ 100ms) plus strategy + risk + analytics on a modern 8–16 core CPU with <1 ms median event-to-worker latency (subject to measurement).

**Further optimization opportunities** (non-exhaustive):
- Integrate mimalloc/jemalloc for general allocations outside hot path.
- Increase hot-path inlining and reduce virtual dispatch in strategy/adapter paths.
- Batch QuestDB ILP writes and ring consumer wakeups.
- NUMA-aware allocation and ring placement on multi-socket systems.
- Vectorized indicator updates and orderbook walks.

# Risk Management & Safety Features

**Built-in controls**:
- Pre-trade: `RiskManager` limits + venue `IRiskCheck` (futures notional, leverage, liquidation distance).
- Startup reconciliation (Binance futures): local vs exchange position/order state; refuses to start on mismatch.
- Source-of-truth: User-data WebSocket drives portfolio and bracket state in live mode.
- Dead-man's switch: automatic flatten if engine heartbeat or connection fails within countdown.
- Kill switch: emergency cancel-all + reduceOnly market close with deadline.
- ExitManager: strategy-declared brackets (SL/TP/trailing) automatically attached with `reduceOnly`.
- Live-money gate: interactive math confirmation + red banner; only present in `engine_live`.
- Ring drop policy: `halt_on_drop` on safety-critical rings in shadow/live.
- Rate limiting, time sync, and HMAC-signed REST requests.
- Full audit trail via binary event logs and QuestDB.

**Notable missing or partial mechanisms**:
- No exchange-side position limits or max drawdown auto-liquidation beyond what the venue provides.
- Limited circuit-breaker logic for extreme volatility (can be added via custom risk check).
- No automatic credential rotation or scoped API key enforcement in the engine itself.
- Shadow mode does not yet simulate funding rate or mark-price liquidation risk for futures.

# Limitations & Known Issues

- TUI requires a capable terminal (ncurses); backtest falls back to simpler ANSI dashboard.
- Live trading currently limited to Binance spot and USDT-M futures (other venues require new providers).
- Strategy library (self-registering): SMA, mean-reversion, MA crossover, breakout/coiled-spring, adaptive-hybrid, structure-continuation (plus supporting indicators: EMA regime, stochastic, swing detector). No built-in portfolio optimization or ML inference.
- Realism in backtest/shadow is only as good as the configured models and data quality; L2 replay for impact is powerful but data-intensive.
- No native Windows GUI or installer; command-line + TUI only.
- QuestDB is the only supported high-resolution persistence backend (soft-fail if unavailable).
- C API exists but is minimal; full Python/Rust bindings are incomplete.
- Documentation is extensive in `docs/` but lacks a single polished user manual (work in progress).
- Some advanced futures features (funding rate integration into P&L, liquidation price simulation) are partially implemented.

# Suggested Improvements for Commercial Release

**Documentation & UX**:
- Complete a single `USER_MANUAL.md` + PDF version covering every CLI flag, model, and futures checklist.
- Add interactive TUI help and `--help` examples for common workflows.
- Video walkthroughs of record → replay → shadow → live progression.

**Product & Distribution**:
- Provide pre-built binaries or easy installers (AppImage, .deb, Homebrew, Docker images) for major platforms.
- Implement a licensing system (license key file or online activation) with tiers: Backtest (free), Shadow (paid), Live + Support (premium).
- Create a strategy marketplace or template repository so users can drop in `.so`/`.dll` strategies without recompiling the engine.
- Add a simple Qt or web-based GUI wrapper (optional paid addon) for users uncomfortable with CLI/TUI.

**Reliability & Polish**:
- Expand golden regression coverage to include full futures bracket + reconciliation scenarios.
- Add structured JSON logging + Prometheus metrics export for production monitoring.
- Implement automatic tape rotation + cloud upload helpers.
- Hardened error handling: never silently drop critical events; always surface with actionable messages.
- Credential manager (encrypted store) instead of raw env/CLI secrets.

**Monetization ideas**:
- Free core engine with open-source core; charge for premium providers (additional exchanges), advanced analytics modules, and priority support.
- "Pro" tier includes pre-tuned realistic models and curated historical tape packs.
- Enterprise tier: custom venue integration, on-prem deployment support, SLA.

# Unique Selling Points for Gumroad

- **True single-binary reuse**: The exact same engine binary (modulo compile-time gate) powers backtest → shadow → live with bit-level behavioral parity where possible.
- **Institutional safety without the price tag**: Dead-man's switch, kill switch, position reconciler, and user-data WS source-of-truth are production features rarely found in retail tools.
- **C++23 performance edge**: Lock-free rings, object pools, zero hot-path allocations, and optional native tuning deliver microsecond-class event handling that Python/JavaScript engines cannot match.
- **Realistic microstructure modeling**: Queue position, walked-book impact, latency stacking, and trade-tape shadow fills let you see realistic slippage before risking capital.
- **Audit-grade observability**: Binary zstd event logs + optional QuestDB give you a complete, queryable, tamper-evident record of every decision and fill.
- **Futures-first design**: First-class USDT-M support with `reduceOnly` brackets, one-way mode enforcement, liquidation distance checks, and funding awareness (partial).
- **Extensible but safe architecture**: New providers and strategies register via macros; live order paths are physically impossible to enable in backtest/shadow builds.

# Full File Index

**Root & Build**:
- `CMakeLists.txt` — Main build script producing three TT_TARGET binaries + optional shared library.
- `cmake/CompilerFlags.cmake`, `Dependencies.cmake` — Centralized C++23 flags, sanitizers, and optional backend wiring.
- `vcpkg.json` — Dependency manifest for optional features (Binance, live data).
- `README.md`, `onboarding.md`, `CLAUDE.md` — High-level and authoritative internal documentation.

**Core Engine** (`src/engine/`, `src/core/`):
- `engine.{h,cpp}`, `engine_config.h` — Central orchestrator, worker management, mode handling (10 files total in engine/).
- `tt_target.h` — Compile-time target definition and `target_allows_live_orders()` gate.
- Event, logging, and worker headers (`event.h`, `event_log.h`, `logging_worker.h`, etc.).

**Providers** (`src/providers/`):
- `provider.h`, `provider_registry.h`, `data_bridge.h` — Core interfaces and registration.
- `local/` (4 files) — CSV/tick file transport + parser for backtesting and replay.
- `binance/` (32 files) — Complete spot + USDT-M futures implementation: parsers, transports, executors, order encoders, bracket adapter, dead-man's switch, kill switch, reconciler, safety checks, user-data handling, time sync, REST client, hybrid executor.

**Strategies & Indicators** (`src/strategy/`, `src/indicator/`):
- `strategy_interface.h`, `strategy_registry.h`, `strategy_factory.h` — Extension points and registration.
- Concrete strategies: `sma_strategy`, `mean_reversion_strategy`, `ma_crossover_strategy`, `hedge_demo_strategy`, `market_maker` (13 files total).
- Indicators: `sma.h`, `ema.h`, `rsi.h`, `bollinger.h`.

**Execution & Order Management** (`src/execution/`, `src/exits/`, `src/orderbook/`):
- `execution_adapter.h`, `portfolio.{h,cpp}`, `order_tracker.h`, `execution_bridge.h`.
- Models: `fee_model.h`, `fill_model.h`, `latency_model.h`, `impact_model.h`, `queue_model.h`, `queue_position_model.h`.
- `exit_manager.{h,cpp}`, `bracket_adapter.h`, `exit_intent.h`.
- `orderbook.{h,cpp}`, `fill_model.h`, `orderbook_registry.h` (20 files total in execution/).

**Risk & Safety** (`src/risk/`):
- `risk_manager.{h,cpp}`, `futures_risk_check.h`, `live_safety.h`.

**Analytics, UI & Persistence**:
- `analytics/` (10 files): `analytics.cpp`, `report_generator`, `adverse_selection_tracker`, `shadow_tracker`, ASCII widgets.
- `ui/` (8 + 10 panels): `tabbed_dashboard`, `console_dashboard`, specialized panels for L2, positions, brackets, risk, health, etc.
- `data/questdb/` (12 files): ILP writer, schema, store, HTTP/TCP clients for high-resolution persistence.
- `data/` (12 files): `csv_data_source`, `tick_csv_data_source`, `binary_cache_source`, `websocket_data_source`.

**Threading & Types** (`src/threading/`, `src/types/`):
- `ring_buffer.h`, `worker.h`, `worker_watchdog.h`, `thread_preset.h`, `spin_policy.h`.
- `object_pool.h`, `order_id.h`, `price.h`, `aliases.h`.

**API & Tools**:
- `src/api/truetest_api.{h,cpp}` — Minimal C API for embedding.
- `tools/python/` — Early Python bindings (incomplete).
- `benchmarks/bench_main.cpp` — Performance micro-benchmarks.
- `tests/` (~40 test files + fixtures + golden): Comprehensive unit, integration, golden regression, and live testnet tests.

**Documentation** (`docs/`):
- `demo-trading-workflow.md`, `futures-order-lifecycle.md`, `futures-testnet.md`, `realism.md`, `performance.md`, `strategy-validation.md`, `testnet.md`, `licenses.md`, and others — detailed operational and design guidance.

---

**Document generated**: Full codebase analysis of the TrueTest / hft-engine trading platform.
**Intended audience**: Operators, quant developers, and commercial product reviewers.
