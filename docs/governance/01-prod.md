# TrueTest Production Readiness Playbook (prod.md)

**Status**: Authoritative – the central production contract and capital-tier governance document.  
**Last major update**: 2026 (post-merge; Monte Carlo to master merge — all phases completed successfully; todos/ split cross-refs 2026-07)  
**Owners**: Core maintainers + Phase 0/1 operators (every phase exit must update this file).

This document defines the exact phase definitions, capital-tier gates, Go-Live checklist, Phase 0 operator ritual, and "why we are careful" philosophy. It is the single source of truth that reviewers, CCB members, and future operators consult before any increase in live capital.

See also:
- `../CLAUDE.md` (AI + human reviewer rules + live-safety freeze mechanics)
- `02-prerequisites.md` (mandatory pre-PR checklist for the frozen safety surface)
- `03-todo.md` (thin high-level canonical task list; every frozen-surface PR must reference items here or precise docs/todos/ e.g. docs/todos/01-P0-phase0.md#P0-01 per 00-OVERVIEW.md)
- `reports/phase0/PROGRESS.md` (evidence tracker)
- Historical snapshot: `../archive/production-readiness-gaps-2026-05.md` (May 2026 view; current gaps/status tracked in `03-todo.md` + docs/todos/ + this file)

**docs/ is now the central authoritative documentation home.** Last updated: 2026-07 (docs overhaul). This file (01-prod.md) is the source of truth for phases/gates/ritual. Other files use short pointers.

---

## Philosophy & Invariants

> "The engine already implements strong compile-time and runtime safety primitives that most retail or early-stage systems lack... This playbook exists so that future operators cannot say 'we forgot why we were careful.'"

**Core non-negotiable rules** (repeated in CLAUDE, instructions, architecture docs, and every safety review):
1. Compile-time live-order gate is absolute (`TT_TARGET` + `target_allows_live_orders()` in `src/core/tt_target.h`).
2. Halt is terminal (`halt_flag_` is write-once; only manual restart clears it).
3. Safety paths are loud and non-retrying (no auto-resume, no helpful fallbacks on kill/DMS/reconciler/watchdog).
4. Hot-path discipline (no `nlohmann::json`, no allocations, lock-free SPSC only, CI-enforced).
5. Reconciler refusal is default (drift > tolerance blocks live start).
6. User-data WebSocket is source of truth.
7. DMS protects orders only (Phase 3 work to add position flattening).
8. Futures mandates (one-way mode, `reduceOnly` + `closePosition=true` brackets, venue `FuturesRiskCheck` first).
9. Small capital first + evidence-based gates (no tier increase without artifacts + two signatures on the full Go-Live Gate).

**Intended Use & Scope**: TrueTest is a private, personal research and retail tool for the author only. It is not, and will never be, an enterprise-ready, institutional, or production trading system. Monte Carlo simulation, high-fidelity backtesting, and shadow divergence analysis are the primary mature capabilities. The live execution paths (`engine_live`) exist with unusually strong compile-time (`TT_TARGET`) and runtime safety layers (reconciler, DMS, kill-switch, venue risk checks, terminal halt, user-data source of truth, etc.). Any use of live paths is experimental, tiny-size, fully attended by the operator, and done at the author's own risk. The Phase 0/1 rituals and Go-Live language in this repository describe the author's personal evidence-gathering hygiene and self-imposed discipline — they are **not** a formal production release process or claim of readiness for others.

---

## Capital-Tier Phases & Exit Criteria

**Strict rule** (for the author only): Moving to a higher personal capital tier requires all prior phase exit criteria + two-person sign-off on the 9-row Go-Live Gate table below. The author will never treat this as a path to external or institutional use.

### Phase 0 — Safe Tiny-Size Mainnet Futures (Current Active Phase)

**Exact recommended command template** (conservative caps; must meet or exceed these for a qualifying session):

```bash
./build/engine_live \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --live \
  --api-key "${BINANCE_FUTURES_KEY}" --api-secret "${BINANCE_FUTURES_SECRET}" \
  --persist --run-tag p0_$(date +%Y%m%d_%H%M) \
  --reconcile-tolerance-bps 3 \
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \
  --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 7 \
  --max-daily-loss 80 --risk-unwind 0.4
```

**Why each element is mandatory**:
- `--depth-stream depth20@100ms`: enables realistic queue/impact/L2 models in shadow.
- `--persist`: **binary zstd event log is the mandatory durable truth**; QuestDB is a secondary, queryable observability store (see questdb-multi-week-hardening-guide.md).
- DMS + reconciler + three futures risk caps + daily-loss/unwind: layered safety nets.
- Tiny notional + low leverage + conservative liq distance: "prove the system, not the P&L".

**Exit criteria for Phase 0 → Phase 1**:
- 15+ fully documented qualifying sessions across ≥3 volatility regimes (High/Med/Low, classified via `scripts/phase0/volatility-classifier.sh` on 7/14d BTC realized vol).
- Zero unexplained drift > tolerance in any session.
- **Full artifacts for every session**: zstd binary event log, QuestDB `run_tag`, signed one-page session note (template in `reports/phase0/templates/`), row in `reports/phase0/PROGRESS.md`, post-halt `grep` review for POSITION-SNAPSHOT/funding/drift.
- Two-person batch reviews every 5 sessions.
- All evidence committed under `reports/phase0/`.

**Ritual** (see the printable SOP planned in `../operations/futures-phase0-operator-sop.md` — "Planned for Doc Phase X – current details live in this file + reports/phase0/"):
Print/sign the SOP, use `new-session.sh`, keep math-captcha visible the entire session, stay at the terminal, confirm one-way mode, watch DMS counter, run mandatory post-halt grep, run `post-session.sh` + classifier, commit artifacts + note, update PROGRESS.md.

**Current status (2026-05)**: 0/15 qualifying. Scripts and templates exist and are ready. First real tiny-size mainnet validation runs are the immediate focus on the active branch.

### Phase 1 — Deepdive Stabilization & Live-Safety Freeze (Required before meaningful size)

**Already completed in planning / mechanical artifacts**:
- 10 files carry the `LIVE-SAFETY SURFACE — Phase 1 freeze` marker (see `scripts/check-live-safety-freeze.sh` for the exact list: `tt_target.h`, `engine.cpp`, futures provider live block, dead_mans_switch, kill_switch, reconciler, risk_manager, futures_risk_check, live_safety, worker_watchdog).
- Enforcement script wired into pre-commit + CI.
- CLAUDE.md and reference/01-instructions.md updated with model-selection + CCB rules.
- `02-prerequisites.md` created (mandatory pre-PR checklist).

**Remaining exit criteria**:
- Complete the current deepdive + per-lot / queue-position / hybrid executor refactor.
- Clean 8-hour (or longer) mainnet `engine_shadow` run with zero drops / unexplained divergence.
- Two-person sign-off recorded in `decisions/phase1-freeze-*.md` (or equivalent under the decisions/ tree).
- Update this file and `03-todo.md` (thin) + relevant docs/todos/ file to mark Phase 1 complete.
- All future edits to any frozen file must carry the token `LIVE_SAFETY_CCB_APPROVED` in the commit message + pass the check script + 4h+ shadow validation.

**All future safety-surface PRs** require the token, CCB review, and clean shadow run even if they are "only docs" that describe the surface.

### Phase 2 — Risk Engine Completion (Highest Impact Remaining Work)

- Funding as first-class event (wired into Portfolio, analytics, risk, QuestDB, TUI, circuit breakers).
- Real tiered maintenance-margin liquidation simulation (already partially landed via `MaintenanceMarginTable` + `/fapi/v1/leverageBracket`).
- Position sizing as % of equity + volatility-adjusted limits.
- Configurable extreme-event circuit breakers (spread, funding spikes, etc.).

**Monte Carlo simulation (research & validation tooling)**

Monte Carlo simulation (introduced on the monte-carlo branch and now mainline) is a research and strategy-robustness tool for stochastic backtesting (`--monte-carlo --mc-trials N`, `--provider synthetic`, object reuse between trials, experimental parallel execution). It reuses the existing strategy, realism, analytics, and ExitManager surfaces.

**This is a research and risk-distribution tool.** It does not replace or accelerate the Phase 0/1 mainnet shadow/live evidence requirements, does not change the live-order safety surface, and should not be used as a substitute for real-market divergence tracking. See `../reference/01-instructions.md` (Monte Carlo section) and `03-todo.md` (MC-* items) for current status, limitations (stylized synthetic L2, experimental parallelism, etc.), and open work items.

Status notes live in `03-todo.md` + docs/todos/ (and historical `../archive/production-readiness-gaps-2026-05.md`).

### Phases 3–6 (High-Level Roadmap)

- **3**: DMS attempts position flattening (`reduceOnly` MARKET) on heartbeat loss + external `tt_watchdog` binary (defense vs SIGSTOP).
- **4**: `--persist-strict` (hard-fail), mandatory binary logging with integrity (xxhash), richer checkpoints, crash-replay golden tests.
- **5**: Prometheus + `IAlertSink`, encrypted credential store + rotation, expanded runbooks.
- **6**: 60+ day continuous mainnet shadow divergence report, formal incident post-mortems for every halt, CCB charter + decision log, signed capital-tier exit review.

---

## Final Go-Live Gate Table (9 Rows)

**For Private Retail Use Only**: These gates, phases, and rituals describe the author's personal evidence-gathering discipline and self-imposed limits for tiny, fully attended personal experiments only. They are not a formal release process or claim of readiness for any other use or capital.

**No increase in the author's personal live capital tier is permitted until all nine rows have two signatures + concrete evidence.**

1. All prior phases met.
2. 60-day shadow report (published or internally audited).
3. Funding + tiered MMR exercised for ≥30 days.
4. DMS position-flattening logic tested (or very strong SOP + automation in place).
5. `--persist-strict` + encrypted creds demonstrated on ≥10 sessions.
6. Prometheus / alerting drill executed successfully.
7. All critical runbooks walked by at least two operators.
8. CCB size-increase request formally approved.
9. Independent safety review (internal or external) with written sign-off.

---

## How to Use This Document

- **Preparing a Phase 0 session** → Read the Phase 0 section + the operator SOP + run `new-session.sh`.
- **Reviewing a PR that touches safety** → Read the Phase 1 freeze rules + `prerequisites.md` + run the check script.
- **Considering any increase in the author's personal live capital** → Read the entire Go-Live Gate table + the most recent `reports/phase0/PROGRESS.md` + current `03-todo.md` (or docs/todos/ for items). (Historical May 2026 gaps view lives in `../archive/production-readiness-gaps-2026-05.md`.) When the author chooses to collect evidence toward personal live use...
- **Updating after a phase exit** → Edit this file (mark the phase complete, record sign-offs, update the roadmap), update `03-todo.md` + relevant docs/todos/*.md , and reference the PR in the migration log.

**If you find a broken or stale cross-reference**, treat it as a documentation bug and either fix it or open an issue with the exact string that needs updating.

---

*This playbook is deliberately repetitive on the invariants. Future operators must be able to read only this document (plus the SOP for the current phase) and still understand exactly why every safety mechanism exists and what the capital gates require.*

**Last updated: 2026-07 (docs overhaul)** — docs/ is now the central authoritative documentation home. Cross-refs updated to use docs/governance/ paths.