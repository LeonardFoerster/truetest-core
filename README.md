# TrueTest (hft-engine)

C++23 trading engine. Single source tree produces three binaries via compile-time `TT_TARGET`: `engine_backtest`, `engine_shadow`, `engine_live`. Live orders physically impossible except on `engine_live` (dead-code elimination).

**Intended use**: private personal research and retail tool only. Not enterprise, institutional or production software for others. Primary mature capabilities: Monte Carlo simulation, backtesting, shadow divergence analysis. Live paths (`engine_live`) are experimental, tiny-size, attended, at own risk. Phase 0/1 describe personal discipline only.

## Binaries

| Binary           | TT_TARGET | Live Orders | Default |
|------------------|-----------|-------------|---------|
| engine_backtest  | BACKTEST  | Impossible  | backtest |
| engine_shadow    | SHADOW    | Impossible  | shadow  |
| engine_live      | LIVE      | Allowed     | live    |

Defined in `src/core/tt_target.h`. Gated in `src/bin/main.inc`.

## Status (2026)

Monte Carlo integrated from `monte-carlo` branch (mainline; MC-01/MC-02 landed).

**Web UI**: opt-in on `feature/web-ui` (`-DENABLE_WEB=ON` + `--web`). Read-only civetweb server + React SPA. Reuses `snapshot_dashboard()` seam. Off hot path, no order routes, no frozen surface changes. See `docs/web-ui.md`.

### Phases

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 0 (Tiny-Size Mainnet Futures) | 0/15 qualifying | Paused during MC work; gates unchanged. Full artifacts required. See `prod.md`, `reports/phase0/`. |
| Phase 1 (Live-Safety Freeze) | Enforced | 10 frozen files + `scripts/check-live-safety-freeze.sh` (CI + pre-commit). Token + CCB + clean shadow run required for edits. |
| Risk / DMS (R-*, S-*) | Partial | Tiered margin landed; position limits, funding wiring, liq calc, DMS flatten pending. See `todo.md`. |

Safety primitives: compile-time gating, IReconciler, DMS + kill-switch (flatten opt), venue `IRiskCheck`, terminal `halt_flag_`, user-data WS as truth, WorkerWatchdog.

## Build

```bash
cmake -B build
cmake --build build
```

Key options:

| Flag | Effect |
|------|--------|
| -DENABLE_BINANCE=ON | Binance spot + futures |
| -DENABLE_QUESTDB=ON | QuestDB ILP + schema |
| -DENABLE_WEB=ON | civetweb + --web UI (feature/web-ui) |
| -DENABLE_LIVE_DATA=ON | Generic WS |
| -DENABLE_DEBUG=ON | Stage timers |
| -DENABLE_NATIVE_OPT=ON | -march=native (live) |
| -DBUILD_TESTS=ON | GoogleTest |

Web assets (after ENABLE_WEB):
```bash
cd src/web/frontend && npm ci && npm run build
```

## Providers

| Name            | Sources                     | Execution          |
|-----------------|-----------------------------|--------------------|
| local           | OHLCV / tick CSV            | paper / hybrid     |
| binance         | REST + WS (trade/depth)     | live + paper       |
| binance-futures | REST + WS (trade/depth20)   | live + brackets    |
| synthetic       | GBM (on demand)             | MC / backtest      |

Replay: `--replay` from zstd binary logs. Realism models: latency, impact, queue (l2-snapshot), fill.

## Safety Surface (Phase 1)

Frozen (single source of truth in script + markers):

- src/core/tt_target.h
- src/engine/engine.cpp
- src/providers/binance/binance_futures_{provider,dead_mans_switch,kill_switch,reconciler}.h
- src/risk/{risk_manager,futures_risk_check}.h
- src/execution/live_safety.h
- src/threading/worker_watchdog.h

All edits require `LIVE_SAFETY_CCB_APPROVED` token, CCB review, clean multi-hour `engine_shadow` run.

## Data Flow (relations)

main.inc → IProvider (or synthetic/MC) → IDataSource / DataBridge → engine → strategy → RiskManager + IRiskCheck → IExecutionAdapter (LocalBook / QueueAware / Hybrid / Bridge) → portfolio + ExitManager (per-lot via opener) + analytics → workers (rings) + snapshot.

`reset_for_next_trial` (MC) clears portfolio/exits/analytics/order state.

## UI

- Console (backtest)
- Tabbed ncurses (shadow/live)
- Web (`--web`): live cockpit + backtest review SPA. Same snapshot data. Read-only.

## Strategies

Registered via macro (`src/strategy/strategy_registry.h`):

sma, ma-crossover, mean-reversion, breakout, coiled-spring, larry_connor, hedge-demo, adaptive-hybrid, structure-continuation.

Indicators: sma/ema/rsi/stochastic/bollinger/atr/swing/rolling.

## Documentation

| File                          | Content |
|-------------------------------|---------|
| CLAUDE.md                     | Build matrix, conventions, model selection, freeze policy, web |
| prod.md                       | Phases, gates, Phase 0 template + ritual |
| todo.md                       | Tasks (P0/P1/MC/R/S/D/A); ref on frozen PRs |
| docs/instructions.md          | CLI flags, providers, MC, threading, realism |
| docs/web-ui.md                | Web UI flags, endpoints, architecture, safety |
| reports/phase0/PROGRESS.md    | Phase 0 tracker (0/15) |
| docs/user-manual.md           | Architecture + operator overview |
| docs/production-readiness-gaps.md | Remaining gaps |

Root governance files + reports/phase0/ + CLAUDE.md are authoritative. MC and web UI do not relax Phase 0/1 gates or safety surface.

## Quick Examples

```bash
./build/engine_backtest --provider local --path market_data.csv --strategy sma
```

```bash
./build/engine_shadow --provider binance-futures --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms --persist --run-tag ...
```

Web (ENABLE_WEB):
```bash
./build/engine_shadow ... --web --web-assets src/web/assets
# http://127.0.0.1:8080/
```

Live: add `--live --api-key ... --api-secret ...` + captcha. Full templates in prod.md.

## Testing

~300 GoogleTest cases, golden regression (execution fidelity), CI hot-path (no JSON), live-safety-freeze script, optional sanitizers + benchmarks.

Consult `prod.md` before increasing live capital. All frozen-surface work requires the CCB token and clean shadow run per `scripts/check-live-safety-freeze.sh`.