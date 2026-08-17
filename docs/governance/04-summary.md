# TrueTest Engine — Feature Summary

**TrueTest (hft-engine)** is a modular, high-performance C++23 trading engine that delivers reproducible backtesting, divergence-aware shadow trading, and gated live execution from a **single source tree**.

Three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) are produced from the identical codebase and differ only by the compile-time `TT_TARGET` define. Live-order paths are physically removed via dead-code elimination in non-live targets.

**Intended Use & Scope**: Full version in [01-prod.md](01-prod.md). Private personal research/retail tool only. Live experimental/tiny/attended. See 01-prod.md.

---

## Core Architecture & Safety

- **Compile-time live-order gate** (`src/core/tt_target.h`): Only `engine_live` can ever submit real orders. All other binaries hard-reject `--mode=live`.
- **Zero-allocation hot path**: `ObjectPool` for events + lock-free SPSC `RingBuffer` (65536 slots) for inter-thread handoff.
- **Layered risk & safety**:
  - `RiskManager` (pre/post-fill checks, position/loss/frequency limits)
  - Venue `IRiskCheck` (futures notional, leverage, liquidation-distance, tiered maintenance margin)
  - `IReconciler` (startup position/order drift check; refuses start on mismatch beyond tolerance)
  - `IKillSwitch` (emergency cancel-all + reduceOnly flatten with deadline)
  - `DeadMansSwitch` (Binance `countdownCancelAll` heartbeat protection)
  - `WorkerWatchdog` (monitors critical threads; promotes heartbeat loss to terminal halt)
- **Terminal halt semantics**: `halt_flag_` is write-once; only manual restart clears it. No auto-resume on safety paths.
- **Hot-path discipline** (CI-enforced): No `nlohmann/json`, no heap allocations on event loop, single-producer rings only.
- **User-data WebSocket as source of truth** for live fills/positions.

---

## Deployment Targets (TT_TARGET)

| Binary           | TT_TARGET | Live Orders | Typical Use                          |
|------------------|-----------|-------------|--------------------------------------|
| `engine_backtest`| BACKTEST  | Impossible  | Historical CSV replay, MC campaigns  |
| `engine_shadow`  | SHADOW    | Impossible  | Real-time paper trading vs. exchange |
| `engine_live`    | LIVE      | Allowed     | Real-money execution (with captcha)  |

---

## Providers & Data Sources

- **`local`** (always available): OHLCV bar CSV and tick-level CSV.
- **`binance`** / **`binance-futures`**: Binance Spot + USDT-M Futures (trade, kline, depth20, combined streams). Full REST + WebSocket execution, backfill, recording/replay. Requires `ENABLE_BINANCE`.
- **`bitget`** / **`bitget-futures`**: Bitget UTA USDT-M futures (trade, kline, books depth). Requires `ENABLE_BITGET`. Demo/paptrading via `--demo`/`--testnet`. See `docs/operations/03-bitget-demo.md`.
- **`bitunix`** / **`bitunix-futures`**: Bitunix futures MD + paper/shadow (Phase 0–1; live order routing refused). Requires `ENABLE_BITUNIX`. See `docs/platforms/bitunix.md`.
- **`synthetic`** / **`montecarlo`**: On-demand GBM path generation (standalone or Monte Carlo campaigns). Configurable `mu`, `sigma`, steps, initial price.
- **Binary replay**: `--replay` from zstd-compressed event logs (deterministic, with time slicing).
- Generic `WebSocketDataSource` was removed as unwired; live market data is owned by the concrete venue providers.

All data flows through the pluggable `IProvider` / `IDataTransport` / `IDataParser` / `IExecutionAdapter` architecture. New venues are sibling providers; core engine stays unchanged.

---

## Strategies & Indicators

Strategies self-register via `REGISTER_STRATEGY` macro and support multi-strategy mode (`--strategy sma,mean-reversion`).

**Current strategies** (in `src/strategy/`):
- `sma` — Simple moving average
- `ma-crossover`
- `mean-reversion`
- `breakout` / `coiled-spring`
- `adaptive-hybrid` — retired prototype; unavailable pending a safe rebuild
- `structure-continuation`
- `larry_connor` (underscore in registry name)
- `hedge-demo` (paired legs + ExitManager demo)

**Indicators** (`src/indicator/`):
- SMA, EMA, EMA-regime
- RSI, Stochastic, ATR
- Bollinger Bands
- Swing detector, rolling extremes

Strategies can emit both `order_event`s and `exit_intent` vectors (for per-lot brackets).

---

## Execution Realism (Backtest & Shadow)

- **Fee models**: Zero, Fixed, Tiered (maker/taker configurable).
- **Latency models**: Configurable strategy-to-eligible and wire latency.
- **Impact & fill models**: Walked-book impact (real L2 when available), probabilistic partial fills.
- **Queue-position modeling** (6-step implementation):
  - `QueueAwareBookAdapter`
  - `L2SnapshotQueueModel` (front/uniform/back-cancel heuristics)
  - Realistic maker queue tracking + adverse selection
- **Hybrid executor**: Paper market orders + local-book limit fills (synthetic book seeding around mid).
- Per-lot attribution, shadow divergence tracking (`shadow_tracker`), adverse selection tracker.

---

## Order Management & Exits

- **ExitManager**: Per-lot SL/TP/trailing brackets attached by strategy.
- **Bracket adapters**:
  - Spot: Binance OCO (`/api/v3/order/oco`)
  - Futures: two independent conditional algo legs (`STOP_MARKET` +
    `TAKE_PROFIT_MARKET`) with `closePosition=true`; Binance forbids quantity
    and `reduceOnly` in this shape, and sibling cancellation is not assumed
- Full order lifecycle: submit → ack → partial → filled / canceled / rejected / amended.
- `OrderTracker` + rich status machine.
- Client order ID minting (WAF-safe prefixes).

---

## Monte Carlo / Stochastic Simulation (integrated from monte-carlo branch)

- Full multi-trial campaigns: `--monte-carlo --mc-trials N --mc-model gbm`
- Reuses the **complete** engine, strategies, realism models, `ExitManager`, analytics, and (partial) QuestDB surfaces per trial.
- Deterministic per-trial seeding.
- Performance: object reuse between trials (`--mc-reuse-objects`), experimental parallel execution (`--mc-parallel`, recommended only with `--thread-preset inline`).
- `MonteCarloReporter`: per-trial + aggregate stats (P&L distribution, Sharpe/Sortino, win rate, profit factor, max DD, etc.) in text + compact JSON.
- **Caveats** (documented): stylized synthetic L2 (constant spread + noise), no auto-calibration from history, parallel mode has pinning/threading restrictions. Research & robustness tool only — does not relax Phase 0/1 gates.

---

## Persistence & Audit

- **Binary event log** (`--record`, zstd compressed): Authoritative, replayable, tamper-evident audit trail of every market/order/fill event.
- **QuestDB ILP** (opt-in via `ENABLE_QUESTDB=ON` + `--persist --run-tag`):
  - High-resolution per-run tables: orders, fills, order_status, rejections, position_snapshots, funding_events.
  - Shared `runs_meta` with campaign analytics.
  - Soft-fail by default; strict mode available.
  - Batched `IlpWriter` background thread.
- **Checkpoints**: Periodic binary diagnostic snapshots only. Resume v1 is disabled until a complete v2 state/replay contract exists.

---

## Observability & UI

- **Rich ncurses TUI** (default for shadow/live, `--status-format tui`): Tabbed panels (Overview, Positions, Lots/Brackets, Orders, L2, Risk, Strategy, Health, Debug, etc.). Hotkeys for pause/flatten/kill.
- **ANSI dashboard** (default for backtest): Plain or colored console output.
- **ndjson** machine-readable output.
- **Analytics**: Welford online stats, Sharpe/Sortino/ProfitFactor, adverse selection, shadow-vs-reality divergence, bar aggregation, report generator (JSON/CSV).
- **Debug instrumentation** (`ENABLE_DEBUG`): `StageTimer` (per-stage µs breakdown), ring stats (drops/HWM), memory/copy trackers, thread stats.
- QuestDB health surface in TUI (connected, pending, fallback lines, age).

---

## Threading & Performance

- 5 auto-detected **thread presets** (`--thread-preset`): `inline` (1–2 cores), `light`, `standard`, `full`, `extended` (8+ cores).
- CPU affinity pinning + configurable spin/yield/adaptive backoff policies.
- Workers consume via dedicated lock-free SPSC rings (Logging, Risk, Stats, RiskStats, Observer, MarketMaker).
- Optional `-march=native`, LTO, PGO-friendly builds.
- Hot path stays on Core 0 in most presets.

---

## Risk Management (Futures-First)

- Pre-trade venue caps: `--max-notional`, `--max-leverage`, `--min-liq-distance-pct`.
- Tiered maintenance-margin table loaded from `/fapi/v1/leverageBracket`.
- Daily loss limit + optional `--risk-unwind` flag (flatten positions on risk halt).
- Post-fill risk worker + `ring_drop_policy::halt_on_drop` on critical rings.
- Funding events partially wired (P&L, QuestDB, snapshots; full analytics/risk circuit breakers in progress).

---

## Build & Extensibility

**Minimal build** (zero external runtime deps):
```bash
cmake -B build && cmake --build build
```

Source registration is centralized in `cmake/Sources.cmake` (core + tests; no globs). Optional venue/backend TUs live in `cmake/Dependencies.cmake`. Common real setups use presets (`cmake --list-presets`); preset binaries land in `out/build/<preset>/`, ad-hoc builds in `build/`. Build presets default to one job, and `ctest --preset linux-tests` is the serial test entry point.

**Key CMake options**:
- Venues: `ENABLE_BINANCE`, `ENABLE_BITGET`, `ENABLE_BITUNIX`
- Feature: `ENABLE_QUESTDB`, `ENABLE_WEB`, `ENABLE_IMGUI`, `ENABLE_DEBUG`, `ENABLE_LTO`, `ENABLE_NATIVE_OPT` (all three engines when ON)
- Compatibility: `ENABLE_LIVE_DATA` is accepted only as a deprecated no-op; select a venue option for live data.
- `BUILD_TESTS`, `ENABLE_BENCHMARKS`, `BUILD_SHARED_LIB`
- Sanitizers: `ENABLE_ASAN` / `TSAN` / `UBSAN` (TSAN exclusive with ASAN/UBSAN; ASAN+UBSAN OK)

- **C API** (`src/api/`): Opaque handle + JSON config surface (`tt_create_engine`, `tt_run`, `tt_get_results`, …) for embedding (Python ctypes, Node ffi, etc.).
- **Strategy & Provider registries**: Macro-based self-registration. Drop-in `.cpp` files + re-link; register in `Sources.cmake`.
- **GoogleTest** suite (hundreds of cases) + golden regression (`test_golden_regression`, execution fidelity, L2, brackets, venue safety, etc.).
- Extensive live testnet + mainnet shadow tests.

---

## Current Status (2026)

- Strong production primitives already in place (compile-time gating, layered safety, reconciler, DMS + kill, user-data truth, per-lot exits, queue realism, MC engine).
- **Phase 0** (tiny-size mainnet futures validation): 0/15 qualifying sessions. Phase 0 collection was paused during priority work on the monte-carlo branch (gates/ritual unchanged). Ritual + templates ready in `01-prod.md` + `reports/phase0/`.
- **Phase 1** Live-Safety Freeze: the engine/execution/provider safety surface is under the mechanical CCB gate (`scripts/check-live-safety-freeze.sh`). Token + two-person review + clean multi-hour shadow are required for all future edits.
- Monte Carlo simulation capabilities (integrated from the monte-carlo branch) are available for research and strategy robustness (object reuse, reporter, synthetic provider).
- **Recommendation**: Research, strategy robustness testing, and tiny-size validation only. Not suitable for meaningful capital until Phase 0/1 exit criteria are met.

---

## Authoritative References

- `README.md` — High-level entry point
- `AGENTS.md` — Single source of truth for current codebase, conventions, model-selection rules
- `01-prod.md` — Production readiness playbook, exact Phase 0/1 gates, Go-Live table, philosophy
- `../reference/02-user-manual.md` — Operator + technical overview
- `../reference/01-instructions.md` — Exhaustive CLI flag reference + usage
- `03-todo.md` (thin) + `docs/todos/` (detailed numbered files) — Living task list (every frozen-surface PR must reference items; see docs/todos/00-OVERVIEW.md)
- `02-prerequisites.md` — Mandatory pre-PR checklist for the safety surface

See also `reports/phase0/PROGRESS.md` and the `scripts/phase0/` tooling.

---

*This file is a synthesized feature list for quick reference. All operational, safety, and governance detail lives in the documents listed above.*

**Last updated: 2026-07 (docs overhaul)** — docs/ is now the central authoritative documentation home. Dupe "Intended Use" slimmed to pointer. Cross-refs fixed.
