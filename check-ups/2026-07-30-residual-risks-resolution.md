# Residual risks resolution — post `feature/backtest-accuracy` merge

**Date:** 2026-07-30  
**Branch:** `fix/residual-risks-backtest-accuracy`  
**Parent merge:** `dcb6ef3` (`feature/backtest-accuracy` → `master`)

## Risks from merge PR #6 and disposition

| # | Residual risk | Severity | Resolution |
|---|---------------|----------|------------|
| 1 | HybridExecutor: `deliver_mm` / `sweep` no-op via `dynamic_cast<LocalBookAdapter*>` | Medium (paper accuracy) | **Fixed:** `on_book_trades` + `sweep_resting_range` promoted to `IExecutionAdapter`; Hybrid forwards; engine uses virtual dispatch. Tests: `HybridExecutor.EngineFacing*`. |
| 2 | ≥4h mainnet/testnet `engine_shadow` soak | Process | **Not runnable unattended** without operator venue credentials (agent must not invent secrets). Synthetic shadow smoke remains green. Operator action: run attended mainnet/testnet shadow SOP from `docs/governance/01-prod.md` when ready; until then soak stays **pending / waived for merge**. |
| 3 | Intermediate freeze commits without `LIVE_SAFETY_CCB_APPROVED` in history | Process hygiene | **Accepted as historical.** Tip + merge commit carry the token; `check-live-safety-freeze.sh` gates HEAD. **No rewrite of published `master` history** (force-push forbidden). Future freeze commits must token every commit. |
| 4 | Unattended live readiness | Out of scope | **Unchanged by design.** Still **not ready for unattended live** — Phase 0–6 in `docs/governance/01-prod.md`. |
| 5 | Stale `engine.h` flatten / halt comment (S3) | Low (docs) | **Fixed:** `request_flatten` documents one-shot unwind without halt auto-clear; added `is_halted()`. |
| 6 | `LocalBookAdapter` `std::make_shared<order>` bypasses order pool | Medium residual perf | **Fixed:** submit path uses `ob_->create_order(...)` (pooled). |

## Explicit non-claims

- This note does **not** claim 4h mainnet shadow soak completed.
- This note does **not** claim unattended live readiness.
- CCB for freeze touch on this follow-up: operator-authorized (`LIVE_SAFETY_CCB_APPROVED`).
