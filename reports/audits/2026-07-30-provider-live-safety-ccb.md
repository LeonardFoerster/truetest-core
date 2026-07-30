# Live-Safety CCB — `provider` → `master`

**Date:** 2026-07-30  
**Branch tip (at attestation):** see git history following this file  
**Base:** `origin/master`  
**PR:** https://github.com/LeonardFoerster/truetest-core/pull/8  
**Token:** `LIVE_SAFETY_CCB_APPROVED`  
**prod.md impact:** Phase-1 freeze surface extended (Bitget four files already on freeze list) + engine/risk tightenings; no capital-tier change.

---

## Scope (frozen files vs master)

| File | Delta |
|------|--------|
| `src/engine/engine.cpp` | L2 mark warm, per-symbol marks, funding→analytics, tracker open-orders into risk snap |
| `src/risk/risk_manager.h` (+ cpp) | `active_orders` / `active_orders_valid`; zero-price non-reducing reject |
| `src/risk/futures_risk_check.h` | Margin base = mark equity (`cash + qty*mark`) |
| `src/providers/bitget/bitget_futures_provider.h` | New (UTA live open refuse checklist + safety wiring) |
| `src/providers/bitget/bitget_futures_dead_mans_switch.h` | New (countdown clamp [5,60]s, no mid-cycle retry) |
| `src/providers/bitget/bitget_futures_kill_switch.h` | New (2 REST steps, no retry, deadline-bounded) |
| `src/providers/bitget/bitget_futures_reconciler.h` | New (default-refuse; no demo soft-pass) |
| `src/core/tt_target.h` | Unchanged |
| `src/execution/live_safety.h` | Unchanged |
| `src/threading/worker_watchdog.h` | Unchanged |
| Binance freeze headers | Unchanged |

---

## Multi-agent CCB panel

| Role | Verdict |
|------|---------|
| **Reviewer A** (live-safety invariants) | **No Safety Invariant Violations** |
| **Reviewer B** (Bitget + engine + risk re-audit) | **No Safety Invariant Violations** |
| **Reviewer C** (fail-open / pre-trade order / token hygiene) | Code tightenings sound; residual intentional skips + process token gap on early Bitget commits |

### Invariants checked (panel consensus)

1. **S2** `target_allows_live_orders()` constexpr; no runtime live bypass  
2. **S3** `halt_flag_` write-once terminal; no auto-resume from Bitget/DMS/kill  
3. **S4** Kill: no retry loop; DMS: fixed countdown bounds, no adaptive lengthening  
4. **S5** Reconciler default-refuse; Bitget demo flag not used to soft-pass  
5. **S7** Venue `IRiskCheck` before `RiskManager` on submit path  
6. **S9** No `HAS_BITGET` in engine/risk/threading/core  
7. Risk tightenings: tracker open-orders, zero-price reject, mark-equity leverage  

### Residual risks (accepted, not merge blockers)

| ID | Residual | Mitigation |
|----|----------|------------|
| R1 | Equity-% caps skip while `risk_view.equity == 0` (pre-first mark) | Absolute notional caps; first mark updates equity |
| R2 | `FuturesRiskCheck` skips when `mark <= 0` | L2 mark warm + limit-price fallback + RM zero-price reject |
| R3 | Leverage/liq skip when mark equity ≤ 0 | Notional cap still applies when configured |
| R4 | Bitget DMS countdown is **account-wide** | Loud ctor log; operator SOP |
| R5 | Bybit safety files outside freeze list | Documented; Gate live refuses open |
| R6 | Early Bitget freeze commits lack token in body | Covered by this consolidating attestation + later tokened commits |

---

## Mechanical gates (at attestation)

```
./scripts/check-hotpath-json.sh          # OK
./scripts/check-layer-deps.sh            # OK
./scripts/check-live-safety-freeze.sh    # OK (HEAD)
ctest linux-providers-questdb            # 1525/1525 (prior integration tip)
```

---

## Soak / human CCB (honest status)

| Requirement | Status |
|-------------|--------|
| Multi-agent panel ≥2 independent | **Done** (A+B+C) |
| Token on consolidating freeze attestation | **This commit** |
| Human two-person CCB signatures | **Pending operator** |
| ≥4h clean `engine_shadow` on freeze path | **Pending** (not claimed) |
| Bitget demo kill/DMS drill | **Pending** (unit coverage present) |

**SAFETY VERDICT: PASS with soak pending**

Human CCB must still sign before capital-tier or mainnet use. Code merge of `provider` is approved from the agent panel for freeze-surface *invariants*; soak remains operator ritual.

---

## Addresses

- `docs/governance/02-prerequisites.md` (freeze PR checklist)  
- `docs/todos/02-P1-freeze.md` (freeze surface hygiene)  
- PR #8 freeze / CCB section  

---

*Attestation authored for consolidating CCB on multi-commit freeze history without rewriting early Bitget commits.*
