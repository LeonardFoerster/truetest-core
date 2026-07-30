# prerequisites.md — Pre-PR Checklist for Live-Safety Surface (Phase 1 Freeze)

**Status**: Authoritative mandatory checklist. Every PR (even "docs only") that touches or describes the frozen safety surface **must** satisfy this before merge.

See also:
- `../CLAUDE.md` (AI model selection rules, anti-patterns, freeze policy)
- `01-prod.md` (Phase 1 gates, philosophy, full Go-Live context)
- `03-todo.md` (task references; P1-* items)
- `scripts/check-live-safety-freeze.sh` (mechanical enforcement)

**docs/ is now the central authoritative documentation home.** Last updated: 2026-07 (docs overhaul).

---

## Frozen Files (single source of truth)

These files carry the live-safety surface. Any modification requires the `LIVE_SAFETY_CCB_APPROVED` token in the commit message + clean shadow validation.

```
src/core/tt_target.h
src/engine/engine.cpp
src/providers/binance/binance_futures_provider.h
src/providers/binance/binance_futures_dead_mans_switch.h
src/providers/binance/binance_futures_kill_switch.h
src/providers/binance/binance_futures_reconciler.h
src/providers/bitget/bitget_futures_provider.h
src/providers/bitget/bitget_futures_dead_mans_switch.h
src/providers/bitget/bitget_futures_kill_switch.h
src/providers/bitget/bitget_futures_reconciler.h
src/risk/risk_manager.h
src/risk/futures_risk_check.h
src/execution/live_safety.h
src/threading/worker_watchdog.h
```

(The script and prod.md / docs/governance/03-todo.md / docs/todos/02-P1-freeze.md lists must stay in sync.)

---

## Mandatory Pre-PR Checklist

Before opening or merging any PR that edits a frozen file (or significantly describes the surface in docs):

1. **Read the rules**:
   - Current `CLAUDE.md` (model selection: Opus-level for frozen surface; Sonnet otherwise; anti-pattern list)
   - `prod.md` (Phase 1 + gates + invariants + philosophy)
   - This file + relevant `todo.md` P1 items (current gaps/status live in todo + prod; see `docs/archive/production-readiness-gaps-2026-05.md` only for May 2026 historical view)

2. **Run the mechanical check** (must pass or explicitly only non-frozen files changed):
   ```bash
   ./scripts/check-live-safety-freeze.sh --check-head
   ```

3. **Commit message requirement**:
   - PR description / commit must contain the exact token: `LIVE_SAFETY_CCB_APPROVED`

4. **No forbidden anti-patterns introduced** (see CLAUDE + prod for full rationale):
   - No runtime live-order bypass / `target_allows_live_orders` weakening
   - `halt_flag_` remains terminal / write-once (no resettable/auto-clearing)
   - No "helpful" retry, backoff, or fallback on kill-switch / DMS / reconciler / watchdog paths
   - No `nlohmann::json` on hot path
   - No second producer on any SPSC ring
   - No `HAS_*` or venue-specific logic in core/engine/threading/risk
   - Reconciler stays default-refuse (except the one documented soft-warn for spot testnet monthly resets)
   - For safety-description changes in docs: review the corresponding code comment blocks for consistency

5. **Governance hygiene**:
   - Note "prod.md impact" (or "no impact") in PR description
   - Reference the relevant `todo.md` item(s) (e.g. "Addresses todo.md #P1-02" or "Addresses docs/todos/02-P1-freeze.md#P1-02" for precision; see docs/todos/00-OVERVIEW.md)
   - Phase 0 is still in active collection (or the PR explicitly advances collection / updates status)

6. **No open untagged changes** on the branch that affect reproducibility or safety surface.

7. **Post-edit / before merge**:
   - Update `docs/governance/03-todo.md` (thin) + the specific `docs/todos/XX-*.md` with the item(s) addressed + any surfaced follow-ups (see docs/todos/00-OVERVIEW.md)
   - For phase-exit PRs: also update corresponding `prod.md` section + Go-Live Gate / `reports/phase0/PROGRESS.md`
   - Change must have been exercised in at least one clean `engine_shadow` or backtest run that covers the modified path
   - Two-person CCB review for any frozen surface change

8. **Model discipline**:
   - Use Opus-class model (per CLAUDE) for any edit touching the 10 frozen files or their core invariants.

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
