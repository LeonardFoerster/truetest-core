# Target Architecture (High-Level)

**Status**: Thin extraction / planned skeleton (Doc Phase per D-03). 

**Planned / extracted; see reference/ for current** authoritative details and diagrams. Full material lives in `docs/reference/02-user-manual.md` (architecture + data flow), `docs/reference/01-instructions.md`, root governance (`prod.md`, `CLAUDE.md`), and source (`src/`).

**Last updated**: 2026-07-30 (Bitget provider + 14-file freeze surface).

---

## Core Value Proposition (from user-manual)

Production-grade, auditable foundation reusing **exact same hot path**, risk engine, analytics, and order lifecycle for:
- Reproducible backtests
- Divergence-aware shadow runs
- Real-money execution (compile-time gated)

Three binaries from one tree via `TT_TARGET` (BACKTEST / SHADOW / LIVE) in `src/core/tt_target.h`:
- Live orders **physically impossible** except `engine_live` (dead-code elimination).
- `target_allows_live_orders()` is constexpr.

## High-Level Components

- **Core engine** (`src/engine/`): event loop, batch/streaming, worker orchestration, StageTimer instrumentation.
- **Providers** (`src/providers/`): `IProvider` sole extension point. local (CSV/tick), binance + binance-futures (WS+REST), bitget-futures / bitget alias (UTA v3 USDT-M, `ENABLE_BITGET`), synthetic (GBM for MC).
- **Strategies** (`src/strategy/`): pluggable via `REGISTER_STRATEGY`; emit `order_event` + `exit_intent`. Self-registering (sma, mean-reversion, breakout, adaptive-hybrid, structure-continuation, ...).
- **Execution** (`src/execution/`): `IExecutionAdapter` family (LocalBook, QueueAware, Hybrid, Bridge), `Portfolio` (per-lot), `OrderTracker`, realism models.
- **Orderbook & matching** (`src/orderbook/`): price-time priority `Orderbook` + `FillModel`.
- **Risk & Exits** (`src/risk/`, `src/exits/`): `RiskManager`, venue `IRiskCheck` (FuturesRiskCheck first), `ExitManager` (per-lot SL/TP/trailing), `IBracketAdapter`.
- **Workers & threading** (`src/threading/`): lock-free SPSC RingBuffer (65536 slots, single producer), Worker base, WorkerWatchdog (3× heartbeat → halt), CPU affinity + presets (inline/light/standard/full/extended), spin policies.
- **Analytics & UI** (`src/analytics/`, `src/ui/`): Welford stats, adverse selection, report gen, rich ncurses TUI (tabbed panels for positions/lots/L2/risk/brackets/debug), optional web (`--web`).
- **Safety (futures)**: venue reconciler (user-data WS truth), DMS (orders-only cancel; Bitget UTA account-wide), kill-switch (cancel + flatten + deadline), pre-trade venue checks, terminal `halt_flag_`. Binance and Bitget each supply their own reconciler/DMS/kill implementations under `src/providers/<venue>/`.

## Data Flow (ASCII from user-manual; see full in reference/02-user-manual.md)

```
Data source → parser (market/tick/l2) → Engine dispatch → Strategy(ies)
  → RiskManager + IRiskCheck (pre-trade) → IExecutionAdapter
    → fill_event (synthetic or real) → Portfolio + ExitManager + Analytics
      → rings → Workers (Logging, Risk, Stats, Observer, MM)
```

1. IDataSource emits raw → events.
2. Engine → strategies.
3. Orders → risk → adapter.
4. Fills update state + post to rings.
5. Async workers consume.

**Key invariants** (repeated in prod, CLAUDE, instructions):
- Compile-time live gate absolute.
- Halt is terminal (write-once).
- Safety paths loud + non-retrying.
- Hot-path: no nlohmann/json, minimal allocs (pools), lock-free SPSC only.
- Reconciler refusal default.
- User-data WS = truth.
- DMS protects orders by default; optional `--dms-attempt-position-close` for in-process flatten on persistent HB failure.
- Provider is sole extension; no `HAS_*` in core.
- Small capital + evidence gates.

## Realism, MC, Persistence
- Realism models (latency/impact/queue/fill/fee) active in backtest/shadow only; bypassed in live.
- MC: synthetic provider + full reuse of engine/strategy/realism/analytics surfaces; deterministic seeding; `--monte-carlo --mc-trials N` (see instructions + todo MC-*).
- Persistence: binary zstd event log (mandatory durable), optional QuestDB (soft-fail), checkpoints.

## Safety Surface (Phase 1 Freeze)
**14 files** (see `scripts/check-live-safety-freeze.sh`, `prod.md`, `prerequisites.md`):
`tt_target.h`, `engine.cpp`, binance_futures_* (provider/dead_mans/kill/reconciler), bitget_futures_* (provider/dead_mans/kill/reconciler), risk/*, live_safety.h, worker_watchdog.h.

All edits: `LIVE_SAFETY_CCB_APPROVED` token + CCB + clean shadow run.

---

**See for current details**:
- Full diagram + features: `docs/reference/02-user-manual.md`
- Realism models: `docs/architecture/03-realism.md` (this set) + instructions §7
- Model/anti-patterns: `docs/architecture/02-model.md`
- Performance: `docs/architecture/04-performance.md`
- Instructions + checklists: `docs/reference/01-instructions.md`
- Governance: `docs/governance/01-prod.md`

This is a thin high-level extract for the aspirational `docs/architecture/` hierarchy. No duplication of long-form ritual/gates.