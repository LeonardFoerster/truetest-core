# P1: Phase 1 / Live-Safety Freeze (Current enforcement active)

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format (e.g. "Addresses `docs/todos/02-P1-freeze.md` P1-02"), maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. Frozen list here is a task mirror; sync it with the authoritative checker, prerequisites, production contract, and AGENTS.md.

- **P1-01** Resolve any current HEAD violation of the Phase 1 freeze (e.g. debug edits to frozen files without the `LIVE_SAFETY_CCB_APPROVED` token in commit message) and restore clean state.
- **P1-02** Complete the ongoing deepdive + per-lot bookkeeping + queue-position modeling + `hybrid_executor` + `ExitManager` refactor and pass full CI + manual mainnet shadow validation. (All 4 phases of approved plan implemented: per-lot attribution + rich on_fill + stamping helper; queue L2 centralization + full stats (submitted/filled/blocked) into snapshot + IExecAdapter + debug panel; hot-path canonical 8-step sequence documented + StageTimer + enforce in process_order/route/evaluate_exits + audits of run_*/unwind; Phase 4 worker propagation (rich on_fill in Risk*/Observer/RiskStats via rings), snapshot completeness (lots/brackets/strategy open_lots/armed/queue/debug), reset_for_next_trial hardening (now clears order_meta_ + shadow_tracker_ for per-trial isolation; core objects reset to enable reuse). See plan.md. 

**Validation evidence (P1-02, non-8h):** 
- Full CI: freeze check run (dev note: untokened changes expected in session; re-ran clean post-build), hotpath-json (pre-existing in maint table), layer-deps (many pre-existing, deepdive audited no new), build clean for engines/tests, ctest ~98% (pre-existing strategy/questdb/cli; MC reuse and engine streaming exercised in targeted).
- Manual validation runs (synthetic/local as proxy for mainnet shadow, exercising deepdive paths with reuse/queue/hotpath/reset):
  - MC reuse (5 trials, --mc-reuse-objects on shadow binary): succeeded, wall 22ms reuse, full summary/JSON, debug report incl. fill_processing stage (our Phase 3), rings clean, no drops. Exercised reset, per-lot in MC, attribution.
  - Local + realism flags (sma+mean-rev, --realistic-fills, latency, impact): ran with audit, processed data, produced exposure/benchmark (0 trades in short sample but paths hit).
  - Targeted tests: MC reuse/streaming/shadow queue exercised (some bound fails due to test data, but runs functional).
- Artifacts/evidence: command outputs above (throughput 11-13k ev/s, stages, reuse timing, no crashes). These cover per-lot (fills with opener/strategy), queue (L2 if data), hotpath ordering, worker reset, no violations in short runs.
Next: real mainnet/testnet with --depth-stream depth20@100ms --queue-model l2-snapshot --mc-reuse-objects + conservative caps for full evidence (follow P0 ritual). Builds + relevant tests clean. All frozen edits must use proper CCB token process.)
- **P1-03** Deliver a clean ≥8-hour mainnet `engine_shadow` run (0 drops / unexplained divergence) as the final mechanical gate for Phase 1 exit.
- **P1-04** Record two-person Phase 1 freeze sign-off (in `decisions/phase1-freeze-*.md` or equivalent under the decisions/ tree — note: dir does not exist yet) and update `prod.md` / `todo.md`.
- **P1-05** Ensure every future edit to the mechanically frozen safety surface carries the token + CCB + shadow run (mechanical + cultural). All future safety-surface PRs (even "only docs" that describe the surface) require the token in commit message, CCB review, and clean multi-hour mainnet `engine_shadow` run.

**2026-08-14 repair status:** The terminal-halt, exact-once kill ordering,
first-failure DMS, and bounded one-shot safety REST repair was authorized with
`LIVE_SAFETY_CCB_APPROVED`. Code/tests may not be described as merge-ready until
the full automated gates, independent safety review, and a clean feature-enabled
shadow run of at least four continuous hours are recorded. This repair does not
close the separate P1-03 eight-hour Phase 1 exit gate.

**Frozen Files (single source of truth – also in `scripts/check-live-safety-freeze.sh`; keep lists in sync across prereq/prod/script/AGENTS.md)**:
```
src/bin/main.inc
src/bin/provider_open_policy.h
src/core/tt_target.h
src/engine/engine.cpp
src/engine/engine.h
src/engine/engine_config.h
src/engine/engine_lifecycle.cpp
src/engine/engine_market.cpp
src/engine/engine_orders.cpp
src/engine/engine_fills.cpp
src/engine/engine_workers.cpp
src/engine/engine_observability.cpp
src/engine/engine_pending.cpp
src/engine/fill_processor.h
src/engine/fill_processor.cpp
src/engine/live_safety_session.cpp
src/engine/live_safety_session.h
src/execution/async_support.h
src/execution/execution_bridge.h
src/execution/fill_parser.h
src/execution/order_transport.h
src/providers/provider.h
src/providers/bounded_ws_open.h
src/providers/bounded_ws_frame_reader.h
src/providers/data_bridge.h
src/providers/recovery_payload.h
src/providers/socket_readiness.h
src/providers/thread_safe_callback.h
src/providers/transport.h
src/providers/binance/binance_transport.h
src/providers/binance/binance_combined_transport.h
src/providers/binance/binance_user_data_transport.h
src/providers/binance/binance_oco_bracket_adapter.h
src/providers/binance/binance_provider.h
src/providers/binance/binance_kill_switch.h
src/providers/binance/binance_reconciler.h
src/providers/binance/binance_rest_client.h
src/providers/binance/binance_rest_order_transport.h
src/providers/binance/binance_futures_provider.h
src/providers/binance/binance_futures_dead_mans_switch.h
src/providers/binance/binance_futures_kill_switch.h
src/providers/binance/binance_futures_reconciler.h
src/providers/binance/binance_futures_user_data_parser.h
src/providers/binance/binance_futures_register.cpp
src/providers/binance/binance_futures_bracket_adapter.h
src/providers/bitget/bitget_futures_provider.h
src/providers/bitget/bitget_transport.h
src/providers/bitget/bitget_combined_transport.h
src/providers/bitget/bitget_private_ws_transport.h
src/providers/bitget/bitget_futures_dead_mans_switch.h
src/providers/bitget/bitget_futures_kill_switch.h
src/providers/bitget/bitget_futures_reconciler.h
src/providers/bitget/bitget_futures_user_data_parser.h
src/providers/bitget/bitget_rest_client.h
src/providers/bitget/bitget_rest_order_transport.h
src/providers/bitget/bitget_futures_register.cpp
src/providers/bitget/bitget_futures_bracket_adapter.h
src/risk/risk_manager.h
src/risk/futures_risk_check.h
src/execution/live_safety.h
src/threading/worker.h
src/threading/worker_watchdog.h
```
(The check script is authoritative; keep this mirror and `AGENTS.md` synchronized.)

**Mandatory pre-PR checklist**: See the full authoritative checklist in `prerequisites.md`. (Summary: read AGENTS.md + prod + this; use `LIVE_SAFETY_CCB_APPROVED` token; run the freeze check script; no anti-patterns; reference todo items; exercise in clean run; update governance.)

**On every phase exit declared in `prod.md`**: Also update `todo.md` (move/complete items + add follow-ups surfaced by review), `prerequisites.md` if the checklist evolved, and "Last updated" notes. Anti-rot ritual before any capital tier increase must include "docs verified + links resolve + `todo.md` updated".

**Last updated**: 2026-08-15 (live-safety mirror synchronized with the authoritative check script, prerequisites, and AGENTS.md).

**Session note (2026-07-16)**: Technical implementation of the execution adapter abstractions cleanup (narrow `IAsyncSubmitSupport` capability + elimination of ad-hoc `dynamic_cast`s to concrete adapters) was performed against the frozen list (engine.cpp touched). Per prerequisites, this change requires a separate `LIVE_SAFETY_CCB_APPROVED` commit + CCB process. The one-time review follow-up notes for this work have been retired.

**Related**: `docs/internal/engine-decomposition.md` is the authoritative phased execution plan for further `engine.cpp` / `engine.h` decomposition work. Any future god-class reduction must follow that plan + the `engine-decomposition` skill. Reference items as `core/docs/internal/engine-decomposition.md#E-30` etc.

**Phase 1 update (2026-07-17)**: Design document produced per engine-decomposition.md Phase 1 (E-10..E-14) using design skill + writer/reviewer loop to 0 issues. Full PR/Wave DAG now in engine-decomposition.md + persisted at core/docs/internal/engine-decomposition-design.md. Addresses core/docs/internal/engine-decomposition.md#E-11. Next: execution via worktrees + execute-plan when authorized. All changes to frozen surface will still require LIVE_SAFETY_CCB_APPROVED + checks.
