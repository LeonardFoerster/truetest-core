# TrueTest

**A modular C++23 trading engine for reproducible backtesting, divergence-aware shadow trading, and gated live execution — from a single source tree.**

Three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) are produced from the same codebase. They differ only by the compile-time `TT_TARGET` define. Live-order paths are physically eliminated via dead-code elimination in non-live targets.

> **Intended use**: Private personal research and retail tool only. Not enterprise, institutional, or production software for others.  
> Primary mature capabilities: Monte Carlo simulation, high-fidelity backtesting, and shadow divergence analysis.  
> Live paths (`engine_live`) are experimental, tiny-size, fully attended, and used at your own risk. Phase 0/1 describe personal discipline and evidence-gathering practices.

## The Three Binaries

| Binary           | TT_TARGET | Live Orders | Primary Use                          |
|------------------|-----------|-------------|--------------------------------------|
| `engine_backtest` | BACKTEST  | Impossible  | Historical replay, MC campaigns      |
| `engine_shadow`   | SHADOW    | Impossible  | Real-time paper trading vs. exchange |
| `engine_live`     | LIVE      | Allowed     | Real-money execution (with safeguards) |

The `TT_TARGET` mechanism and `target_allows_live_orders()` gate are defined in `src/core/tt_target.h`.

## Core Capabilities

- **Monte Carlo simulation** — Stochastic backtesting with GBM paths, deterministic multi-trial campaigns, object reuse, and experimental parallelism. Use `--monte-carlo --mc-trials N`.
- **High-fidelity backtesting** — Local CSV (OHLCV + tick), binary log replay, walked-book impact, configurable realism models (latency, queue position, market impact, fees, fills).
- **Shadow trading** — Real-time Binance streaming with `TradeTapeShadowAdapter`. Tracks divergence between simulated and observed fills.
- **Strong safety architecture** (compile-time + runtime):
  - `TT_TARGET` dead-code elimination for live orders
  - Position/order reconciler, Dead Man's Switch, Kill Switch
  - Venue `IRiskCheck` (futures notional, leverage, liquidation distance)
  - Terminal `halt_flag_`, user-data WebSocket as source of truth, `WorkerWatchdog`
- **Observability** — Binary zstd event logs, optional QuestDB ILP, rich ncurses TUI (shadow/live), optional read-only web UI.
- **Zero-allocation hot path** — `ObjectPool` + lock-free SPSC `RingBuffer` (64k slots). CI-enforced.

## Quick Start

```bash
# Minimal build (CSV backtesting only)
cmake -B build
cmake --build build -j
```

```bash
# Run a simple backtest
./build/engine_backtest \
  --provider local \
  --path market_data.csv \
  --strategy sma
```

```bash
# Shadow trade against Binance futures (paper only)
./build/engine_shadow \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade \
  --depth-stream depth20@100ms \
  --persist --run-tag my_shadow_run
```

For live execution you must add `--live` together with API credentials and complete the interactive math captcha. The full recommended Phase 0 template and operator ritual are documented in `docs/governance/01-prod.md`.

### Web UI

The optional web interface (civetweb + React SPA) serves a read-only live cockpit and backtest report viewer. It reuses the same snapshot data as the TUI and cannot place or modify orders.

Enable at build time:

```bash
cmake -B build -DENABLE_WEB=ON
cmake --build build
cd src/web/frontend && npm ci && npm run build
```

Run with:

```bash
./build/engine_shadow ... --web --web-assets src/web/assets
# Then open http://127.0.0.1:8080/
```

## Build System

Modernized CMake setup (single source of truth in `cmake/Sources.cmake`; no globs).
Adding a core `.cpp` or its unit test is done in that one file.

**Path contract:**
- **Presets** write to `out/build/<presetName>` (e.g. `out/build/linux-tests`).
- **Ad-hoc** `cmake -B build ...` still uses the classic `build/` tree (docs/scripts default).

Common configurations as presets:

```bash
cmake --preset linux-tests
cmake --build --preset linux-tests -j
# binaries: out/build/linux-tests/engine_*

cmake --preset linux-binance-questdb   # Binance + QuestDB + tests
cmake --preset linux-bitget            # Bitget UTA futures
cmake --preset linux-bitunix           # Bitunix MD/shadow
cmake --preset linux-venues            # Binance + Bitget + Bitunix
cmake --preset linux-providers-questdb # all venues + QuestDB
cmake --preset linux-web
cmake --preset linux-asan
cmake --preset linux-release-native
# cmake --list-presets  # full inventory
```

Key CMake options:

| Option                    | Effect                                       |
|---------------------------|----------------------------------------------|
| `-DENABLE_BINANCE=ON`     | Binance spot + USDT-M futures                |
| `-DENABLE_BITGET=ON`      | Bitget UTA USDT-M futures                    |
| `-DENABLE_BITUNIX=ON`     | Bitunix futures (MD/shadow Phase 0–1)        |
| `-DENABLE_QUESTDB=ON`     | QuestDB ILP writer + schema                  |
| `-DENABLE_WEB=ON`         | Embedded civetweb + `--web` UI               |
| `-DENABLE_DEBUG=ON`       | Stage timers + instrumentation               |
| `-DENABLE_NATIVE_OPT=ON`  | `-march=native` on all three engines (Release)|
| `-DBUILD_TESTS=ON`        | GoogleTest suite                             |
| `-DBUILD_SHARED_LIB=ON`   | `libtruetest.so` C API                       |

After enabling the web UI, build the frontend once:

```bash
cd src/web/frontend && npm ci && npm run build
# or: cmake --build out/build/linux-web --target web_assets
```

See `docs/reference/01-instructions.md` for the complete reference.

## Providers

| Provider          | Data Sources                     | Execution                  |
|-------------------|----------------------------------|----------------------------|
| `local`           | OHLCV / tick CSV files           | Paper / hybrid             |
| `binance`         | REST + WebSocket (trade/depth)   | Live + paper               |
| `binance-futures` | REST + WebSocket (trade/depth20) | Live + bracket orders      |
| `bitget-futures`  | Bitget UTA REST + WS (`ENABLE_BITGET`) | Live + paper + safety |
| `bitunix-futures` | Bitunix REST + WS (`ENABLE_BITUNIX`)   | MD/shadow Phase 0–1   |
| `synthetic`       | GBM paths (on demand)            | Monte Carlo / backtest     |

Additional modes include `--replay` from zstd-compressed binary logs. Realism models cover latency, market impact, queue position (based on L2 snapshots), and synthetic fill simulation.

## Strategies

Registered via the `REGISTER_STRATEGY` macro (`src/strategy/strategy_registry.h`):

`sma`, `ma-crossover`, `mean-reversion`, `breakout`, `coiled-spring`, `larry_connor`, `hedge-demo`, `adaptive-hybrid`, `structure-continuation`.

Supported indicators: SMA, EMA, RSI, Stochastic, Bollinger Bands, ATR, swing detection, rolling extremes.

Multiple strategies can run together: `--strategy sma,mean-reversion`.

## User Interfaces

- **Backtest**: Clean console dashboard (fast, non-interactive)
- **Shadow / Live**: Rich tabbed ncurses TUI with panels for positions, orders, L2, risk, brackets, analytics, etc.
- **Web** (`--web`): Read-only browser cockpit and backtest report viewer (same snapshot data as the TUI). Opt-in, off the hot path, no order submission routes.

## Safety Surface (Phase 1 Freeze)

The following core files and modules are under the live-safety freeze (see the enforcement script for the exact list):

- `src/core/tt_target.h`
- `src/engine/engine.cpp`
- `src/providers/binance/` (futures provider, dead_mans_switch, kill_switch, reconciler)
- `src/risk/{risk_manager, futures_risk_check}.h`
- `src/execution/live_safety.h`
- `src/threading/worker_watchdog.h`

All modifications require the `LIVE_SAFETY_CCB_APPROVED` token in the commit message, two-person CCB review, and a clean multi-hour `engine_shadow` run. Enforcement is provided by `scripts/check-live-safety-freeze.sh` (wired into pre-commit and CI).

See `docs/governance/01-prod.md`, `docs/governance/02-prerequisites.md`, and `AGENTS.md`.

## Development Phases

| Phase                              | Status          | Notes |
|------------------------------------|-----------------|-------|
| Phase 0 (Tiny-Size Mainnet Futures) | 0/15 qualifying | Full artifacts + two signatures required. See `docs/governance/01-prod.md` and `reports/phase0/`. |
| Phase 1 (Live-Safety Freeze)        | Enforced        | 10 frozen files + token + CCB + clean shadow run. |
| Risk / DMS (R-*, S-*)               | Partial         | Tiered margin support landed; further items tracked in `docs/governance/03-todo.md`. |

Monte Carlo capabilities are fully integrated into the mainline engine and do not relax any Phase 0/1 gates or safety requirements.

## Documentation

| Document                              | Purpose |
|---------------------------------------|---------|
| `docs/reference/01-instructions.md`   | Master reference: CLI, build, providers, MC, realism, threading |
| `docs/governance/01-prod.md`          | Production playbook, phases, Go-Live gate, Phase 0 ritual |
| `docs/governance/03-todo.md`          | High-level task list (detailed items live under `docs/todos/`) |
| `docs/reference/02-user-manual.md`    | Architecture and operator overview |
| `AGENTS.md`                           | AI/agent coding rules, freeze policy, hot-path invariants |
| `docs/reference/05-web-ui.md`         | Web UI usage, endpoints, architecture |
| `docs/upcoming_platform/`             | Multi-venue status (Binance, Bitget, Bitunix) |
| `reports/phase0/PROGRESS.md`          | Phase 0 qualifying session tracker |
| `docs/README.md`                      | Documentation navigation |

Root `AGENTS.md`, `docs/governance/`, `reports/phase0/`, and the reference manuals are authoritative. MC and Web UI work do not relax safety or phase gates.

## Testing

Extensive GoogleTest suite (~850+ cases) including:

- Golden regression tests for execution fidelity
- Hot-path allocation discipline checks (`scripts/check-hotpath-json.sh`)
- Live-safety-freeze enforcement
- Optional sanitizers (ASAN/UBSAN/TSAN) and benchmarks

Run with `ctest --test-dir build` or the `linux-tests` preset.

Before using live capital, consult `docs/governance/01-prod.md`. All changes to the frozen surface must pass the CCB process and `scripts/check-live-safety-freeze.sh`.

---

**TrueTest is a personal research platform.** Use it responsibly. All live trading carries risk.