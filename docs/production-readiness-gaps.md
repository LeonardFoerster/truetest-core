# TrueTest — Live Trading Production Readiness Gaps

**Date**: May 2026 (snapshot; some items partially addressed since — see current status in root `todo.md` R-*/S-*/H-* + `prod.md` phases/Go-Live)  
**Scope**: Binance USDT-M futures live trading (`--provider binance-futures --mode live`) on real mainnet with real capital.  
**Purpose**: Honest gap analysis between current state and a production-grade, set-and-forget live trading system suitable for meaningful size. **Current open items map directly to root `todo.md` (R-01..R-05, S-01..S-06, H-01..H-06, P0/P1 gates, arch risk). Read together with `prod.md` (per governance).**

This document assumes the operator has already completed every prerequisite in `docs/futures-testnet.md` and the user manual (correct binary, one-way mode, conservative caps, dead-man's switch armed, tiny size, etc.). Planned `docs/operations/futures-testnet.md` — current details in prod.md / instructions.

**Intended Use & Scope**: TrueTest is a private, personal research and retail tool for the author only. It is not, and will never be, an enterprise-ready, institutional, or production trading system. Monte Carlo simulation, high-fidelity backtesting, and shadow divergence analysis are the primary mature capabilities. The live execution paths (`engine_live`) exist with unusually strong compile-time (`TT_TARGET`) and runtime safety layers (reconciler, DMS, kill-switch, venue risk checks, terminal halt, user-data source of truth, etc.). Any use of live paths is experimental, tiny-size, fully attended by the operator, and done at the author's own risk. The Phase 0/1 rituals and Go-Live language in this repository describe the author's personal evidence-gathering hygiene and self-imposed discipline — they are **not** a formal production release process or claim of readiness for others.

---

## Executive Summary

The engine has **unusually strong safety scaffolding** for a system of its age and origin — particularly around the dead-man's switch, kill switch, reconciler, pre-trade venue caps, and compile-time live-order gating.

However, it is **not yet ready for the author's private attended live use at scale**. The project is in the middle of a major architectural refactor ("deepdive"), several important risk features remain partial or planned, and real-world mainnet live trading history is still limited.

**Current maturity**: Late alpha / advanced prototype with excellent safety bones for the author's personal research.  
**Recommended use**: Tiny-size mainnet validation runs only, fully attended by the author. Not suitable for significant capital or unattended operation. When the author chooses to collect evidence toward personal live use, the Phase 0/1 gates apply.

---

## Gap Categories

### 1. Architectural & Refactoring Risk (Highest Meta-Risk)

| Gap | Current State | Risk Level | Affected Components | What Needs to Be Done |
|-----|---------------|------------|---------------------|-----------------------|
| Ongoing "deepdive" + per-lot refactor | Active multi-step changes (queue awareness, per-lot bookkeeping, latency modeling) still landing on `testnet` branch | **High** | `src/engine/`, `src/execution/`, `src/risk/`, threading, order tracker | Complete the full refactor phase, pass CI + manual shadow validation on mainnet before considering larger size |
| Cross-file safety invariants under change | Many live-critical paths (halt_flag_, reconciler, DMS heartbeat, kill switch) are explicitly flagged as high-risk for model edits | High | `tt_target.h`, `engine.cpp`, `risk_worker.h`, all `*kill_switch*`, `*dead_mans_switch*`, `*reconciler*` files | Freeze these areas until refactor stabilizes; require Opus-level review on any changes |
| Master branch includes MC work (integrated) | Freeze process and 10-file LIVE-SAFETY SURFACE rules remain for any safety-surface changes, regardless of branch | Medium | Entire project | The merge process itself requires clean docs + verification; safety freeze enforcement (script + CCB) is unchanged |

### 2. Risk Management Completeness

| Gap | Current State | Risk Level | Affected Components | What Needs to Be Done |
|-----|---------------|------------|---------------------|-----------------------|
| Position-based risk limits | Only balance-based (`RiskManager`) + venue notional/leverage/liquidation-distance caps | **High** | `src/risk/risk_manager.cpp`, `futures_risk_check.h` | Implement proper position sizing risk engine (max position as % of equity, volatility-adjusted limits, etc.) |
| Full mark-price liquidation simulation | Approximation exists (`cash / notional − maintenance_margin`) but marked as partial in docs | High | `binance_futures_safety.h`, `FuturesRiskCheck` | Build accurate per-position liquidation price calculator using Binance tiered maintenance margin rates + funding |
| Funding rate integration | Shadow mode and P&L do not yet simulate funding; risk engine does not account for it | Medium-High | `shadow_tracker.h`, `portfolio.cpp`, analytics | Wire funding events from user-data stream into portfolio and risk checks |
| Cross-margin / multi-symbol risk | Engine is largely single-symbol today; cross-margin awareness is out of scope | Medium | `Portfolio`, `RiskManager`, futures provider | Add account-level margin ratio monitoring and cross-symbol exposure limits |
| No extreme event circuit breakers | No automatic pause on massive spread widening, funding spikes, or exchange anomalies | Medium | Risk layer + observer | Add configurable volatility / spread / funding circuit breakers |

### 3. Dead-Man’s Switch & Kill Switch Limitations

| Gap | Current State | Risk Level | Affected Components | What Needs to Be Done |
|-----|---------------|------------|---------------------|-----------------------|
| DMS only cancels orders (does not flatten positions) | Explicitly documented foot-gun: open positions remain after DMS fires | **High** | `binance_futures_dead_mans_switch.h` | Extend DMS to also submit reduceOnly market closes (or provide very clear operator SOPs + automation) |
| Process suspension (`SIGSTOP`) defeats DMS | Heartbeat thread pauses → countdown fires while engine is frozen | High | Worker watchdog + DMS | Document more aggressively; consider adding a separate external watchdog process |
| Kill switch has hard deadline | If `reduceOnly` close misses deadline, operator must intervene manually | Medium | `binance_futures_kill_switch.h` | Improve retry + escalation logic; add external monitoring hooks |

### 4. Bracket / Exit Management on Futures

| Gap | Current State | Risk Level | Affected Components | What Needs to Be Done |
|-----|---------------|------------|---------------------|-----------------------|
| Partial-fraction brackets declined | `BinanceFuturesBracketAdapter` rejects `qty_fraction != 1.0`; falls back to engine-side ExitManager | Medium | `binance_futures_bracket_adapter.h`, `ExitManager` | Either implement proper multi-leg conditional orders or make the fallback behavior more robust and tested |
| Bracket placement is non-atomic | Two separate `STOP_MARKET` + `TAKE_PROFIT_MARKET` POSTs with `closePosition=true` | Medium | `binance_futures_bracket_adapter.h` | Add placement retry + reconciliation logic; improve logging of partial bracket states |
| Golden regression coverage for complex brackets | Not yet comprehensive for futures multi-leg scenarios | Medium | `tests/test_engine_brackets.cpp`, golden files | Expand test matrix to cover futures bracket + reconciliation edge cases |

### 5. Persistence & Recovery Reliability

| Gap | Current State | Risk Level | Affected Components | What Needs to Be Done |
|-----|---------------|------------|---------------------|-----------------------|
| QuestDB is soft-fail only | Unreachable → warning + continues (data loss for that run) | Medium-High | `data/questdb/`, engine startup | Add `--persist-strict` / hard-fail mode + automatic retry + local checkpoint fallback |
| Checkpoint / recovery robustness | Checkpoint format exists but recovery story is underdeveloped | Medium | `src/engine/checkpoint.h` | Implement reliable crash recovery with position + open order replay from binary logs |
| Binary event log as primary audit source | Excellent when enabled, but not mandatory | Medium | `event_log.h` | Make structured binary logging + integrity verification mandatory in live mode |

### 6. Production Hardening & Observability

| Gap | Current State | Risk Level | Affected Components | What Needs to Be Done |
|-----|---------------|------------|---------------------|-----------------------|
| No Prometheus / metrics export | Only TUI + QuestDB + ndjson | Medium | UI panels, workers | Add proper metrics endpoint or structured logging for external monitoring |
| Limited external alerting hooks | Operator must watch TUI or parse logs manually | Medium | Health panel, risk worker | Expose structured events for PagerDuty / Slack / Telegram on halt, large loss, DMS trigger, etc. |
| Secrets management | API keys via environment or CLI only | Medium | `main.inc`, credential scripts | Add encrypted credential store + key rotation support |
| No automatic tape rotation + offsite upload | Manual process | Low-Medium | Recording path | Build helpers for automatic tape management and cloud backup |

### 7. Testing & Provenance

| Gap | Current State | Risk Level | Affected Components | What Needs to Be Done |
|-----|---------------|------------|---------------------|-----------------------|
| Limited long-duration mainnet shadow data | Most validation on testnet or short shadow runs | High | — | Publish (or at least internally accumulate) multi-day, multi-symbol mainnet shadow divergence statistics |
| Small-size mainnet live track record | Very few documented real-money runs | High | — | Require minimum 30–60 days of tiny-size mainnet live operation with zero incidents before increasing size |
| No public or internal "incident post-mortems" | None visible | Medium | — | Start collecting and reviewing every live incident (even tiny-size) |

### 8. Documentation & Operator Procedures

| Gap | Current State | Risk Level | Affected Components | What Needs to Be Done |
|-----|---------------|------------|---------------------|-----------------------|
| User manual now exists but is still new | `docs/user-manual.md` was just generated | Medium | Documentation | Expand with decision trees, "what to do when X happens" runbooks, and full futures mainnet checklist |
| DMS + Kill Switch operator playbook | Excellent for testnet (`futures-testnet.md`); lighter for mainnet | Medium | — | Create dedicated mainnet "Live Trading Runbook" covering every failure mode |
| No formal change control process | Refactor work is moving fast | Medium | — | Define and enforce a change-control gate for any modification touching live order paths |

---

## Prioritized Roadmap to Production Readiness

**Phase 0 (Current)** — Tiny-size mainnet validation only (with all safety nets armed)

**Phase 1 (Required before meaningful size)**
- Complete current deepdive refactor phase + full CI + manual shadow validation
- Implement position-based risk limits
- Extend DMS to also attempt position flattening (or very strong SOPs)
- Add hard-fail option for QuestDB + mandatory binary logging in live mode
- Expand futures bracket golden tests

**Phase 2 (Strongly Recommended)**
- Full mark-price liquidation engine with tiered maintenance margins
- Funding rate integration into P&L and risk
- Prometheus metrics + external alerting hooks
- Encrypted credential store
- Published multi-week mainnet shadow divergence report

**Phase 3 (Nice to Have for Professional Use)**
- Multi-symbol / cross-margin risk engine
- Automatic tape rotation + offsite backup
- Circuit breakers for extreme market events
- Formal incident post-mortem process and change control board

---

## Verdict

| Question | Answer |
|----------|--------|
| Does the engine have better safety architecture than most retail tools? | Yes — significantly better in several areas (DMS, compile-time gating, reconciler, user-data source of truth). |
| Is it currently safe to run meaningful real capital live? | **No** — and this tool will never be positioned for that. It exists only for the author's private, tiny-size, fully attended personal experiments. |
| Is it safe for tiny-size "prove the system" mainnet runs with an experienced operator (the author) watching closely? | Marginally acceptable for the author's personal use, with eyes wide open and all safety nets armed. |
| What is the single biggest blocker right now? | The combination of an **ongoing deep architectural refactor** + **incomplete position-based risk management** + **DMS not protecting positions**. |

---

**Recommendation**

Do **not** treat this engine as ready for the author's private attended use at any meaningful scale yet.

When the author chooses to collect evidence toward personal live use, use the next 3–6 months for:
- Completing the refactor
- Hardening the risk surface
- Accumulating real mainnet shadow + tiny live data
- Building the missing operational tooling (alerting, credential management, runbooks)

Only after those items are demonstrably closed (with full artifacts and two-person sign-off) should the author consider increasing personal position size beyond "proof of concept" tiny-size attended levels.

---

*This document should be updated after every major refactor phase and before any increase in the author's personal live capital allocation (for the author's private attended use only).*