# prerequisites.md — Pre-PR Checklist for Live-Safety Surface (Phase 1 Freeze)

**Status**: Authoritative mandatory checklist. Every PR (even "docs only") that touches or describes the frozen safety surface **must** satisfy this before merge.

See also:
- `../../AGENTS.md` (AI model selection rules, anti-patterns, freeze policy)
- `01-prod.md` (Phase 1 gates, philosophy, full Go-Live context)
- `03-todo.md` (task references; P1-* items)
- `scripts/check-live-safety-freeze.sh` (mechanical enforcement)

**docs/ is now the central authoritative documentation home.** Last updated: 2026-07 (docs overhaul).

---

## Frozen Files (mechanical mirror)

These files carry the live-safety surface. Any modification requires the `LIVE_SAFETY_CCB_APPROVED` token in the commit message + clean shadow validation. `scripts/check-live-safety-freeze.sh` is the executable source of truth.

**Current-worktree note (2026-09-01)**: The literal token was supplied for the
current edit request, but there is no commit/body-token evidence, human
two-person CCB approval, or clean continuous ≥4-hour mainnet `engine_shadow`
evidence. The current worktree is not merge-ready or live-ready; Phase 0 remains
0/15.

```
src/core/tt_target.h
src/engine/engine.cpp
src/engine/engine.h
src/engine/engine_config.h
src/engine/engine_pending.cpp
src/engine/live_safety_session.cpp
src/engine/live_safety_session.h
src/bin/main.inc
src/bin/provider_open_policy.h
src/execution/execution_bridge.h
src/execution/fill_parser.h
src/execution/async_support.h
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
src/providers/binance/binance_provider.h
src/providers/binance/binance_kill_switch.h
src/providers/binance/binance_reconciler.h
src/providers/binance/binance_rest_client.h
src/providers/binance/binance_rest_order_transport.h
src/providers/binance/binance_oco_bracket_adapter.h
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

(The script and prod.md / docs/governance/03-todo.md / docs/todos/02-P1-freeze.md lists must stay in sync.)

---

## Mandatory Pre-PR Checklist

Before opening or merging any PR that edits a frozen file (or significantly describes the surface in docs):

1. **Read the rules**:
   - Current `AGENTS.md` (model selection: Opus-level for frozen surface; Sonnet otherwise; anti-pattern list)
   - `prod.md` (Phase 1 + gates + invariants + philosophy)
   - This file + relevant `todo.md` P1 items (current gaps/status live in todo + prod; see `docs/archive/production-readiness-gaps-2026-05.md` only for May 2026 historical view)

2. **Run the mechanical check** (must pass or explicitly only non-frozen files changed):
   ```bash
   ./scripts/check-live-safety-freeze.sh
   # Optional: ./scripts/check-live-safety-freeze.sh --base <commit>
   ```

3. **Commit message requirement**:
   - PR description / commit must contain the exact token: `LIVE_SAFETY_CCB_APPROVED`

4. **No forbidden anti-patterns introduced** (see `AGENTS.md` + prod for full rationale):
   - No runtime live-order bypass / `target_allows_live_orders` weakening
   - `halt_flag_` remains terminal / write-once (no resettable/auto-clearing)
   - No "helpful" retry, backoff, or fallback on kill-switch / DMS / reconciler / watchdog paths
   - No `nlohmann::json` on hot path
   - No second producer on any SPSC ring
   - No `HAS_*` or venue-specific logic in core/engine/threading/risk
   - Reconciler stays default-refuse (except the one documented soft-warn for spot testnet monthly resets)
   - No reserved-mainnet normal or generic `risk_unwind` order path may bypass
     the exact pre-submit durability ACK and post-publication halt recheck
   - Durable-ledger compromise remains sticky; no destructor or incomplete
     shutdown may implicitly mark a reserved ledger finalized
   - Do not describe the current event log as a full command WAL, exactly-once
     execution, or crash recovery; cancel/amend/native-bracket and
     kill-switch/DMS/native safety command coverage plus state reconstruction
     remain follow-up work
   - For safety-description changes in docs: review the corresponding code comment blocks for consistency

5. **Governance hygiene**:
   - Note "prod.md impact" (or "no impact") in PR description
   - Reference the relevant todo item(s) by file and ID (e.g. "Addresses `docs/todos/02-P1-freeze.md` P1-02"; see docs/todos/00-OVERVIEW.md)
   - Phase 0 is still in active collection (or the PR explicitly advances collection / updates status)

6. **No open untagged changes** on the branch that affect reproducibility or safety surface.

7. **Post-edit / before merge**:
   - Update `docs/governance/03-todo.md` (thin) + the specific `docs/todos/XX-*.md` with the item(s) addressed + any surfaced follow-ups (see docs/todos/00-OVERVIEW.md)
   - For phase-exit PRs: also update corresponding `prod.md` section + Go-Live Gate / `reports/phase0/PROGRESS.md`
   - Change must have been exercised in at least one clean `engine_shadow` or backtest run that covers the modified path
   - Two-person CCB review for any frozen surface change
   - At least four continuous hours of clean `engine_shadow` evidence for kill/DMS/reconciler/halt lifecycle changes; unit tests do not replace this soak

8. **Model discipline**:
   - Use an approved frontier model (per `AGENTS.md` multi-agent protocol) for any edit touching the mechanically frozen safety surface or its core invariants.

Escalate to CCB if borderline or if the change is large.

---

## After Phase Exits (in prod.md)

On every phase exit declared in `prod.md`:
- Update `docs/governance/03-todo.md` (thin high-level) + relevant `docs/todos/XX-*.md` (detailed)
- Update this file if the checklist evolved
- Update "Last updated" notes across governance docs
- Run "docs verified + links resolve + todo.md updated" ritual before any capital tier increase (see docs/todos/00-OVERVIEW.md)

**Last updated**: 2026 (post-merge cleanup + todos/ split cross-refs)

Treat this file as living governance. Broken links or stale references to it are doc bugs.

*Last updated: 2026-07 (docs overhaul) — pointers to new governance paths.*
