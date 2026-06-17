# CLAUDE.md

## AI assistant note: model selection

For AI coding assistants working in this repo: this codebase has two
tiers of edits with different model requirements. **Full rationale,
file list, and pre-merge checklist live in `docs/MODEL.md`** (planned for Doc Phase 2 / deferred; current rules are in this file + `prod.md`). See `docs/README.md` for the current realized documentation structure.

**Default — Sonnet 4.6** is sufficient for: new strategies, indicators,
tests, CLI flags, docs, single-file refactors, provider-stack additions
that follow existing patterns, and work in `src/simulation/` + `src/providers/synthetic/`
(Monte Carlo path generation and campaign controller).

**Switch to Opus 4.7 (`/model opus`) before editing any of:**
- `src/engine/engine.{h,cpp}`, `src/engine/engine_config.h`
- Anything matching `*kill_switch*`, `*dead_mans_switch*`,
  `*reconciler*`, `*watchdog*`
- `src/core/tt_target.h` and any `TT_TARGET` / `target_allows_live_orders`
  callsite
- `src/threading/` (SPSC rings, spin policy, CPU affinity)
- `src/risk/` and any code that touches `halt_flag_`
- Hot-path code (no `nlohmann/json`, CI-enforced via
  `scripts/check-hotpath-json.sh`)
- Binance live-mode safety glue: refusal gates, time sync, OCO/bracket
  placement, REST signing, dead-man's-switch heartbeats

These areas carry **cross-file invariants** (compile-time live-order
gating, manual-recovery-only halt semantics, no hot-path allocations,
no auto-retry on safety paths) that Sonnet has a measurable tendency
to break by adding "helpful" fallback/retry logic. When in doubt:
upgrade to Opus, then downgrade after the edit.

**Monte Carlo / simulation layer note**: `src/simulation/` and the synthetic provider
are generally safe for Sonnet-level work. Cross-file invariants to respect:
deterministic per-trial seeding, no hidden shared state between trials,
`MonteCarloReporter` should remain allocation-light for large N, and
`--mc-parallel` must not be treated as a general-purpose threading primitive
(it conflicts with engine core pinning in most presets).

**Phase 1 Live-Safety Freeze (see `prod.md` Phase 1)**:
The files that carry the `LIVE-SAFETY SURFACE — Phase 1 freeze` comment
block (tt_target.h, engine.cpp + the full list in prod.md) are now under
an additional mechanical gate:
- Any edit requires the commit message to contain the token
  `LIVE_SAFETY_CCB_APPROVED`
- Two-person CCB review + a clean 4-hour mainnet `engine_shadow` run
  before merge (enforced by `scripts/check-live-safety-freeze.sh`).
Even Opus-level changes must still carry the token and go through the
CCB process. The initial freeze marking PR itself was the last
unrestricted change to this surface.

## Documentation Maintenance Rules (added with prod.md / prerequisites.md / todo.md)

- The three root governance files (`prod.md`, `prerequisites.md`, `todo.md`) + `reports/phase0/` are the single source of truth for phases, checklists, and task tracking. Keep them authoritative and up to date.
- Every PR touching the frozen safety surface (or the *description* of that surface in docs) must reference the relevant items in `todo.md` and run `./scripts/check-live-safety-freeze.sh`.
- On every phase exit declared in `prod.md`, also update `todo.md` (move/complete items), `prerequisites.md` if the checklist evolved, and the "Last updated" note in the affected docs.
- When a cross-reference is still aspirational (e.g. `docs/operations/futures-phase0-operator-sop.md` before Doc Phase 1), it must say so explicitly: "Planned for Doc Phase X – current details live in prod.md / instructions.md §N".
- Extraction rule: long-form phase/ritual/gate content lives in `prod.md` (or the dedicated SOP). `instructions.md` contains pointers + quick command templates, not duplicates.
- Anti-rot ritual: before increasing any capital tier, the exit review must include "docs verified + links resolve + `todo.md` updated".
- When new work lands in `src/simulation/` (Monte Carlo), the MC section in `docs/instructions.md` and any governance mentions (README, todo.md, prod.md) must be updated in the same PR or immediate follow-up.

See the approved documentation plan (session plan file) and `docs/README.md` for the phased rollout of the slimmed structure.

## What this is

TrueTest — a modular C++23 engine that starts as a backtesting platform but is
designed to be reused as the foundation for different deployments: pure backtesting,
Binance spot execution (mainnet + testnet), Polymarket AMM, MetaTrader EA, or
anything else that processes market data through a strategy and orderbook pipeline.
Each deployment attaches different "blocks" (storage backends, execution targets,
data feeds) to the same core.

The build produces three binaries from the same source tree —
`engine_backtest`, `engine_shadow`, `engine_live` — differing only in a
compile-time `TT_TARGET` define. Live-order paths are gated at compile time,
so only `engine_live` can ever place real orders (the others hard-reject
`--mode=live`).

## Project structure

```
hft-engine/
├── CMakeLists.txt                      # build config, opt-in feature flags
├── CMakePresets.json                   # linux-default + Windows presets
├── cmake/
│   ├── CompilerFlags.cmake             # C++23, per-config opt, tt_apply_* helpers
│   └── Dependencies.cmake              # tt_fetch_* + tt_wire_optional_backends
├── .github/workflows/ci.yml            # CI pipeline
├── market_data.csv                     # sample OHLCV data
├── docs/
│   ├── instructions.md                 # build + run + every CLI flag (master reference)
│   ├── user-manual.md                  # high-level architecture + operator overview
│   ├── (many sub-items listed below are aspirational / deferred; see docs/README.md for current realized state)
│   ├── testnet.md                      # Binance spot testnet operator guide (planned)
│   ├── db.md                           # QuestDB integration spec + DDL (planned)
│   ├── c-api.md, performance.md, perf-baseline.md, realism.md, licenses.md (planned / partial)
├── tests/                              # GoogleTest cases (incl. test_questdb_*,
│                                       #   test_binance_testnet_live)
└── src/
    ├── bin/
    │   ├── main.inc                    # CLI parsing + dispatch (shared)
    │   ├── engine_backtest/main.cpp    # `#include "../main.inc"`, TT_TARGET=BACKTEST
    │   ├── engine_shadow/main.cpp      # TT_TARGET=SHADOW
    │   └── engine_live/main.cpp        # TT_TARGET=LIVE
    ├── api/
    │   └── truetest_api.h/.cpp         # C API for embedding (BUILD_SHARED_LIB)
    ├── core/
    │   ├── event.h                     # event types: market, order, fill, tick
    │   ├── event_log.h                 # binary event log (write + replay, zstd)
    │   └── tt_target.h                 # compile-time target id (TT_TARGET),
    │                                   #   default mode, target_allows_live_orders()
    ├── engine/
    │   ├── engine.h/.cpp               # event loop orchestrator (batch + streaming)
    │   ├── engine_config.h             # engine_config struct (mode, fees, threading,
    │   │                               #   risk, QuestDB persistence, checkpoints,
    │   │                               #   backfill, log rotation, provider hooks)
    │   ├── checkpoint.h                # portfolio-state snapshot format
    │   ├── logging_worker.h            # event logging (text + binary sinks)
    │   ├── risk_worker.h               # shadow portfolio + halt flag
    │   ├── stats_worker.h              # analytics accumulation + snapshots
    │   ├── observer_worker.h           # combined observer (light preset)
    │   ├── risk_stats_worker.h         # combined risk+stats (standard preset)
    │   └── market_maker_worker.h       # MM replenish orders (extended preset)
    ├── data/
    │   ├── data_source.h               # IDataSource interface
    │   ├── csv_data_source.h/.cpp      # CSV OHLCV backend (default, zero deps)
    │   ├── tick_csv_data_source.h/.cpp # tick-level CSV backend
    │   ├── binary_cache_source.h/.cpp  # caching decorator for any IDataSource
    │   ├── websocket_data_source.h/.cpp # generic WS feed (#ifdef HAS_LIVE_DATA)
    │   ├── data_handler.h              # in-memory OHLCV column vectors
    │   ├── data_loader.cpp             # load_from_csv + load_into_queue impl
    │   └── questdb/                    # (#ifdef HAS_QUESTDB) order-lifecycle capture
    │       ├── store.h/.cpp            # QuestdbStore — DDL + per-event capture API
    │       ├── ilp_writer.h/.cpp       # ILP TCP writer (port 9009)
    │       ├── http_client.h/.cpp      # HTTP /exec client for DDL (port 9000)
    │       ├── tcp_client.h/.cpp       # raw POSIX socket helpers
    │       ├── schema.h/.cpp           # DDL for runs_meta + 6 per-run tables
    │       └── run_tag.h/.cpp          # auto-tag generator (run_<ts>_<rand>)
    ├── execution/
    │   ├── execution_adapter.h         # IExecutionAdapter, LocalBookAdapter
    │   ├── execution_bridge.h          # async order/fill bridge (live providers)
    │   ├── order_tracker.h             # lifecycle state tracking
    │   ├── order_encoder.h             # IOrderEncoder (per-venue wire format)
    │   ├── client_order_id.h           # ClientOrderIdMinter + WAF-safe scan
    │   ├── live_safety.h               # IReconciler / IKillSwitch + Noop impls
    │   ├── rate_limiter.h              # token-bucket order rate limiter
    │   ├── trade_tape_shadow_adapter.h # shadow-mode fill replay
    │   ├── queue_aware_book_adapter.h  # paper maker queue simulation (QueueAwareBookAdapter + IQueueModel)
    │   ├── queue_model.h               # Front/Uniform/BackCancelModel for maker queue
    │   ├── queue_position_model.h      # L2SnapshotQueueModel for shadow queue position
    │   ├── portfolio.h/.cpp            # position tracking, PnL
    │   ├── fee_model.h                 # IFeeModel (Zero, Fixed, Tiered)
    │   └── latency_model.h             # execution latency simulation
    ├── orderbook/
    │   ├── orderbook.h/.cpp            # price-time priority matching engine
    │   ├── orderbook_registry.h        # multi-symbol orderbook management
    │   └── fill_model.h                # partial-fill probability modeling
    ├── strategy/
    │   ├── strategy_interface.h        # IStrategy interface
    │   ├── strategy_registry.h         # factory + REGISTER_STRATEGY macro
    │   ├── strategy_factory.h
    │   ├── mean_reversion_strategy.h/.cpp
    │   ├── sma_strategy.h/.cpp
    │   ├── ma_crossover_strategy.h/.cpp
    │   └── hedge_demo_strategy.h/.cpp  # paired-leg demo for ExitManager + brackets
    ├── exits/                          # per-lot exit brackets (SL/TP/trailing)
    │   ├── exit_manager.h/.cpp         # opens brackets, fires exit_intent on hit
    │   ├── exit_intent.h               # exit-side order intent
    │   └── bracket_adapter.h           # IBracketAdapter (e.g. Binance OCO)
    ├── indicator/                      # sma, ema, rsi, bollinger
    ├── analytics/
    │   ├── analytics.h/.cpp            # Welford online algo, Sharpe/Sortino/...
    │   ├── bar_aggregator.h            # tick-to-bar aggregation
    │   └── shadow_tracker.h            # shadow-vs-exchange fill comparison
    ├── risk/
    │   └── risk_manager.h/.cpp         # pre/post-fill checks, halt signaling
    ├── threading/
    │   ├── thread_preset.h             # 5 presets: inline, light, standard, full, extended
    │   ├── thread_config.h             # CPU affinity detection + pinning
    │   ├── spin_policy.h               # spin / yield / adaptive backoff
    │   ├── worker.h                    # Worker base class
    │   └── ring_buffer.h               # lock-free SPSC ring buffer
    ├── ui/                             # ncurses-based status displays
    │   ├── ansi.h                      # ANSI helpers (used by ConsoleDashboard)
    │   ├── console_dashboard.h/.cpp    # plain/ANSI status (engine_backtest default)
    │   ├── tabbed_dashboard.h/.cpp     # rich ncurses TUI (engine_shadow / engine_live)
    │   ├── dashboard_snapshot.h        # snapshot DTO populated by engine
    │   └── panels/                     # positions, lots, brackets, fills, debug
    ├── web/                            # (#ifdef HAS_WEB) opt-in browser UI server
    │   ├── web_server.h/.cpp           # civetweb HTTP+WS server (read-only, own thread)
    │   ├── web_config.h                # bind/port/token/assets/poll config
    │   ├── snapshot_json.h/.cpp        # dashboard_snapshot → SnapshotFrame JSON
    │   ├── report_json.h/.cpp          # AnalyticsReport → ResultsReport JSON
    │   ├── json_emit.h                 # hand-rolled JSON writer (no nlohmann)
    │   ├── tools/dump_fixtures.cpp     # emits engine-shaped JSON fixtures
    │   └── frontend/                   # React + Vite + TS SPA (built → src/web/assets/)
    ├── providers/
    │   ├── provider.h                  # IProvider (lifecycle, configure, on_mid_price)
    │   ├── provider_registry.h         # factory registry + REGISTER_PROVIDER macro
    │   ├── transport.h                 # IDataTransport (batch + streaming)
    │   ├── parser.h                    # IDataParser<T> template
    │   ├── data_bridge.h               # transport + parser orchestrator
    │   ├── provider_event.h            # normalized event variant (bar, tick, l2, status)
    │   ├── provider_convert.h, provider_sink.h
    │   ├── prepend_transport.h         # decorator that injects lines before delegating
    │   ├── local/                      # file-based data (CSV bar + tick)
    │   └── binance/                    # live spot market (#ifdef HAS_BINANCE)
    │       ├── binance_provider.h          # provider with testnet refusal gates
    │       ├── binance_register.cpp        # endpoints chosen via --testnet flag
    │       ├── binance_endpoints.h         # spot_mainnet() / spot_testnet()
    │       ├── binance_transport.h         # WS market-data transport
    │       ├── binance_combined_transport.h / _parser.h  # multi-stream
    │       ├── binance_depth_parser.h      # L2 depth
    │       ├── binance_parser.h            # trade / kline parser
    │       ├── binance_executor.h          # paper + live REST order submission
    │       ├── binance_auth.h              # HMAC-SHA256, reusable EVP_MAC_CTX
    │       ├── binance_rest_client.h       # signed REST + TLS session resumption
    │       ├── binance_rest_order_transport.h # async order tx for ExecutionBridge
    │       ├── binance_user_data_transport.h  # user-data WS, listenKey keepalive
    │       ├── binance_user_data_parser.h
    │       ├── binance_order_encoder.h     # cached symbol+side+type prefix
    │       ├── binance_oco_bracket_adapter.h # OCO via /api/v3/order/oco
    │       ├── binance_kill_switch.h       # cancel-all + market-sell on shutdown
    │       ├── binance_reconciler.h        # startup drift check + testnet-reset rule
    │       ├── binance_time_sync.h         # /api/v3/time clock-skew guard
    │       ├── binance_backfill.h          # historical klines via REST
    │       ├── hybrid_executor.h           # paper-market + book-limit fills
    │       ├── binance_recorder.h          # record live WS to file
    │       └── binance_replay_transport.h  # replay recorded WS file
    ├── market_maker/
    │   └── market_maker.h/.cpp         # liquidity seeding
    ├── types/
    │   ├── order_id.h                  # global order ID generator
    │   ├── price.h                     # fixed-point price representation
    │   ├── object_pool.h               # pre-allocated event pool
    │   └── aliases.h                   # type aliases
    ├── utils/
    │   ├── log/logger.h
    │   └── retry.h
    └── debug/                          # (#ifdef HAS_DEBUG)
        └── stage_timer / memory_info / hardware_info / thread_stats /
            ring_stats / copy_tracker / debug_log / debug_report
```

## Build

```bash
# Default — CSV data only, no external runtime deps
cmake -B build
cmake --build build

# With optional features
cmake -B build \
  -DENABLE_BINANCE=ON \       # Binance live streaming + REST execution + testnet
                              #   (Boost.Beast + OpenSSL)
  -DENABLE_QUESTDB=ON \       # QuestDB persistence (raw POSIX sockets, no extra deps)
  -DENABLE_WEB=ON \           # Embedded web UI server (civetweb; --web) — see docs/web-ui.md
  -DENABLE_LIVE_DATA=ON \     # Generic WebSocket data source
  -DENABLE_DEBUG=ON \         # Performance instrumentation (Abseil)
  -DENABLE_TSAN=ON \          # ThreadSanitizer (mutually exclusive with ASAN/UBSAN)
  -DENABLE_ASAN=ON \          # AddressSanitizer
  -DENABLE_UBSAN=ON \         # UndefinedBehaviorSanitizer
  -DENABLE_NATIVE_OPT=ON \    # -march=native + unroll across all binaries (Release)
  -DENABLE_BENCHMARKS=ON \    # Google Benchmark suite
  -DBUILD_SHARED_LIB=ON \     # libtruetest shared library + C API
  -DBUILD_TESTS=ON            # GoogleTest suite + CLI integration tests
cmake --build build

# Run — three binaries, same source tree, distinct TT_TARGET at compile time
./build/engine_backtest                                   # interactive setup TUI
./build/engine_backtest --provider local --path market_data.csv --strategy sma
./build/engine_shadow   --provider binance --symbol btcusdt --stream trade
./build/engine_backtest --replay event_log.bin

# Live (mainnet) — math-captcha confirmation prompt before any order
./build/engine_live --provider binance --symbol btcusdt --stream kline_1m \
  --live --api-key KEY --api-secret SECRET

# Live (testnet) — same code path, captcha skipped
./build/engine_live --provider binance --symbol btcusdt --stream kline_1m \
  --testnet --live --api-key KEY --api-secret SECRET \
  --persist --run-tag testnet_smoke
```

Each binary links the shared `engine_core` OBJECT library and differs only
in its `TT_TARGET` define. `--mode=live` is rejected by any binary whose
`target_allows_live_orders()` returns `false` (i.e. everything except
`engine_live`). The shared library (when `BUILD_SHARED_LIB=ON`) is
`build/libtruetest.so`, C header at `src/api/truetest_api.h`.

## Architecture decisions

### Provider system (transport + parser + executor)
All external data/execution flows through the `IProvider` interface. Providers
self-register via `REGISTER_PROVIDER()` macro at static init. Each provider owns:
- `IDataTransport` — where data comes from (file, WebSocket, pipe)
- `IDataParser<T>` — how to parse it (CSV, JSON, binary)
- `IExecutionAdapter` — how to submit orders (local orderbook, exchange API)

`IProvider` also exposes `configure(engine_config&)`, `on_mid_price(sym, px)`,
`get_reconciler()`, `get_kill_switch()`, and `lifecycle_state()` so
provider-specific wiring (backfill, hybrid executor book-seeding, WebSocket
state, live-mode safety hooks) stays out of the core engine.

`DataBridge<T>` orchestrates transport + parser for both batch and streaming modes.
`PrependTransport` is a decorator that yields a fixed set of lines before
delegating to an inner transport (used by `BinanceProvider` to inject historical
backfill bars as synthetic kline JSON, invisible to the engine).

### Strategies self-register
`REGISTER_STRATEGY()` in `strategy/strategy_registry.h` mirrors the provider
registry. `sma`, `mean-reversion`, `ma-crossover`, and `hedge-demo` register
at static init. `main.inc` supports multi-strategy mode via
`--strategy sma,mean-reversion`.

### Storage is pluggable via IDataSource
All data backends implement `IDataSource::load_data(shared_ptr<data_handler>)`.
The core engine never touches storage directly.

### Persistence: QuestDB (opt-in)
`-DENABLE_QUESTDB=ON` (`HAS_QUESTDB`) at compile time, `--persist` at runtime.
Captures every order-lifecycle event (submission, status transition, fill,
rejection, cancellation, amendment, and funding) to a local QuestDB instance.
All `record_*` calls go directly to `QuestdbStore` and are serialized behind
one `std::mutex` inside the store. Writes are handed to a batched `IlpWriter`
(its own background flush thread). Schema: one shared `runs_meta` table +
six (plus funding) per-run tables prefixed with `--run-tag`. Wire:
ILP/TCP (port 9009) for ingest, HTTP /exec (port 9000) for DDL — raw POSIX
sockets, no client library. **Soft-fail** on unreachable daemon: warning to
stderr, persistence disabled, run continues. Spec in `docs/db.md`.

### Per-lot exit brackets (ExitManager)
Strategies can attach SL/TP/trailing brackets to entry lots via
`ExitManager`. When a price target is hit, an `exit_intent` is emitted and
routed back through the order pipeline. On Binance live, brackets can be
mirrored to exchange-side OCO orders via `BinanceOcoBracketAdapter` so they
survive a process crash. Demo: `hedge-demo` strategy.

### BinaryCacheSource is a decorator
Wraps any `IDataSource` and caches results to a binary file.

### Engine owns no data loading
`engine::run()` expects `data_handler` to already be populated. Loading happens
before `run()` in `main.inc`. Streaming mode (`run_streaming()`) receives records
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

QuestDB writes (when `--persist` is set) happen via direct calls protected by
a mutex inside `QuestdbStore`. The heavy I/O is offloaded to the `IlpWriter`'s
internal flush thread. No dedicated `QuestDbWorker` exists. CPU affinity and
worker presets apply to the other workers (Logging, Risk, Stats, etc.).

### Portfolio checkpointing
When `checkpoint_path` is set, the engine writes a binary snapshot of portfolio
state every N events (default 10k). Setting `resume_checkpoint_path` at
construction pre-populates the portfolio from the referenced file, enabling
resume-after-crash workflows. Orthogonal to QuestDB persistence.

### Status displays (TUIs + opt-in web UI)
Two TUIs ship in the tree, both opt-in via `--status-format`:
- **`ConsoleDashboard`** (`src/ui/console_dashboard.{h,cpp}`) — plain or
  ANSI-coloured status; default on `engine_backtest`.
- **Rich tabbed ncurses TUI** (`src/ui/tabbed_dashboard.{h,cpp}` +
  `src/ui/panels/`) — wired into `engine_shadow` and `engine_live` via
  `tt_wire_rich_tui()` in `CMakeLists.txt`. Tabs: positions, lots,
  brackets, fills, debug. Operator hotkeys: pause/resume, flatten,
  kill-switch (live only). Pulls structured snapshots from
  `engine::snapshot_dashboard()` and reuses the same atomics + recent-event
  ring as `ConsoleDashboard`.

`--status-format off|plain|tui|ndjson|auto` (default `auto`: tty → tui).
Structured machine-readable output goes through `--status-format ndjson`.

**Opt-in web UI** (`-DENABLE_WEB=ON` + `--web`, `HAS_WEB`): a browser cockpit +
backtest-review SPA served by an embedded **civetweb** HTTP+WS server (`src/web/`),
on its own thread. It is a third read-only consumer of the same
`engine::snapshot_dashboard()` / `get_analytics()` seam the TUI uses — zero
hot-path work, no contact with the frozen live-safety surface, **no order/flatten/
kill routes on any target** (read-only by construction). WS `/stream` pushes
`SnapshotFrame` JSON; REST `/api/snapshot` + `/api/results`. Hand-rolled JSON
(`src/web/json_emit.h`), no nlohmann. React+Vite+TS frontend under
`src/web/frontend/` (plain CSS, not Tailwind). This is a new, deliberately
decoupled design — distinct from the old Boost.Beast WebSocket UI + React SPA
that were removed (whose hot-path coupling this layout avoids). Full guide:
`docs/web-ui.md`.

### C API for embedding
`src/api/truetest_api.h` exposes an opaque handle + JSON-config surface
(`tt_create_engine`, `tt_run`, `tt_get_results`, `tt_last_error`,
`tt_destroy`) intended for Python (ctypes/cffi) or Node.js (ffi-napi) host
processes. Built as `libtruetest.so` when `-DBUILD_SHARED_LIB=ON`. The C
API is not currently wired through the QuestDB capture path or the rich
TUI — it produces analytics + results JSON only.

## Implemented providers

### Local (always available)
File-based CSV provider. Supports bar (OHLCV) and tick formats. Batch mode only.

### Binance (ENABLE_BINANCE=ON)
Live WebSocket streaming from Binance spot market. Supports `trade`, `kline_*`,
combined, and depth (L2) streams. Parser is pure C++ (no JSON library on the
hot path — only nlohmann/json is linked, and only for static config files).
Historical-bar backfill via REST, injected into the live stream through
`PrependTransport`.

Endpoints chosen at registration time: `--testnet` (or any
`testnet`-containing host) selects `binance::spot_testnet()`
(`stream.testnet.binance.vision` / `testnet.binance.vision`); otherwise
`spot_mainnet()`.

Execution modes:
- **Paper** (default): orders logged, fills simulated from last price.
- **Hybrid**: paper market orders + local-book limit fills (default for
  backtest/shadow/paper modes). Owns synthetic book-seeding around the mid
  price.
- **Live**: signed REST order submission against `/api/v3/order` via
  `BinanceRestClient`; user-data WebSocket streams `executionReport` for
  fills via `BinanceUserDataTransport`. Cancel and modify wired. Requires
  `--live` flag, `--api-key`, `--api-secret`, and the math-captcha
  confirmation (skipped on `--testnet`).

Live-mode safety hooks (`engine.cpp` queries them via the provider):
- `BinanceReconciler` — `/api/v3/account` drift check at startup; refuses
  to start if cash or position drifts past `--reconcile-tolerance-bps`.
  Testnet downgrades the venue-zero / local-non-zero case to a
  `[TESTNET-RESET]` warning so the monthly testnet wipe doesn't block
  startup.
- `BinanceKillSwitch` — `cancel_all_and_flatten()` on shutdown:
  `DELETE /api/v3/openOrders`, `GET /api/v3/account`, `MARKET SELL` the
  free base balance within `--kill-switch-deadline-ms`.
- `BinanceProvider::open()` (live only) refusal gates: clock-skew check,
  unsigned `exchangeInfo` symbol probe, SQL-keyword scan of the
  `clientOrderId` prefix (testnet WAF rejects `OR/AND/SELECT/DROP/UNION/--`).

OCO bracket: `BinanceOcoBracketAdapter` mirrors `ExitManager` brackets to
exchange-side OCO orders so SL/TP survive a process crash. Currently uses
`/api/v3/order/oco`; migration to `/api/v3/orderList/oco` is tracked
separately.

Live WS recording + replay: `BinanceRecorder` captures a live stream to file;
`BinanceReplayTransport` replays it as a transport — useful for deterministic
testing against real exchange data.

Operator guide for testnet: `docs/testnet.md` (planned / deferred; current details in `docs/instructions.md` and `prod.md`).

### Binance Futures USDT-M (ENABLE_BINANCE=ON)
Sibling provider registered as `binance-futures`. Same compile-time gate
as spot — the live order path lives in `engine_live` only. Different
endpoint stack (`fapi.binance.com` / `fstream.binance.com` for mainnet,
`testnet.binancefuture.com` / `stream.binancefuture.com` for testnet)
and different keys (separate email-signup at testnet.binancefuture.com,
not the spot portal's GitHub OAuth).

Endpoints chosen at registration time: `--testnet` selects
`binance::usdm_testnet()`; `binancefuture` host substring also flips
the registry into testnet mode automatically (the testnet WS host
`stream.binancefuture.com` carries no `testnet` token).

Execution modes mirror spot — paper / hybrid / live — but with
futures-specific wire format (`/fapi/v1/order`, `STOP_MARKET`/`STOP`
mapping, no `timeInForce` for `MARKET` or `STOP_MARKET`, no
`positionSide` emitted in one-way mode).

Live-mode safety hooks (futures-specific implementations):
- `BinanceFuturesReconciler` — reads `availableBalance` from
  `/fapi/v2/account` and signed `positionAmt` from
  `/fapi/v2/positionRisk?symbol=...`. **No spot-style testnet-reset
  shortcut** — futures testnet does not wipe on the same cadence as
  spot, and the spot heuristic would mask real drift.
- `BinanceFuturesKillSwitch` — `DELETE /fapi/v1/allOpenOrders`, then
  reads positionRisk and submits a `reduceOnly=true MARKET` on the
  opposite side sized to `|positionAmt|`. **Closes positions; does
  not sweep balances** — there's no "free base" on a derivatives book
  to sell.
- `BinanceFuturesProvider::open()` refusal gates: clock-skew check
  via `/fapi/v1/time`, `/fapi/v1/exchangeInfo` symbol probe,
  `/fapi/v1/positionSide/dual` hedge-mode gate (refuses if
  `dualSidePosition=true`; one-way only in v1).
- Startup advisories (warnings, not refusals) via
  `binance::futures::compute_advisories`: margin-mode mismatch
  (`--margin-type ISOLATED|CROSSED`) and liquidation distance
  (`--liquidation-warn-pct`, default 5%). Tolerate
  `liquidationPrice == 0` / `markPrice == 0` (unfunded testnet
  accounts, just-opened positions).
- `BinanceFuturesDeadMansSwitch` — server-side `POST
  /fapi/v1/countdownCancelAll` with a heartbeat thread. Bounds
  catastrophic-shutdown order exposure (SIGKILL / OOM / kernel
  panic / network gone) from "indefinite" to "≤ countdown_ms after
  last successful heartbeat." Default ON at 30s countdown / 10s
  heartbeat (`--dead-man-countdown-ms` / `--dead-man-heartbeat-ms`
  / `--disarm-deadman`). The heartbeat thread itself is monitored
  by `WorkerWatchdog` (`src/threading/worker_watchdog.h`); if the
  heartbeat hangs for `3 × heartbeat_ms`, the watchdog fires
  `halt_flag_` so the orderly kill-switch runs before the venue
  countdown cancels orders mid-quote. Cancels orders only — does
  NOT flatten positions; the kill-switch's flatten step is the
  other half of the safety net.
- Pre-trade venue risk check (`FuturesRiskCheck`,
  `src/risk/futures_risk_check.h`) — notional / leverage /
  projected-liquidation-distance caps applied per outgoing order
  before `RiskManager::check_order`. Refusals emit
  `rejection_event` with reason `venue_risk_reject` and the engine
  continues (reject, not halt). Off by default; enable via
  `--max-notional` / `--max-leverage` / `--min-liq-distance-pct`.

Bracket adapter: `BinanceFuturesBracketAdapter` places SL+TP as two
separate conditional orders (`STOP_MARKET` + `TAKE_PROFIT_MARKET`),
both with `closePosition=true reduceOnly=true`. Placement is two
non-atomic POSTs (no OCO endpoint on futures); cancel-other-when-fires
is provided exchange-side by `closePosition=true` semantics — when one
leg triggers and brings the position to zero, Binance auto-cancels
every other `closePosition=true` order on the symbol. Partial-fraction
intents (`qty_fraction != 1.0`) are declined; engine-side ExitManager
remains the only enforcer for those.

Operator guide: `docs/futures-testnet.md` (planned / deferred; current details in `docs/instructions.md`, `prod.md`, and `prerequisites.md`).

## Not yet implemented

- **Risk resume** — `halt_flag_` stops the engine but there's no resume channel.
- **Generic ExchangeAdapter** — Binance (spot + futures) is the only live venue family today.
- **QuestDB hard-fail** — daemon unreachable currently downgrades to a
  warning; "refuse to start when `--persist` is set but QuestDB is down"
  is documented in `docs/db.md` as a follow-up (deferred; current soft-fail behavior described in `docs/instructions.md` QuestDB section).
- **COIN-M (inverse) futures** — separate stack (`dapi.binance.com`,
  `dstream.binance.com`, settles in base asset). Will land as a sibling
  provider, not a flag on `binance-futures`.
- **Hedge mode** — futures provider refuses if the account is in hedge
  mode. Adding support means `positionSide=LONG/SHORT` plumbed through
  the encoder and a second per-symbol position bucket in lot bookkeeping.
- **Position-based pre-trade risk** — existing `RiskManager` is
  balance-based (cash). Futures notional / leverage / liquidation-distance
  caps are tracked as a backlog item; needs to land before the futures
  live path is used against real money.

## Stack decisions

- **JSON library: `nlohmann/json` (config-time only).** Used in two files
  (`src/bin/main.inc` CLI config, `src/api/truetest_api.cpp` C API config +
  result serialization). Zero hot-path usage — verified by grep across
  `core/`, `engine/`, `execution/`, `strategy/`, and `providers/binance/`.
  CI script `scripts/check-hotpath-json.sh` enforces this.
- **Persistence: QuestDB (opt-in via `ENABLE_QUESTDB=ON` + `--persist`).**
  Replaced SQLite + PostgreSQL backends, which are gone. Zero-dependency
  client (raw POSIX sockets, no libpq or libpqxx). `--persist` is opt-in
  per session; default off.
- **Status display: ncurses tabbed TUI** for `engine_shadow` /
  `engine_live`; headless / pipe-friendly output via `--status-format ndjson`.
- **Web UI: embedded civetweb HTTP+WS server (opt-in via `ENABLE_WEB=ON` +
  `--web`).** Read-only browser cockpit + backtest review (`src/web/`), React+
  Vite+TS frontend (`src/web/frontend/`, plain CSS). Third consumer of the
  `snapshot_dashboard()` seam — off the hot path, no control routes. This is a
  fresh, decoupled design; the older Boost.Beast WebSocket UI + React SPA that
  were removed are unrelated to it. Spec: `docs/web-ui.md`.
- **Live venue: Binance (mainnet + spot testnet).** New venues will be
  sibling providers under `src/providers/`; the core does not need to
  change to add one.
- **TLS: OpenSSL 1.1.1 / 3.x** via Boost.Beast for Binance. Session
  resumption cached per `BinanceRestClient` and per `BinanceUserDataTransport`
  so reconnects skip the asymmetric handshake.

## Conventions

- C++23 standard, enforced via `CMAKE_CXX_STANDARD_REQUIRED` in
  `cmake/CompilerFlags.cmake`
- Interfaces are prefixed with `I` (`IDataSource`, `IStrategy`, `IProvider`,
  `IReconciler`, `IKillSwitch`, `IBracketAdapter`, `IOrderEncoder`)
- New optional dependencies get their own `ENABLE_*` CMake flag + `HAS_*` define
- Optional deps are wired into `engine_core` (OBJECT library) exactly once via
  `tt_wire_optional_backends()` in `cmake/Dependencies.cmake`, with PUBLIC
  link / compile-definition scope so `HAS_*` reaches every executable that
  links `engine_core`
- The core engine must always compile with zero external dependencies; no
  `HAS_*` guards are allowed in `engine/engine.{h,cpp}` or `engine/engine_config.h`
- Source files that depend on an optional library are wrapped in `#ifdef HAS_*`
  and added via `target_sources()` inside `tt_wire_optional_backends`
- Runtime mode switching (`--mode backtest|shadow|live`) is kept ONLY at the
  argument-parsing edge; everywhere else, the compile-time `TT_TARGET` id
  from `core/tt_target.h` (and helpers like `target_allows_live_orders()`)
  is the source of truth
- JSON on the hot path is hand-rolled (snprintf for serialization, string
  extraction for parsing). `nlohmann/json` is linked only for static config
  files in `main.inc` and the C API (CI-enforced)
- Lock-free SPSC rings for all inter-thread communication
- Object pools for hot-path event allocation
- Live-mode safety: halt is terminal (no resume channel), kill-switch on
  shutdown is mandatory, stderr warnings on kill-switch failure require
  manual operator intervention
