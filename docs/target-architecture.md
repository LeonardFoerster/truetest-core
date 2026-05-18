# TrueTest Target Architecture (Post-Deepdive)

**Status**: Initial version created during Phase 1 of `prod.md`. This is the intended steady state the team is driving toward. It will be updated as the deepdive progresses.

## Guiding Principles

1. **Compile-time safety is non-negotiable** — Only `engine_live` (and future `keeper_live` equivalents) can ever emit real orders or on-chain transactions. `target_allows_live_orders()` and `TT_TARGET` remain the single source of truth.
2. **Halt is terminal** — Once `halt_flag_` is raised, the only recovery path is manual operator intervention + explicit restart. No auto-resume.
3. **Provider abstraction is the extension point** — New venues (Binance futures/spot today, Drift keeper tomorrow, others later) plug in via `IProvider` + transport/parser/execution + the four safety hooks (`IReconciler`, `IKillSwitch`, `IRiskCheck`, `IBracketAdapter`).
4. **Hot path discipline** — Zero allocations, no `nlohmann/json`, lock-free SPSC rings, object pools, CPU affinity. Enforced by `scripts/check-hotpath-json.sh` and review.
5. **Observability by default** — Binary event log + optional QuestDB + rich TUI + ndjson + (future) Prometheus are always available.
6. **Small capital first, then scale** — Every phase gate in `prod.md` must be passed before increasing position size or removing training wheels.

## Core Components (Steady State)

### Event & Execution Model
- `engine` class owns the hot loop and the five worker presets (inline → extended).
- `provider::event` variant becomes the primary streaming type (bar / tick / l2 / status / liquidation_opportunity / funding / etc.).
- `ExecutionBridge` + user-data / on-chain source of truth for fills and position snapshots (never REST polling for truth).

### Risk & Safety Layers
- Layered checks (in order):
  1. `FuturesRiskCheck` / venue-specific `IRiskCheck` (notional, leverage, liquidation distance, **real tiered MMR** from `/fapi/v1/leverageBracket`)
  2. `RiskManager` (balance-based + daily loss, max drawdown, **position size as % of equity**, **volatility-aware limits**, **spread & funding-rate circuit breakers**)
  3. `IReconciler` at startup
  4. `IKillSwitch` + `DeadMansSwitch` on shutdown / heartbeat loss
  5. `WorkerWatchdog` for long-lived provider threads
- Funding is a first-class event (`funding_event`) that updates `portfolio` cash/equity, flows through the event ring, is captured in QuestDB, and is visible to risk/analytics/TUI.
- Realized volatility (EWMA/Welford on returns) and current spread (from L2) are maintained in the analytics layer and fed to risk decisions via `risk_snapshot`.
- Circuit breakers can raise `halt_flag_` on extreme spread or funding rate.

### Persistence & Recovery
- Binary event log (zstd + integrity trailer) is mandatory in live/keeper modes.
- QuestDB is opt-in; `--persist-strict` will make it hard-fail.
- Checkpoint format includes the full `lots_` map (not just netted positions) so crash recovery can replay fills correctly.
- `run_keeper()` (or equivalent) mode for non-CEX strategies such as the Drift liquidation keeper.

### Providers
- Sibling providers under `src/providers/`:
  - `local/`
  - `binance/` (spot + futures, with full safety surface)
  - `drift/` (future) — will likely use a hybrid C++/Rust FFI boundary for account decoding and transaction building.
- Each provider owns its `IDataTransport`, parser, execution adapter, and the four safety interfaces.

### Observability & Operations
- Rich ncurses TUI (already present for shadow/live).
- Structured ndjson output for headless operation.
- Future: Prometheus exposition + external alerting sinks.
- Encrypted credential store (future) so raw keys are never on the command line in production.

## What Is Explicitly Out of Scope (or Deferred)

- Hedge mode for futures (refused today; may be added later with second position bucket).
- COIN-M (inverse) futures — separate provider family.
- Generic multi-venue cross-margin risk engine (Phase 3+).
- Web UI (the old Boost.Beast + React stack was deliberately removed).

## Relationship to prod.md Phases

This document is the "north star" that the phased work in `prod.md` is converging toward.

- Phase 1 freezes the current safety surface so we can reason about it.
- Phase 2 completes the risk model (funding + tiered liquidation).
- Later phases harden recovery, observability, and operational maturity.

When the architecture described here is substantially realized and all Phase 6 gates are passed, this document should be updated from "target" to "current" and a new major version of the architecture may be started if needed.

---

**Last significant update**: 2026-05 (Phase 1 baseline)
**Review cadence**: Update at the end of every major phase in `prod.md`.
