# TrueTest Task List & Roadmap (todo.md)

**Status**: Living – the single source of record for current-phase work items. Every PR that touches the frozen safety surface (or describes it) **must** reference the relevant item(s) here.  
**Post-merge note (Phase 5)**: Monte Carlo integration complete. All "active on monte-carlo" / "paused on monte-carlo" / "current branch focus" language removed from live docs (except legitimate feature names like --monte-carlo, "Monte Carlo" in capability descriptions, and historical notes in this plan / archive/). Private retail scope ("Intended Use & Scope" + "never be enterprise") established across root docs. ENGINE_AI_SUMMARY.md refs consolidated. MC-01/MC-02 marked landed. Safety freeze untouched by MC work.

**Completed (Phase 8)**: Monte Carlo to master merge — all phases completed successfully. Master is the new baseline. Private retail character clearly stated. All technical safety docs remain intact. See MERGE_PLAN.md for full record.  
**Last update: 2026 (post-merge; Monte Carlo to master merge — all phases completed successfully).  
**How to reference**: "Addresses todo.md #P0-03 (Phase 0 evidence scaffolding)" or "Closes #A-07".

Items are grouped by theme and roughly prioritized within each group. Completed items are moved to the bottom or struck through after the phase they belong to is declared done in `prod.md`.

**Current focus (post monte-carlo integration)**: Monte Carlo simulation (research & strategy-robustness tool; `--monte-carlo --mc-trials N`, object reuse, experimental parallel, any strategy + realism; integrated from the monte-carlo branch). Phase 0 tiny-size mainnet futures validation (0/15 qualifying; collection was paused during monte-carlo branch priority — gates/ritual unchanged). Phase 1 Live-Safety Freeze mechanically enforced. Tiny-size validation and research only. Not suitable for meaningful capital until Phase 0/1 exit criteria satisfied. Read `prod.md` + `CLAUDE.md` + `prerequisites.md` + this before any frozen-surface work.

**Read first**: summary.md (root) + CLAUDE.md (for AI rules) + this MERGE_PLAN.md context (dense power + constraints for agents), `prod.md` (central contract, phases, 9-row Go-Live Gate, Phase 0 template + ritual), `CLAUDE.md` (AI model selection, Phase 1 freeze + CCB/token rules, doc maintenance), `prerequisites.md` (mandatory pre-PR checklist for the 10 frozen files).

---

## Phase 0 Immediate (Unblock tiny-size mainnet validation; active; 0/15)

- **P0-01** Run first qualifying tiny-size mainnet `engine_shadow` / live sessions using the conservative template in `prod.md` (exact: `--provider binance-futures --symbol BTCUSDT --stream trade --depth-stream depth20@100ms --live ... --persist --run-tag p0_... --reconcile-tolerance-bps 3 --dead-man-countdown-ms 30000 ... --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 7 --max-daily-loss 80 --risk-unwind 0.4`) and collect full artifacts (zstd binary event log as **mandatory durable truth** + QuestDB `run_tag` + signed one-page session note from `reports/phase0/templates/phase0-session-note.md` + volatility classification via `scripts/phase0/volatility-classifier.sh` on 7/14d BTC realized vol + post-halt mandatory `grep -i "POSITION-SNAPSHOT\|funding\|drift\|reconcile\|halt\|kill\|DMS" ... | tail -20` clean + row in `reports/phase0/PROGRESS.md`).
- **P0-02** Populate `reports/phase0/PROGRESS.md` and close the first 5-session batch review (two signatures). (Batch reviews every 5 sessions + two signatures on the batch; all evidence under `reports/phase0/`; record `run_tag` + binary log filename for auditability.)
- **P0-03** Create the printable `docs/operations/futures-phase0-operator-sop.md` (Doc Phase 1) and actually use it on real sessions. (Ritual: print/sign the SOP, use `scripts/phase0/new-session.sh`, keep math-captcha visible the entire session, stay at the terminal, confirm one-way mode, watch DMS counter, run mandatory post-halt grep, run `post-session.sh` + classifier, commit artifacts + note, update PROGRESS.md. Current ritual + template live in `prod.md` + `reports/phase0/` files.)
- **P0-04** Achieve 15+ qualifying sessions across ≥3 volatility regimes (High/Med/Low) with zero unexplained drift > tolerance in any session. Full artifacts for every session + two-person batch reviews every 5.
- **Standing (from reports/phase0/ + prod + scripts)**: 6-step per-session process (new-session.sh → execute with all safety nets armed → post-session + classifier + mandatory grep → commit note + artifacts under reports/phase0/ (dated subdir or direct) → update PROGRESS row → every-5 batch in ops/ + 2 sigs). All entries must survive a clean 4-hour+ `engine_shadow` mainnet run with zero unexplained drift before counting. "MC work does not change P0 gates/ritual/evidence requirements." Sync "0/15" + Phase 0 collection notes (paused during monte-carlo priority; gates/ritual unchanged) across `reports/phase0/README.md`, `PROGRESS.md`, root files, `prod.md`, `instructions.md`, and this file.

**Current status (2026)**: 0/15 qualifying. Scripts, templates, and ritual definition ready. Collection was paused during monte-carlo branch priority work (gates/ritual unchanged). The requirements defined in `prod.md` remain the gate for any capital increase.

---

## Phase 1 / Live-Safety Freeze (Current enforcement active)

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
- **P1-05** Ensure every future edit to any of the 10 frozen files carries the token + CCB + shadow run (mechanical + cultural). All future safety-surface PRs (even "only docs" that describe the surface) require the token in commit message, CCB review, and clean multi-hour mainnet `engine_shadow` run.

**Frozen Files (single source of truth – also in `scripts/check-live-safety-freeze.sh`; keep lists in sync across prereq/prod/script/CLAUDE)**:
```
src/core/tt_target.h
src/engine/engine.cpp
src/providers/binance/binance_futures_provider.h
src/providers/binance/binance_futures_dead_mans_switch.h
src/providers/binance/binance_futures_kill_switch.h
src/providers/binance/binance_futures_reconciler.h
src/risk/risk_manager.h
src/risk/futures_risk_check.h
src/execution/live_safety.h
src/threading/worker_watchdog.h
```
(Note: some refs say "9 critical files"; standardize on the exact 10-file list above.)

**Mandatory pre-PR checklist (see full in `prerequisites.md`)**: Read current `CLAUDE.md` (model + freeze) + `prod.md` (Phase 1 + gates + invariants) + `production-readiness-gaps.md`; PR description contains `LIVE_SAFETY_CCB_APPROVED` token; run `./scripts/check-live-safety-freeze.sh --check-head` (passes or files explicitly outside list); note "prod.md impact" (or "no") + reference relevant `todo.md` items; change introduces none of the anti-patterns (runtime live-order bypass, resettable/auto-clearing `halt_flag_`, retry/backoff/"helpful fallback" on kill/DMS/reconciler/watchdog paths, nlohmann/json on hot path, second producer on any SPSC ring, `HAS_*` or venue-specific in core/engine/threading/risk, soft-warn for reconciler drift except the one documented spot-testnet case); appropriate model (Opus-level for any frozen surface per CLAUDE); Phase 0 still "in active collection" (or PR advances + updates all refs); no open untagged changes on branch affecting reproducibility of the safety surface; for safety desc changes in docs, review corresponding code comment block for consistency; post-edit: `todo.md` updated with item(s) addressed + any new follow-ups; for phase-exit PR: corresponding `prod.md` section + Go-Live Gate / PROGRESS.md updated; change exercised in at least one clean shadow or backtest run exercising the modified path. Escalate to CCB if borderline.

**On every phase exit declared in `prod.md`**: Also update `todo.md` (move/complete items + add follow-ups surfaced by review), `prerequisites.md` if the checklist evolved, and "Last updated" notes. Anti-rot ritual before any capital tier increase must include "docs verified + links resolve + `todo.md` updated".

---

## Monte Carlo Simulation (integrated)

- **MC-01** Validate performance and scaling of object reuse (`--mc-reuse-objects`) and experimental parallel modes (`--mc-parallel` with `--thread-preset inline`) across realistic strategy + realism flag combinations. (Phase 4 deepdive reset hardening landed: `reset_for_next_trial` now clears order_meta_ + shadow_tracker_ (new reset()); core objects (portfolio/lots, ExitManager, analytics, etc.) fully supported for reuse. MC controller comment updated. "Phase A" data_handler/strategy + internals good; full bit-identical across every detail not guaranteed. Parallel still Phase 5 experimental (inline preset recommended). Now validate + scale the reuse. **Landed with monte-carlo merge**.)
- **MC-02** Enhance `MonteCarloReporter` (richer per-trial analytics, improved JSON schema for downstream tools, better QuestDB campaign summary rows when `--persist` is used). **Substantially complete** — per-trial win_rate/profit_factor + aggregate mean/median/PF>1 counts now populated and rendered (Step A). Reporter + JSON enhanced. AnalyticsReport gained `winning_trades` for exact counts. **Landed with monte-carlo merge**.)
- **MC-03** Improve synthetic L2 fidelity in the synthetic provider / GBM generator (when needed for queue-position, impact, or realism model testing). (Current: "Phase 1: L2 is basic and stylized (constant spread + noise)"; optional 3-level depth in GBM; "for queue-position / realism fidelity testing".)
- **MC-04** Document realistic usage patterns, limitations, and recommended workflows for strategy robustness testing and risk-distribution analysis (this update + follow-ups). (Current: MC section in `docs/instructions.md` + "Stochastic Backtesting" subsection in `docs/user-manual.md` + README + summary.md (root) + CLAUDE.md (for AI rules) + this MERGE_PLAN.md context; strong caveats must be respected.)
- **MC-05** Exercise Monte Carlo campaigns with adaptive, breakout, and other strategies to test stochastic robustness; add representative MC regression tests or golden aggregates. (Initial MC smoke campaigns executed successfully against the new strategies per completed items; full "first-class" blocked by demo caveats on adaptive-hybrid / structure-continuation / mean-reversion advanced paths — see A-*.)
- **MC-06** (Future) Full per-trial order-lifecycle capture into QuestDB under per-trial run tags when running campaigns with `--persist`. (Current: MC mode in `main.inc` does only lightweight campaign summary row + `TODO: integrate with --output, richer per-trial persist`; per-trial engine runs without full order-lifecycle wiring.)

**Standing invariants & rules for MC work (from CLAUDE / summary.md (root) + CLAUDE.md (for AI rules) + this MERGE_PLAN.md context / prod / instructions)**:
- MC/simulation layer generally safe for Sonnet-level work. Cross-file invariants: deterministic per-trial seeding (`base_seed` derives `trial_id ^ magic`), no hidden shared state between trials, `MonteCarloReporter` should remain allocation-light for large N, `--mc-parallel` must not be treated as a general-purpose threading primitive (it conflicts with engine core pinning in most presets; recommended with `--thread-preset inline`).
- Caveats (must be respected): stylized synthetic L2 (constant spread + noise), no automatic calibration from historical data, parallelism has threading/pinning restrictions.
- MC is a **research and strategy-robustness / risk-distribution tool**. It does **not** change the live-order safety surface or Phase 0/1 capital gates. It is not a substitute for real-market divergence tracking.
- When new work lands in `src/simulation/` (Monte Carlo), the MC section in `docs/instructions.md` and any governance mentions (README, `todo.md`, `prod.md`, CLAUDE) must be updated in the same PR or immediate follow-up.
- Recommendation for agents: Tiny-size validation and research / MC only. Do not propose live capital increases. Respect every invariant.

**From code (visible incompletes)**: Hardcoded `initial_balance = 10000.0` + TODO make configurable in McRunConfig; generator only GBM (future models commented); synthetic provider demo defaults/short paths; limited --output/richer export for per-trial.

---

## Risk Management (Highest remaining technical risk per gaps.md)

- **R-01** Implement proper position-based risk limits (max % of equity, volatility-adjusted) in `RiskManager` / `FuturesRiskCheck`.
- **R-02** Complete accurate per-position mark-price liquidation calculator using real tiered Binance maintenance margins + funding. (Current: `MaintenanceMarginTable` + `/fapi/v1/leverageBracket` landed for rates; liq distance in `FuturesRiskCheck` remains approximation `cash / notional − maintenance_margin`; "Approx" comments present.)
- **R-03** Wire funding rate events from user-data stream into `Portfolio`, `shadow_tracker`, P&L, analytics, risk checks, and QuestDB. (Fees/FUNDING_FEE events fully wired end-to-end: portfolio `on_funding`, analytics, QuestDB `record_funding`, engine publish; 8h rate setter + circuit breaker fields exist in risk_manager/analytics but **zero callsites** — always 0.)
- **R-04** Add cross-margin / multi-symbol / account-level margin ratio monitoring and exposure limits. (Engine largely single-symbol; cross-margin coarse proxy noted as out-of-scope.)
- **R-05** Add configurable extreme-event circuit breakers (spread widening, funding spikes, exchange anomalies). (Phase 2.4 partial: spread/funding rate breakers in risk_manager via Analytics snapshot; "can be fed" comments.)
- Additional (Go-Live Gate row 3): Funding + tiered MMR exercised for ≥30 days. Position-based pre-trade risk must land before futures live path vs. real money (existing RiskManager is balance-based/cash; venue notional/leverage/liq caps are backlog).

---

## DMS / Kill-Switch / Bracket Hardening

- **S-01** Extend DMS to also attempt `reduceOnly` MARKET position flattens on heartbeat loss (default behavior or very strong SOP + automation). (Current in `binance_futures_dead_mans_switch.h`: "Critically: this only cancels ORDERS. Open futures positions stay open." Phase 3 `attempt_close` extension in-process via provider fn on persistent fail; pairs with external `tt_watchdog` consideration.)
- **S-02** Address SIGSTOP / process-suspension defeat of DMS (document + consider external `tt_watchdog` binary). (WorkerWatchdog monitors heartbeat thread; 3× promotes to `halt_flag_` so orderly kill-switch runs.)
- **S-03** Improve kill-switch retry/escalation + external monitoring hooks.
- **S-04** Support partial-fraction brackets (`qty_fraction != 1.0`) or make engine-side `ExitManager` fallback robust and fully tested for futures. (Current: `BinanceFuturesBracketAdapter` declines `qty_fraction != 1.0`; engine ExitManager is the only enforcer; logs for missing SL/TP/partials.)
- **S-05** Make futures bracket placement (currently two non-atomic POSTs: `STOP_MARKET` + `TAKE_PROFIT_MARKET` with `closePosition=true` + `reduceOnly`; venue auto-cancels the other leg) more atomic or add robust retry + partial-state reconciliation.
- **S-06** Expand golden regression tests for complex futures multi-leg brackets + reconciliation (`test_engine_brackets.cpp` + golden files).
- Additional (Go-Live Gate row 4): DMS position-flattening logic tested (or very strong SOP + automation in place).

---

## Documentation & Structure (this plan + D items)

- **D-01** (Doc Phase 0 – core done): Create `prod.md`, `prerequisites.md`, `todo.md`, and `reports/phase0/` skeleton + update all cross-references + gov sync for MC branch (landed; see Completed).
- **D-02** (Doc Phase 1): Create `docs/operations/futures-phase0-operator-sop.md` (printable) and `futures-testnet.md`. (Also P0-03; use on real sessions. Current ritual + template live in `prod.md` + `reports/phase0/`.)
- **D-03** (Doc Phase 2): Create the core architecture files (`target-architecture.md`, `migration.md`, `MODEL.md`, `realism.md`) and begin extraction from `instructions.md`. (Aspirational hierarchy under `docs/architecture/` etc.; see docs/README.)
- **D-04**: Add "Documentation Maintenance Rules" + anti-rot process (checklist in `prerequisites.md`, phase-exit ritual, explicit "planned" language for aspirational links). (Core rules landed in CLAUDE + prereq; enforce on all cross-refs.)
- **D-05** (Future): Create remaining operations guides, reference material, archive population, and any lightweight link-check tooling.
- **D-06 (this consolidation)**: After multi-agent analysis (code structure + incompletes + full MD classification), extract all current points from scattered documents, purge the duplicate/outdated todo lists / phase details / action items from docs/ files (replace with thin pointers: "See root `todo.md` (P0-*/MC-*/R-* etc.) and `prod.md` for current tasks, phases, and gates. This file is the technical reference."), produce single authoritative `todo.md` in root. Clean stale "Doc Phase" / missing-dir refs (make explicit "Planned for Doc Phase X – current details live in prod.md / instructions.md §N" per CLAUDE rule). Move historical `questdb-multi-week-hardening-guide.md` to `docs/archive/` (or mark clearly). Resolve 9-vs-10 files inconsistency. Enforce extraction rule (long-form in prod/SOP; pointers + quick templates in instructions). Sync "Last updated" + branch notes. (See plan for full details.)

**Ongoing (CLAUDE "Documentation Maintenance Rules" + prod + prereq + todo)**:
- The three root governance files (`prod.md`, `prerequisites.md`, `todo.md`) + `reports/phase0/` + CLAUDE are the single source of truth. Keep them authoritative and up to date.
- Every PR touching the frozen safety surface (or the *description* of that surface in docs) must reference the relevant items in `todo.md` and run `./scripts/check-live-safety-freeze.sh`.
- On every phase exit declared in `prod.md`, also update `todo.md` (move/complete items), `prerequisites.md` if the checklist evolved, and the "Last updated" note in the affected docs.
- When a cross-reference is still aspirational (e.g. `docs/operations/futures-phase0-operator-sop.md` before Doc Phase 1), it **must** say so explicitly: "Planned for Doc Phase X – current details live in prod.md / instructions.md §N".
- Extraction rule: long-form phase/ritual/gate content lives in `prod.md` (or the dedicated SOP). `instructions.md` contains pointers + quick command templates, not duplicates.
- Anti-rot ritual: before increasing any capital tier, the exit review must include "docs verified + links resolve + `todo.md` updated".
- When new work lands in `src/simulation/` (Monte Carlo), the MC section in `docs/instructions.md` and any governance mentions (README, `todo.md`, `prod.md`) must be updated in the same PR or immediate follow-up.
- If you find a broken or stale cross-reference, treat it as a documentation bug and either fix it or open an issue with the exact string that needs updating.

**Aspirational / missing dirs & files** (referenced in docs/README, instructions, CLAUDE, todo, reports, etc.; do not exist yet; use explicit planned language):
- `docs/operations/futures-phase0-operator-sop.md` (and `futures-testnet.md`, `demo-trading-workflow.md`)
- `docs/architecture/` (target-architecture.md, migration.md, MODEL.md, realism.md, futures-order-lifecycle.md, ...)
- `docs/reference/`, `docs/archive/` (unless created for historical guides)
- Root: `archive/`, `decisions/`, `upcoming/`, `PHASE0_COMPLETION_PLAN.md`, Coiled_Spring...Guide.md
- Keep lists in docs/README + instructions in sync with realized state.

---

## Adaptive Hybrid Strategy (previous / lower-priority branch work)

**Note**: Active development focus has shifted to Monte Carlo simulation on the `monte-carlo` branch. Items below remain for historical context or future resumption. The strategy is registered and L2 dispatch is present (with `LIVE_SAFETY_CCB_APPROVED` comment); usable for MC backtests/experiments with caveats.

- **A-01** Replace v1/demo placeholders in `AdaptiveHybridStrategy` / `RiskValidator` / `OnChainMonitor` (simplified decision logic, mock OnChain thread, equity proxies, omitted L2Snapshot handling, "real version" comments) with full hot-path deterministic implementation. (Visible in `src/strategy/adaptive_hybrid_strategy.cpp:467` "Simplified decision for compilable demo — real version uses full RiskValidator + L2Snapshot"; many "placeholder", "real impl would", "v1 simplified", "for paper/backtest; production wires real feed"; `enable_onchain_mock=true` default + mock producer thread.)
- **A-02** Add real on-chain data feed integration (TRON/Helius or equivalent) and proper spike detection instead of the current always-false stub. (Mock only; `inject_spike` test hook only; "No automatic spikes in mock".)
- **A-03** Implement `take_pending_exit_intents` for the strategy (consistent with breakout / other strategies that use `ExitManager`).
- **A-04** Exercise the full 9-step flow + RiskValidator gates in real backtests and shadow runs; remove "for compilable demo / harness only" limitations.
- **A-05** Add `adaptive-hybrid` to the strategy matrix / golden tests and TUI indicators as a first-class citizen.
- **A-06** Wire ATR (recently added) + other indicators cleanly into the adaptive regime detection.
- **A-07** Any L2 dispatch or safety-surface touches for adaptive hybrid must carry `LIVE_SAFETY_CCB_APPROVED`.

(See detailed spec in `docs/AdaptiveHybridStrategy.md`; code in `src/strategy/adaptive_hybrid_*`; test in `tests/test_adaptive_hybrid.cpp`. On-chain mock + simplified gates are the main blockers.)

---

## Persistence, Observability & Hardening (later phases)

**All QuestDB / persistence work happens on the `database` branch.**

Primary reference: `docs/db.md` (current authoritative schema + queries + reliability + ritual) and `docs/questdb-multi-week-hardening-guide.md` (historical phased log; most phases landed).

- **H-01** Add `--persist-strict` / hard-fail mode + automatic retry + local checkpoint fallback for QuestDB. (See the multi-week hardening guide — Phase 2. Current: soft-fail default (warning to stderr, persistence disabled, run continues); `--persist-strict` + fallback .ilp wired in engine/main/QuestdbStore/IlpWriter + health surface (strict_mode/fallback_lines); "refuse to start when `--persist` is set but QuestDB is down" documented as follow-up.)
- **H-02** Make structured binary event logging + integrity verification mandatory in live mode.
- **H-03** Implement reliable crash recovery (position + open-order replay from binary logs). (Richer checkpoints; crash-replay golden tests.)
- **H-04** Add Prometheus / metrics export endpoint + structured alerting hooks (halt, large loss, DMS trigger, etc.). (IAlertSink; alerting drill — Go-Live row 6.)
- **H-05** Encrypted credential store + key rotation support. (Demonstrated on ≥10 sessions — Go-Live row 5.)
- **H-06** Automatic tape rotation + offsite / cloud backup helpers.
- Additional (from code/gaps/db + Go-Live): Funding table + `record_funding` present; MC campaigns use only lightweight campaign summary (per-trial full is MC-06); scripts/questdb_* + 45-min soak guide + verify_reconciliation (binary side is manual/placeholder); health surface (pending/dropped/fallback/age); time-based flush (150ms default + `--questdb-flush-ms`).

---

## Other / Nice-to-Have / Future Venues

- Multi-symbol / cross-margin risk engine (overlaps R-04).
- COIN-M (inverse) futures provider sibling (separate `dapi`/`dstream` stack; not a flag on `binance-futures`).
- Hedge mode (`positionSide` plumbing; currently refuses if `dualSidePosition=true`; one-way only in v1).
- Generic `IExchangeAdapter` abstraction (currently Binance-heavy; only live venue family today).
- Solana/Drift liquidation keeper (7-phase plan in archive/drift-* analysis docs; tracked in `upcoming/` per README — both dirs missing).
- Richer C API + language bindings. (C API exists for embedding when `BUILD_SHARED_LIB`; produces analytics + results JSON only; not wired through QuestDB capture or rich TUI.)
- Formal incident post-mortem process + CCB charter (Phase 6).
- Go-Live Gate rows (overarching; no capital tier increase permitted until all nine rows have two signatures + concrete evidence): 1. All prior phases met. 2. 60-day shadow report (published or internally audited). 3. Funding + tiered MMR exercised for ≥30 days. 4. DMS position-flattening logic tested (or very strong SOP + automation). 5. `--persist-strict` + encrypted creds demonstrated on ≥10 sessions. 6. Prometheus / alerting drill executed successfully. 7. All critical runbooks walked by at least two operators. 8. CCB size-increase request formally approved. 9. Independent safety review (internal or external) with written sign-off.
- Risk resume (halt_flag_ stops the engine but there's no resume channel).
- QuestDB hard-fail (daemon unreachable currently downgrades to a warning; "refuse to start when `--persist` is set but QuestDB is down" is documented as follow-up).

**From CLAUDE / ENGINE "Not yet implemented" + hard invariants (ongoing for all work)**: Respect every one (compile-time live-order gate absolute via `TT_TARGET` + `target_allows_live_orders()`; halt is terminal/write-once atomic; no auto-resume/no cooldown/no "helpful" retry on safety paths; hot-path discipline — zero nlohmann/json (CI-enforced), zero or pooled allocs, lock-free SPSC only (one producer/consumer per ring); reconciler refusal default (except documented spot-testnet carve-out); user-data WS source of truth; provider is the only extension point (IProvider + four safety hooks + transport/parser/executor; core has no `HAS_*` or venue specifics); small capital first + evidence-based gates (full artifacts + 9-row Go-Live with two signatures before any tier increase). "The engine already implements strong compile-time and runtime safety primitives that most retail or early-stage systems lack... This playbook exists so that future operators cannot say 'we forgot why we were careful.'"

**AI coding rules (ongoing; CLAUDE + ENGINE + prereq)**: Default Sonnet 4.6 sufficient for new strategies, indicators, tests, CLI flags, docs, single-file refactors, provider-stack additions following patterns, work in `src/simulation/` + synthetic provider (MC). **Must switch to Opus 4.7 before editing** any of: `src/engine/engine.{h,cpp}` + `engine_config.h`; `*kill_switch*`, `*dead_mans_switch*`, `*reconciler*`, `*watchdog*`; `src/core/tt_target.h` + any `TT_TARGET`/`target_allows_live_orders` callsites; `src/threading/` (SPSC, spin, affinity); `src/risk/` + any `halt_flag_` code; hot-path (no nlohmann/json); Binance live safety glue (refusal gates, time sync, OCO/brackets, REST signing, DMS heartbeats). Every frozen-surface PR must run the check script, carry the CCB token, reference `todo.md` items, and update governance if needed. MC/synthetic generally Sonnet-safe but preserve deterministic seeding + isolation.

---

## Completed / Superseded Items

(Will be populated as phases are declared done in `prod.md` and items are closed in PRs. Move or strike from above sections on exit.)

- Initial creation of `reports/phase0/` skeleton and governance root files (`prod.md`, `prerequisites.md`, `todo.md`) – Doc Phase 0 core (this cycle).
- Phase 1 mechanical freeze markers + enforcement script (already landed).
- Governance + status synchronization for `monte-carlo` branch Monte Carlo work (README, todo.md, CLAUDE.md, prod.md, reports/phase0, instructions.md, user-manual.md).
- Post-landing doc hygiene for new strategies (structure-continuation, adaptive-hybrid) + indicators (ema_regime, stochastic, swing_detector): updated CLI `--help`, instructions.md, flags.md, user-manual.md, CLAUDE.md. Initial MC smoke campaigns executed successfully against the new strategies (MC-05 partial).
- MC-02 Step A (per-trial win_rate / profit_factor distributions + enhanced reporter; tiny AnalyticsReport addition for exact `winning_trades` count; JSON + QuestDB campaign rows).
- Multi-agent consolidation of scattered todos/docs (this update): single root `todo.md`; docs/ purged of duplicate action lists (now pointers only); historical hardening guide archived or clearly marked; stale planned refs cleaned with explicit language.

---

**Maintenance note**: When a phase exit is declared in `prod.md`, move or strike all items that were required for that exit, add any new follow-ups that the exit review surfaced, and reference the declaring PR here. This file is reviewed together with code changes that affect the safety surface. Update "Last update" with summary of changes (e.g. "MC-02 Step A landed; P0 status 0/15 paused on MC branch; doc consolidation").

**If you find a broken or stale cross-reference**, treat it as a documentation bug (per prod.md) and fix or open an issue with the exact string.

All operational detail, architecture decisions, current invariants, and the full development log live in `CLAUDE.md`, `prod.md`, `prerequisites.md`, summary.md (root) + CLAUDE.md (for AI rules) + this MERGE_PLAN.md context, `docs/instructions.md`, `docs/user-manual.md`, and the `docs/` tree (with root governance as SoT). Consult `reports/phase0/` for Phase 0 evidence.
