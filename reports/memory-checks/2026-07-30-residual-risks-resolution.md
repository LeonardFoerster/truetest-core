# Residual risks resolution — post `feature/backtest-accuracy` merge

**Date:** 2026-07-30  
**Branch (original fix):** `fix/residual-risks-backtest-accuracy` → merged as `060abdc`  
**Parent merge:** `dcb6ef3` (`feature/backtest-accuracy` → `master`)  
**Execution tip:** `master` @ `060abdc` (re-verified this run)

## Risks from merge PR #6 and disposition

| # | Residual risk | Severity | Resolution |
|---|---------------|----------|------------|
| 1 | HybridExecutor: `deliver_mm` / `sweep` no-op via `dynamic_cast<LocalBookAdapter*>` | Medium (paper accuracy) | **Fixed + re-verified:** `on_book_trades` + `sweep_resting_range` on `IExecutionAdapter`; Hybrid forwards; engine virtual dispatch. Tests: `HybridExecutor.EngineFacing*` (7/7 PASS). `rg dynamic_cast<LocalBookAdapter` → **none** in `src/`. |
| 2 | ≥4h mainnet/testnet `engine_shadow` soak | Process | **Agent execution complete for synthetic path only.** Synthetic `engine_shadow --provider synthetic --seed 424242` exit **0** (artifact `/tmp/shadow-residual-exec.json`). **No venue credentials** in environment (`BINANCE_*` / `API_*` unset). Mainnet/testnet ≥4h soak remains **operator-only** per `docs/governance/01-prod.md` (8h mainnet shadow evidence language). **Waived for agent merge hygiene; pending for operator Phase evidence.** |
| 3 | Intermediate freeze commits without `LIVE_SAFETY_CCB_APPROVED` in history | Process hygiene | **Accepted as historical (no rewrite).** Tip commits tokened: `060abdc`, `26876e6`, `067c08d`. Gate `./scripts/check-live-safety-freeze.sh` **OK**. Force-push of `master` **not** performed. Future freeze commits must token every commit. |
| 4 | Unattended live readiness | Out of scope | **Unchanged by design.** Explicitly **not ready for unattended live** — Phase 0–6 in `docs/governance/01-prod.md`. No `engine_live` capital path exercised. |
| 5 | Stale `engine.h` flatten / halt comment (S3) | Low (docs) | **Fixed + re-verified:** `request_flatten` documents one-shot unwind without halt auto-clear; `is_halted()` present (`engine.h`). |
| 6 | `LocalBookAdapter` `std::make_shared<order>` bypasses order pool | Medium residual perf | **Fixed + re-verified:** submit path uses `ob_->create_order(...)` (`execution_adapter.h`). Note: Hybrid synthetic **quote reseed** still uses `make_shared<order>` (`hybrid_executor.h` ~171) — out of this item’s original LocalBook **submit** scope; tracked as follow-up pool hygiene (memory-check M2). |

## Explicit non-claims

- This note does **not** claim 4h mainnet shadow soak completed.
- This note does **not** claim unattended live readiness.
- CCB for freeze touch on residual fix: operator-authorized (`LIVE_SAFETY_CCB_APPROVED` on `26876e6` / merge `060abdc`).

---

## Execution log (agent re-run)

**When:** 2026-07-30 (execute residual-risks resolution)  
**Cwd:** `/home/leonard/work/projects/truetest/core`  
**HEAD:** `060abdc` = `origin/master`

| Step | Action | Result |
|------|--------|--------|
| E1 | Confirm code for items 1, 5, 6 on tip | Present (virtual dispatch, flatten docs, `create_order`) |
| E2 | Rebuild `truetest_tests` + `engine_shadow` | OK |
| E3 | `HybridExecutor.*:StopFillPricing.*:RealisticFills.*:WalkedBookImpact.*:HotpathAllocs.*` | **26/26 PASS** |
| E4 | Gate scripts (hotpath-json / layer-deps / freeze) | **All OK** |
| E5 | Synthetic `engine_shadow` smoke | **exit 0** |
| E6 | Venue credential probe (names only) | **All unset** → mainnet soak not started |
| E7 | History rewrite for untokened intermediate commits | **Skipped** (forbidden / accepted) |
| E8 | Unattended live / `engine_live` capital | **Not run** (out of scope) |

### Agent-doable completion

```text
ITEMS_FIXED_AND_VERIFIED = {1, 5, 6}
ITEMS_PROCESS_ACCEPTED   = {3, 4}
ITEM_SOAK                = synthetic OK; mainnet/testnet ≥4h PENDING_OPERATOR (no credentials)
RESIDUAL_EXECUTION       = COMPLETE_FOR_AGENT
```

### Operator remaining (optional)

1. Export venue keys (testnet preferred) via local env / secret store — never commit.
2. Attended `engine_shadow` mainnet or testnet soak ≥4h (prod.md Phase language prefers 8h mainnet for freeze evidence culture).
3. Attach soak evidence path / run-tag to governance notes when done.
4. Optional follow-up PR: Hybrid quote ladder → `book_->create_order` (memory-check M2).

---

*Original fix PR: https://github.com/LeonardFoerster/truetest-core/pull/7*  
*Accuracy merge PR: https://github.com/LeonardFoerster/truetest-core/pull/6*
