# Target Architecture (High-Level)

**Status**: Thin extraction / planned skeleton (Doc Phase per D-03). 

**Planned / extracted; see reference/ for current** authoritative details and diagrams. Full material lives in `docs/reference/02-user-manual.md` (architecture + data flow), `docs/reference/01-instructions.md`, `docs/governance/01-prod.md`, `AGENTS.md`, and source (`src/`).

**Last updated**: 2026-07 (new content impl — extracted high-level only; no invention).

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
- **Providers** (`src/providers/`): `IProvider` sole extension point. Always available: local (CSV/tick) and synthetic (GBM for MC); conditional venues: Binance spot/futures, Bitget UTA futures, and Bitunix futures MD/paper/shadow (live routing refused).
- **Strategies** (`src/strategy/`): pluggable via `REGISTER_STRATEGY`; emit `order_event` + `exit_intent`. Self-registering (sma, mean-reversion, breakout, structure-continuation, ...). The retired Adaptive Hybrid prototype is not shipped.
- **Execution** (`src/execution/`): `IExecutionAdapter` family (LocalBook, QueueAware, Hybrid, Bridge), `Portfolio` (per-lot), `OrderTracker`, realism models.
- **Orderbook & matching** (`src/orderbook/`): price-time priority `Orderbook` + `FillModel`.
- **Risk & Exits** (`src/risk/`, `src/exits/`): `RiskManager`, venue `IRiskCheck` (FuturesRiskCheck first), `ExitManager` (per-lot SL/TP/trailing), `DefaultExitPolicy` (platform SL/TP for every strategy via `--exit-policy` / `--sl` / `--tp`), `IBracketAdapter`.
- **Workers & threading** (`src/threading/`): lock-free SPSC RingBuffer (65536 slots, single producer), Worker base, WorkerWatchdog (3× heartbeat → halt), CPU affinity + presets (inline/light/standard/full/extended), spin policies.
- **Analytics & UI** (`src/analytics/`, `src/ui/`): Welford stats, adverse selection, report gen, rich ncurses TUI (tabbed panels for positions/lots/L2/risk/brackets/debug), optional web (`--web`).
- **Safety (futures)**: `BinanceFuturesReconciler` (user-data WS truth), `DeadMansSwitch` (orders-only cancel), `KillSwitch` (flatten + deadline), pre-trade venue checks, terminal `halt_flag_`.

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

**Key invariants** (repeated in prod, `AGENTS.md`, instructions):
- Compile-time live gate absolute.
- Halt is terminal (write-once).
- Safety paths loud + non-retrying.
- Hot-path: no nlohmann/json, minimal allocs (pools), lock-free SPSC only.
- Reconciler refusal default.
- User-data WS = truth.
- DMS owns the venue countdown only. Its first failure signals the centralized
  exact-once kill session, which owns cancellation and flattening.
- Provider is sole extension; no `HAS_*` in core.
- Small capital + evidence gates.

## Realism, MC, Persistence
- Realism models (latency/impact/queue/fill/fee) active in backtest/shadow only; bypassed in live.
- MC: synthetic provider + full reuse of engine/strategy/realism/analytics surfaces; deterministic seeding; `--monte-carlo --mc-trials N` (see instructions + todo MC-*).
- Persistence: a cleanly sealed, current-v3, non-segmented binary zstd event
  log is the authoritative replay ledger; in-run/unsealed prefixes and v1/v2
  checkpoints are diagnostic only. Reserved-mainnet normal and generic
  `risk_unwind` order intents cross an exact-ID, two-second pre-submit durability
  barrier; the engine refuses to invoke finalization for ledgers already known
  incomplete or compromised. This is not a full command WAL, exactly-once
  execution, or crash recovery; native safety commands remain outside this
  order-intent ACK. QuestDB is optional (soft-fail by default; terminal/nonzero
  with `--persist-strict`). See the canonical durability contract in
  `docs/governance/01-prod.md`.

## Safety Surface (Phase 1 Freeze)
The mechanically frozen surface covers the compile-time live gate, engine/session lifecycle, execution admission, Binance and Bitget provider/REST safety paths, risk, and worker halt propagation. See `scripts/check-live-safety-freeze.sh` for the exact list.

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
