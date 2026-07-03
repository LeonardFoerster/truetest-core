# TrueTest Task List & Roadmap (todo.md) [THIN CANONICAL STUB]

**Status**: Living – high-level entry point and canonical summary of record. Full detailed items + precise anchors (e.g. **P0-01**) live in `docs/todos/`. See `docs/todos/00-OVERVIEW.md` for structure, numbering, "Closes docs/todos/01-P0-phase0.md#P0-01" examples, maintenance rules, and full verbatim extraction of all P0/P1/MC/R/S/D/A/H items from this file's prior content.

**Post-merge note (Phase 5)**: Monte Carlo integration complete. All "active on monte-carlo" / "paused on monte-carlo" / "current branch focus" language removed from live docs (except legitimate feature names like --monte-carlo, "Monte Carlo" in capability descriptions, and historical notes in this plan / archive/). Private retail scope ("Intended Use & Scope" + "never be enterprise") established across root docs. ENGINE_AI_SUMMARY.md refs consolidated. MC-01/MC-02 marked landed. Safety freeze untouched by MC work.

**Completed (Phase 8)**: Monte Carlo to master merge — all phases completed successfully. Master is the new baseline. Private retail character clearly stated. All technical safety docs remain intact. See `docs/archive/MERGE_PLAN.md` for the full historical execution record.  

**Last update**: 2026-07-03 (todos/ split applied per TODOS-SPLIT-SPEC; see docs/todos/00-OVERVIEW.md).  
**How to reference**: "Addresses todo.md #P0-03 (Phase 0 evidence scaffolding)" or "Closes #A-07" (backward compat preserved in thin file); for precision use "Closes docs/todos/01-P0-phase0.md#P0-01" or "Addresses docs/todos/02-P1-freeze.md#P1-02".

Items are grouped by theme (details in numbered docs/todos/ files). Completed items moved/stuck per-file or summarized here after phase declared done in `prod.md`.

**Current focus (post monte-carlo integration)**: Monte Carlo simulation (research & strategy-robustness tool; `--monte-carlo --mc-trials N`, object reuse, experimental parallel, any strategy + realism; integrated from the monte-carlo branch). Phase 0 tiny-size mainnet futures validation (0/15 qualifying; collection was paused during monte-carlo branch priority — gates/ritual unchanged). Phase 1 Live-Safety Freeze mechanically enforced. Tiny-size validation and research only. Not suitable for meaningful capital until Phase 0/1 exit criteria satisfied. Read `prod.md` + `CLAUDE.md` + `prerequisites.md` + this + `docs/todos/00-OVERVIEW.md` before any frozen-surface work.

**Read first**: summary.md (root) + CLAUDE.md (for AI rules), `prod.md` (central contract, phases, 9-row Go-Live Gate, Phase 0 template + ritual), `prerequisites.md` (mandatory pre-PR checklist for the 10 frozen files), `CLAUDE.md` (AI model selection, Phase 1 freeze + CCB/token rules, doc maintenance), `docs/todos/00-OVERVIEW.md`. (Historical merge context in `docs/archive/MERGE_PLAN.md`.)

**Full detailed task list by group**: See `docs/todos/00-OVERVIEW.md` + the numbered files below. This thin file holds only high-level status + philosophy + pointers (no duplication of full item text).

**Current high-level status**:
- Phase 0: 0/15 (details + verbatim items: docs/todos/01-P0-phase0.md)
- Phase 1: Enforced (details + frozen list + P1-*: docs/todos/02-P1-freeze.md)
- MC: Integrated; MC-01/MC-02 landed (details + standing: docs/todos/03-MC-simulation.md)
- See docs/todos/ for R-*/S-*/D-*/A-*/H-* + Other/Go-Live (04- through 09-).

(See also: prod.md, prerequisites.md, CLAUDE.md, docs/README.md, docs/governance/04-summary.md.)

---

## Phase 0 Immediate (Unblock tiny-size mainnet validation; active; 0/15)

Full details: see docs/todos/01-P0-phase0.md ; see 00-OVERVIEW.md . (Status: 0/15; standing + 4 items verbatim extracted.)

---

## Phase 1 / Live-Safety Freeze (Current enforcement active)

Full details: see docs/todos/02-P1-freeze.md ; see 00-OVERVIEW.md . (Includes P1-01..P1-05 + exact Frozen Files 10-list + validation evidence + checklist.)

---

## Monte Carlo Simulation (integrated)

Full details: see docs/todos/03-MC-simulation.md ; see 00-OVERVIEW.md . (MC-01..MC-06 + landed notes + standing invariants.)

---

## Risk Management (Highest remaining technical risk per gaps.md)

Full details: see docs/todos/04-R-risk-management.md ; see 00-OVERVIEW.md .

---

## DMS / Kill-Switch / Bracket Hardening

Full details: see docs/todos/05-S-dms-kill-brackets.md ; see 00-OVERVIEW.md .

---

## Documentation & Structure (this plan + D items)

Full details: see docs/todos/06-D-documentation-structure.md ; see 00-OVERVIEW.md . (D-01..D-06 + ongoing + aspirational; this split implements D-06.)

---

## Adaptive Hybrid Strategy (previous / lower-priority branch work)

Full details: see docs/todos/07-A-adaptive-hybrid.md ; see 00-OVERVIEW.md .

---

## Persistence, Observability & Hardening (later phases)

Full details: see docs/todos/08-H-persistence-observability.md ; see 00-OVERVIEW.md .

---

## Other / Nice-to-Have / Future Venues

Full details: see docs/todos/09-other-future-gates.md ; see 00-OVERVIEW.md . (Includes multi-symbol etc. + full 9 Go-Live rows + CLAUDE/ENGINE invariants + AI coding rules.)

---

## Completed / Superseded Items

(High-level summary; see per-file "Completed" subsections in docs/todos/ and 00-OVERVIEW.md for moved items with context.)

- Initial creation of `reports/phase0/` skeleton and governance root files (`prod.md`, `prerequisites.md`, `todo.md`) – Doc Phase 0 core (this cycle).
- Phase 1 mechanical freeze markers + enforcement script (already landed).
- Governance + status synchronization for `monte-carlo` branch Monte Carlo work (README, todo.md, CLAUDE.md, prod.md, reports/phase0, instructions.md, user-manual.md).
- Post-landing doc hygiene for new strategies (structure-continuation, adaptive-hybrid) + indicators (ema_regime, stochastic, swing_detector): updated CLI `--help`, instructions.md, flags.md, user-manual.md, CLAUDE.md. Initial MC smoke campaigns executed successfully against the new strategies (MC-05 partial).
- MC-02 Step A (per-trial win_rate / profit_factor distributions + enhanced reporter; tiny AnalyticsReport addition for exact `winning_trades` count; JSON + QuestDB campaign rows).
- Multi-agent consolidation of scattered todos/docs (this update): single root `todo.md`; docs/ purged of duplicate action lists (now pointers only); historical hardening guide archived or clearly marked; stale planned refs cleaned with explicit language.
- todos/ split (D-06 continuation): detailed items extracted verbatim to docs/todos/NN-*.md ; this file thinned to high-level stub + pointers (2026-07-03).

**Maintenance note**: When a phase exit is declared in `prod.md`, move or strike all items that were required for that exit (in the relevant docs/todos/ file), add any new follow-ups that the exit review surfaced, update "Last update" here + in the specific todos/*.md + prod + prereq, and reference the declaring PR. This thin file + full docs/todos/ are reviewed together with code changes that affect the safety surface. Update "Last update" with summary of changes (e.g. "todos/ split applied; see docs/todos/00-OVERVIEW.md").

**If you find a broken or stale cross-reference**, treat it as a documentation bug (per prod.md) and fix or open an issue with the exact string.

All operational detail, architecture decisions, current invariants, and the full development log live in `CLAUDE.md`, `docs/governance/01-prod.md`, `docs/governance/02-prerequisites.md`, `docs/governance/03-todo.md` (thin high-level) + `docs/todos/` (detailed), summary.md (root) + CLAUDE.md (for AI rules), `docs/reference/01-instructions.md`, `docs/reference/02-user-manual.md`, and the `docs/` tree (with root governance as SoT). Historical merge context: `docs/archive/MERGE_PLAN.md`. Consult `reports/phase0/` for Phase 0 evidence. Full task details authoritative in `docs/todos/`.















---




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






---





---








- Go-Live Gate rows (overarching; no capital tier increase permitted until all nine rows have two signatures + concrete evidence): 1. All prior phases met. 2. 60-day shadow report (published or internally audited). 3. Funding + tiered MMR exercised for ≥30 days. 4. DMS position-flattening logic tested (or very strong SOP + automation). 5. `--persist-strict` + encrypted creds demonstrated on ≥10 sessions. 6. Prometheus / alerting drill executed successfully. 7. All critical runbooks walked by at least two operators. 8. CCB size-increase request formally approved. 9. Independent safety review (internal or external) with written sign-off.






---




