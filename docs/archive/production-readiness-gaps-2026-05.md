# Production readiness gaps - live futures trading

What still separates the current state (May 2026) from running
`--provider binance-futures --mode live` on mainnet with real capital,
unattended and at meaningful size. Assumes the operator has already
worked through `docs/futures-testnet.md` and the user manual (correct
binary, one-way mode, conservative caps, dead-man's switch armed, tiny
size).

Short version: the safety layer (dead-man's switch, kill switch,
reconciler, pre-trade venue caps, compile-time live-order gating) is in
good shape. Everything around it is not there yet - the deepdive
refactor is still in flight, several risk features are partial, and
there is very little real mainnet live history. Treat it as a late
alpha: fine for tiny-size validation runs under supervision, not for
real capital or unattended operation.

---

## Gaps

### 1. Refactor in flight (biggest meta-risk)

| Gap | Current State | Risk Level | Affected Components | To do |
|-----|---------------|------------|---------------------|-----------------------|
| Ongoing "deepdive" + per-lot refactor | Active multi-step changes (queue awareness, per-lot bookkeeping, latency modeling) still landing on `testnet` branch | **High** | `src/engine/`, `src/execution/`, `src/risk/`, threading, order tracker | Complete the full refactor phase, pass CI + manual shadow validation on mainnet before considering larger size |
| Cross-file safety invariants under change | Many live-critical paths (halt_flag_, reconciler, DMS heartbeat, kill switch) are explicitly flagged as high-risk for model edits | High | `tt_target.h`, `engine.cpp`, `risk_worker.h`, all `*kill_switch*`, `*dead_mans_switch*`, `*reconciler*` files | Freeze these areas until refactor stabilizes; require Opus-level review on any changes |
| Master branch frozen pending refactor | Root README states master is intentionally held back until phases are green | Medium | Entire project | Finish current phase and produce documented "green" mainnet shadow + small live run |

### 2. Risk Management Completeness

| Gap | Current State | Risk Level | Affected Components | To do |
|-----|---------------|------------|---------------------|-----------------------|
| Position-based risk limits | Only balance-based (`RiskManager`) + venue notional/leverage/liquidation-distance caps | **High** | `src/risk/risk_manager.cpp`, `futures_risk_check.h` | Implement proper position sizing risk engine (max position as % of equity, volatility-adjusted limits, etc.) |
| Full mark-price liquidation simulation | Approximation exists (`cash / notional - maintenance_margin`) but marked as partial in docs | High | `binance_futures_safety.h`, `FuturesRiskCheck` | Build accurate per-position liquidation price calculator using Binance tiered maintenance margin rates + funding |
| Funding rate integration | Shadow mode and P&L do not yet simulate funding; risk engine does not account for it | Medium-High | `shadow_tracker.h`, `portfolio.cpp`, analytics | Wire funding events from user-data stream into portfolio and risk checks |
| Cross-margin / multi-symbol risk | Engine is largely single-symbol today; cross-margin awareness is out of scope | Medium | `Portfolio`, `RiskManager`, futures provider | Add account-level margin ratio monitoring and cross-symbol exposure limits |
| No extreme event circuit breakers | No automatic pause on massive spread widening, funding spikes, or exchange anomalies | Medium | Risk layer + observer | Add configurable volatility / spread / funding circuit breakers |

### 3. Dead-Man's Switch & Kill Switch Limitations

| Gap | Current State | Risk Level | Affected Components | To do |
|-----|---------------|------------|---------------------|-----------------------|
| DMS only cancels orders (does not flatten positions) | Explicitly documented foot-gun: open positions remain after DMS fires | **High** | `binance_futures_dead_mans_switch.h` | Extend DMS to also submit reduceOnly market closes (or provide very clear operator SOPs + automation) |
| Process suspension (`SIGSTOP`) defeats DMS | Heartbeat thread pauses -> countdown fires while engine is frozen | High | Worker watchdog + DMS | Document more aggressively; consider adding a separate external watchdog process |
| Kill switch has hard deadline | If `reduceOnly` close misses deadline, operator must intervene manually | Medium | `binance_futures_kill_switch.h` | Improve retry + escalation logic; add external monitoring hooks |

### 4. Bracket / Exit Management on Futures

| Gap | Current State | Risk Level | Affected Components | To do |
|-----|---------------|------------|---------------------|-----------------------|
| Partial-fraction brackets declined | `BinanceFuturesBracketAdapter` rejects `qty_fraction != 1.0`; falls back to engine-side ExitManager | Medium | `binance_futures_bracket_adapter.h`, `ExitManager` | Either implement proper multi-leg conditional orders or make the fallback behavior more robust and tested |
| Bracket placement is non-atomic | Two separate `STOP_MARKET` + `TAKE_PROFIT_MARKET` POSTs with `closePosition=true` | Medium | `binance_futures_bracket_adapter.h` | Add placement retry + reconciliation logic; improve logging of partial bracket states |
| Golden regression coverage for complex brackets | Not yet comprehensive for futures multi-leg scenarios | Medium | `tests/test_engine_brackets.cpp`, golden files | Expand test matrix to cover futures bracket + reconciliation edge cases |

### 5. Persistence & Recovery Reliability

| Gap | Current State | Risk Level | Affected Components | To do |
|-----|---------------|------------|---------------------|-----------------------|
| QuestDB is soft-fail only | Unreachable -> warning + continues (data loss for that run) | Medium-High | `data/questdb/`, engine startup | Add `--persist-strict` / hard-fail mode + automatic retry + local checkpoint fallback |
| Checkpoint / recovery robustness | Checkpoint format exists but recovery story is underdeveloped | Medium | `src/engine/checkpoint.h` | Implement reliable crash recovery with position + open order replay from binary logs |
| Binary event log as primary audit source | Excellent when enabled, but not mandatory | Medium | `event_log.h` | Make structured binary logging + integrity verification mandatory in live mode |

### 6. Production Hardening & Observability

| Gap | Current State | Risk Level | Affected Components | To do |
|-----|---------------|------------|---------------------|-----------------------|
| No Prometheus / metrics export | Only TUI + QuestDB + ndjson | Medium | UI panels, workers | Add proper metrics endpoint or structured logging for external monitoring |
| Limited external alerting hooks | Operator must watch TUI or parse logs manually | Medium | Health panel, risk worker | Expose structured events for PagerDuty / Slack / Telegram on halt, large loss, DMS trigger, etc. |
| Secrets management | API keys via environment or CLI only | Medium | `main.inc`, credential scripts | Add encrypted credential store + key rotation support |
| No automatic tape rotation + offsite upload | Manual process | Low-Medium | Recording path | Build helpers for automatic tape management and cloud backup |

### 7. Testing & Provenance

| Gap | Current State | Risk Level | Affected Components | To do |
|-----|---------------|------------|---------------------|-----------------------|
| Limited long-duration mainnet shadow data | Most validation on testnet or short shadow runs | High | - | Publish (or at least internally accumulate) multi-day, multi-symbol mainnet shadow divergence statistics |
| Small-size mainnet live track record | Very few documented real-money runs | High | - | Require minimum 30-60 days of tiny-size mainnet live operation with zero incidents before increasing size |
| No public or internal "incident post-mortems" | None visible | Medium | - | Start collecting and reviewing every live incident (even tiny-size) |

### 8. Documentation & Operator Procedures

| Gap | Current State | Risk Level | Affected Components | To do |
|-----|---------------|------------|---------------------|-----------------------|
| User manual now exists but is still new | `docs/user-manual.md` was just generated | Medium | Documentation | Expand with decision trees, "what to do when X happens" runbooks, and full futures mainnet checklist |
| DMS + Kill Switch operator playbook | Excellent for testnet (`futures-testnet.md`); lighter for mainnet | Medium | - | Create dedicated mainnet "Live Trading Runbook" covering every failure mode |
| No formal change control process | Refactor work is moving fast | Medium | - | Define and enforce a change-control gate for any modification touching live order paths |

---

## Roadmap

**Phase 0 (current)** - tiny-size mainnet validation only, all safety nets armed

**Phase 1 (required before meaningful size)**
- Complete current deepdive refactor phase + full CI + manual shadow validation
- Implement position-based risk limits
- Extend DMS to also attempt position flattening (or very strong SOPs)
- Add hard-fail option for QuestDB + mandatory binary logging in live mode
- Expand futures bracket golden tests

**Phase 2 (strongly recommended)**
- Full mark-price liquidation engine with tiered maintenance margins
- Funding rate integration into P&L and risk
- Prometheus metrics + external alerting hooks
- Encrypted credential store
- Published multi-week mainnet shadow divergence report

**Phase 3 (nice to have)**
- Multi-symbol / cross-margin risk engine
- Automatic tape rotation + offsite backup
- Circuit breakers for extreme market events
- Formal incident post-mortem process and change control board

---

## Bottom line

The safety architecture (DMS, compile-time gating, reconciler,
user-data stream as source of truth) is ahead of most retail tooling.
That doesn't make the system safe for meaningful capital today: the
refactor is still moving, position-based risk management is incomplete,
and the DMS cancels orders without protecting positions. Those three
together are the blocker.

Tiny-size "prove the system" mainnet runs with an experienced operator
watching closely are acceptable. Nothing beyond that until the refactor
is done, the risk surface is hardened, real shadow + tiny-live data has
accumulated, and the operational tooling (alerting, credential
management, runbooks) exists. Realistically that is 3-6 months of work.

*This document should be updated after every major refactor phase and before any increase in live capital allocation.*