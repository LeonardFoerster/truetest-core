# Other / Nice-to-Have / Future Venues + Go-Live + Invariants

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. Cross-ref prod.md Go-Live table (long-form stays in prod). See AGENTS.md + prod.md + engine for invariants.

- Multi-symbol / cross-margin risk engine (overlaps R-04).
- COIN-M (inverse) futures provider sibling (separate `dapi`/`dstream` stack; not a flag on `binance-futures`).
- Hedge mode (`positionSide` plumbing; currently refuses if `dualSidePosition=true`; one-way only in v1).
- Generic `IExchangeAdapter` abstraction (currently Binance-heavy; only live venue family today).
- Solana/Drift liquidation keeper (7-phase plan in archive/drift-* analysis docs; tracked in `upcoming/` per README — both dirs missing).
- Richer C API + language bindings. (C API exists for embedding when `BUILD_SHARED_LIB`; produces analytics + results JSON only; not wired through QuestDB capture or rich TUI.)
- Formal incident post-mortem process + CCB charter (Phase 6).

- Go-Live Gate rows (overarching; no capital tier increase permitted until all nine rows have two signatures + concrete evidence): 1. All prior phases met. 2. 60-day shadow report (published or internally audited). 3. Funding + tiered MMR exercised for ≥30 days. 4. DMS first-failure to centralized-kill handoff and process-suspension SOP exercised. 5. `--persist-strict` + encrypted creds demonstrated on ≥10 sessions. 6. Prometheus / alerting drill executed successfully. 7. All critical runbooks walked by at least two operators. 8. CCB size-increase request formally approved. 9. Independent safety review (internal or external) with written sign-off.
- Risk resume (halt_flag_ stops the engine but there's no resume channel).
- QuestDB remains soft-fail by default. `--persist-strict` fails startup or
  runtime writes with a nonzero result; shadow/live also latch terminal halt.
  Monte Carlo explicitly rejects strict mode until its summary writer supports
  the same contract.

**From AGENTS.md / ENGINE "Not yet implemented" + hard invariants (ongoing for all work)**: Respect every one (compile-time live-order gate absolute via `TT_TARGET` + `target_allows_live_orders()`; halt is terminal/write-once atomic; no auto-resume/no cooldown/no "helpful" retry on safety paths; hot-path discipline — zero nlohmann/json (CI-enforced), zero or pooled allocs, lock-free SPSC only (one producer/consumer per ring); reconciler refusal default (except documented spot-testnet carve-out); user-data WS source of truth; provider is the only extension point (IProvider + four safety hooks + transport/parser/executor; core has no `HAS_*` or venue specifics); small capital first + evidence-based gates (full artifacts + 9-row Go-Live with two signatures before any tier increase). "The engine already implements strong compile-time and runtime safety primitives that most retail or early-stage systems lack... This playbook exists so that future operators cannot say 'we forgot why we were careful.'"

**AI coding rules (ongoing; AGENTS.md + ENGINE + prereq)**: Default Sonnet 4.6 sufficient for new strategies, indicators, tests, CLI flags, docs, single-file refactors, provider-stack additions following patterns, work in `src/simulation/` + synthetic provider (MC). **Must switch to Opus 4.7 before editing** any of: `src/engine/engine.{h,cpp}` + `engine_config.h`; `*kill_switch*`, `*dead_mans_switch*`, `*reconciler*`, `*watchdog*`; `src/core/tt_target.h` + any `TT_TARGET`/`target_allows_live_orders` callsites; `src/threading/` (SPSC, spin, affinity); `src/risk/` + any `halt_flag_` code; hot-path (no nlohmann/json); Binance live safety glue (refusal gates, time sync, OCO/brackets, REST signing, DMS heartbeats). Every frozen-surface PR must run the check script, carry the CCB token, reference `todo.md` items, and update governance if needed. MC/synthetic generally Sonnet-safe but preserve deterministic seeding + isolation.

**Completed items** (high-level; full details moved to relevant per-file or noted):
- Initial creation of `reports/phase0/` skeleton and governance root files (`prod.md`, `prerequisites.md`, `todo.md`) – Doc Phase 0 core (this cycle).
- Phase 1 mechanical freeze markers + enforcement script (already landed).
- Governance + status synchronization for `monte-carlo` branch Monte Carlo work (README, todo.md, AGENTS.md, prod.md, reports/phase0, instructions.md, user-manual.md).
- Historical Adaptive Hybrid smoke work is not current evidence; the prototype is retired and excluded from the CLI and MC matrix pending the A-* rebuild.
- MC-02 Step A (per-trial win_rate / profit_factor distributions + enhanced reporter; tiny AnalyticsReport addition for exact `winning_trades` count; JSON + QuestDB campaign rows).
- Multi-agent consolidation of scattered todos/docs (this update): single root `todo.md`; docs/ purged of duplicate action lists (now pointers only); historical hardening guide archived or clearly marked; stale planned refs cleaned with explicit language.

**Last updated**: 2026-07-03 (split from governance/03-todo.md per TODOS-SPLIT-SPEC; verbatim extraction of Other bullets + full 9-row Go-Live + invariants + AI rules + completed high-level; see 00-OVERVIEW.md + prod.md).
