# TrueTest (hft-engine)

A modular, high-performance C++23 trading engine designed for reproducible backtesting, divergence-aware shadow trading, and gated live execution from a single source tree.

The build produces three binaries — `engine_backtest`, `engine_shadow`, `engine_live` — that differ only by the compile-time `TT_TARGET` define. Live order submission is physically impossible in the non-live binaries thanks to dead-code elimination.

## Current Development Status (May 2026)

Development is active on the `testnet` branch.

**Recent major advancements** (deepdive stabilization & realism pass):
- **Queue-position awareness** (6-step implementation) — realistic orderbook queue modeling, latency, impact, and walked-book fill simulation for high-fidelity shadow vs. reality divergence tracking.
- Modernized **rich ncurses TUI** with tabbed panels, risk dashboards, and live updates (multiple UI passes).
- **Dead Man’s Switch** hardening — now attempts position flattening (`reduceOnly` MARKET) on heartbeat loss, plus related safety optimizations.
- **Tiered maintenance-margin risk** (Phase 2.2) — `MaintenanceMarginTable` + `FuturesRiskCheck` using real `/fapi/v1/leverageBracket` data; funding events wired toward P&L and risk.
- Extensive **realism wiring**, per-lot bookkeeping, hybrid executor, ExitManager, and orderbook fidelity improvements.
- UI cleanup, comment stripping, and documentation synchronization across CLAUDE.md / instructions.md.

**Production-readiness gating** (see [`prod.md`](prod.md)):
- **Phase 0 — Safe Tiny-Size Mainnet Futures**: In active execution. Collecting 15+ qualifying sessions across volatility regimes with zero unexplained drift. Full artifacts (binary log + QuestDB + notes) required. Tracker and SOP live in [`reports/phase0/`](reports/phase0/) and [`docs/futures-phase0-operator-sop.md`](docs/futures-phase0-operator-sop.md).
- **Phase 1 — Live-Safety Freeze**: Planning artifacts created, `LIVE-SAFETY SURFACE` markers applied to the 9 critical files (tt_target.h, engine core, all kill-/dead-man’s/reconciler/risk surfaces), enforcement script (`scripts/check-live-safety-freeze.sh`) wired into CI + pre-commit, CLAUDE.md policy updated. Awaiting final clean 8-hour mainnet `engine_shadow` run + two-person sign-off.
- Strong institutional-grade safety primitives already in place: compile-time live-order gating, IReconciler + clock-skew checks, layered DMS + kill-switch, venue-specific `IRiskCheck`, terminal `halt_flag_`, user-data WebSocket as source-of-truth, WorkerWatchdog.
- **Current recommendation**: Tiny-size validation and research only. Not suitable for meaningful capital or unattended operation until Phase 0/1 exit criteria are satisfied. Full gap analysis in [`docs/production-readiness-gaps.md`](docs/production-readiness-gaps.md).

The engine has excellent Binance spot + USDT-M futures support (signed REST, combined trade + depth20 streams, brackets, OCO, funding, reconciler) and a clean pluggable `IProvider` architecture. Future venue work (e.g. Solana Drift liquidation keeper) is tracked in `upcoming/`.

## Authoritative Documentation

| Document                                              | Purpose |
|-------------------------------------------------------|---------|
| [`CLAUDE.md`](CLAUDE.md)                              | **Single source of truth** for the *current* codebase: build flags & matrix, directory layout, coding conventions, hot-path rules, model-selection guidance, live-safety freeze policy. |
| [`prod.md`](prod.md)                                  | Production readiness playbook, exact phase definitions, capital-tier gates, Phase 0 command template + operator SOP, Go-Live checklist. |
| [`docs/user-manual.md`](docs/user-manual.md)          | Comprehensive technical + operator manual (architecture, safety, TUI, realism models). |
| [`docs/instructions.md`](docs/instructions.md)        | Exhaustive CLI flag reference, CMake options, provider modes, QuestDB, threading, replay, etc. |
| [`docs/target-architecture.md`](docs/target-architecture.md) | Long-term target design (read with the deviations in migration.md). |
| [`todo.md`](todo.md) + [`docs/migration.md`](docs/migration.md) | Deepdive task list and chronological file-change history. |
| [`reports/phase0/PROGRESS.md`](reports/phase0/PROGRESS.md) | Live tracker of Phase 0 qualifying sessions. |
| [`prerequisites.md`](prerequisites.md)                | Mandatory checklist before any new work touches the frozen safety surface. |

Additional deep-dive notes live under [`docs/`](docs/) (realism.md, futures-order-lifecycle.md, killswitch timeline, etc.).

## Quick Build & Run

```bash
# Default — CSV backtesting only, zero external runtime dependencies
cmake -B build
cmake --build build

./build/engine_backtest --provider local --path market_data.csv --strategy sma
```

Optional backends (enable at configure time):

```bash
cmake -B build \
  -DENABLE_BINANCE=ON \     # Binance spot + futures (REST + WS)
  -DENABLE_QUESTDB=ON \     # High-resolution persistence + ILP
  -DENABLE_LIVE_DATA=ON \   # Generic WebSocket data source
  -DENABLE_DEBUG=ON \       # Instrumentation & stage timers
  -DENABLE_NATIVE_OPT=ON    # -march=native for Release builds
cmake --build build
```

**Futures shadow example** (requires the two flags above):

```bash
./build/engine_shadow \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --persist --run-tag demo_$(date +%Y%m%d_%H%M)
```

Live execution (`engine_live`) adds `--live --api-key … --api-secret …` and a mandatory math-captcha confirmation on mainnet. Full templates and risk caps are documented in `prod.md` (Phase 0 section).

## Data Formats & Providers

- Standard OHLCV CSV for classic backtests.
- Tick-level CSV and live WebSocket feeds via the `local`, `binance`, and `binance-futures` providers (see [`docs/instructions.md`](docs/instructions.md) for exact schemas).
- Full replay from binary event logs (`--replay`).

## Testing & Quality

Large GoogleTest suite (`test_*`), golden-file regression for execution fidelity, CI-enforced hot-path rules (no JSON), live-safety-freeze script, and optional sanitizers + benchmarks.

---

This README is deliberately concise. All operational detail, architecture decisions, current invariants, and the full development log live in `CLAUDE.md` and the `docs/` tree. The project follows a strict phase-gated approach to production readiness — consult `prod.md` before increasing live capital.