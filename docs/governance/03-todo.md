# TrueTest Task List & Roadmap (todo.md) [THIN CANONICAL STUB]

**Status**: Living – high-level entry point and canonical summary of record. Full detailed items + precise anchors (e.g. **P0-01**) live in `docs/todos/`. See `docs/todos/00-OVERVIEW.md` for structure, numbering, "Closes docs/todos/01-P0-phase0.md#P0-01" examples, maintenance rules, and full verbatim extraction of all P0/P1/MC/R/S/D/A/H items from this file's prior content.

**Post-merge note (Phase 5)**: Monte Carlo integration complete. All "active on monte-carlo" / "paused on monte-carlo" / "current branch focus" language removed from live docs (except legitimate feature names like --monte-carlo, "Monte Carlo" in capability descriptions, and historical notes in this plan / archive/). Private retail scope ("Intended Use & Scope" + "never be enterprise") established across root docs. ENGINE_AI_SUMMARY.md refs consolidated. MC-01/MC-02 marked landed. Safety freeze untouched by MC work.

**Completed (Phase 8)**: Monte Carlo to master merge — all phases completed successfully. Master is the new baseline. Private retail character clearly stated. All technical safety docs remain intact. See `docs/archive/MERGE_PLAN.md` for the full historical execution record.

**Last update**: 2026-07-06 (removed stale duplicated todo tail after todos/ split; this file is now a thin high-level stub only. Full details live in docs/todos/).
**How to reference**: "Addresses todo.md #P0-03 (Phase 0 evidence scaffolding)" or "Closes #A-07" (backward compat preserved in thin file); for precision use "Closes docs/todos/01-P0-phase0.md#P0-01" or "Addresses docs/todos/02-P1-freeze.md#P1-02".

Items are grouped by theme (details in numbered docs/todos/ files). Completed items moved/stuck per-file or summarized here after phase declared done in `prod.md`.

**Current focus (post monte-carlo integration)**: Monte Carlo simulation (research & strategy-robustness tool; `--monte-carlo --mc-trials N`, object reuse, experimental parallel, any strategy + realism; integrated from the monte-carlo branch). Phase 0 tiny-size mainnet futures validation (0/15 qualifying; collection was paused during monte-carlo branch priority — gates/ritual unchanged). Phase 1 Live-Safety Freeze mechanically enforced. Tiny-size validation and research only. Not suitable for meaningful capital until Phase 0/1 exit criteria satisfied. Read `01-prod.md` + root `AGENTS.md` + `02-prerequisites.md` + this + `docs/todos/00-OVERVIEW.md` before any frozen-surface work.

**Read first**: `docs/governance/04-summary.md` + root `AGENTS.md` (agent rules), `01-prod.md` (central contract, phases, 9-row Go-Live Gate, Phase 0 template + ritual), `02-prerequisites.md` (mandatory pre-PR checklist for the 10 frozen files), `docs/todos/00-OVERVIEW.md`. (Historical merge context in `docs/archive/MERGE_PLAN.md`.)

**Full detailed task list by group**: See `docs/todos/00-OVERVIEW.md` + the numbered files below. This thin file holds only high-level status + philosophy + pointers (no duplication of full item text).

**Current high-level status**:
- Phase 0: 0/15 (details + verbatim items: docs/todos/01-P0-phase0.md)
- Phase 1: Enforced (details + frozen list + P1-*: docs/todos/02-P1-freeze.md)
- MC: Integrated; MC-01/MC-02 landed (details + standing: docs/todos/03-MC-simulation.md)
- See docs/todos/ for R-*/S-*/D-*/A-*/H-* + Other/Go-Live (04- through 09-).

(See also: `01-prod.md`, `02-prerequisites.md`, root `AGENTS.md`, `docs/README.md`, `docs/governance/04-summary.md`.)

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

Full details: see docs/todos/09-other-future-gates.md ; see 00-OVERVIEW.md . (Includes multi-symbol etc. + full 9 Go-Live rows + AGENTS.md/ENGINE invariants + AI coding rules.)

---

## Completed / Superseded Items

(High-level summary; see per-file "Completed" subsections in docs/todos/ and 00-OVERVIEW.md for moved items with context.)

- Initial creation of `reports/phase0/` skeleton and governance root files (`prod.md`, `prerequisites.md`, `todo.md`) – Doc Phase 0 core (this cycle).
- Phase 1 mechanical freeze markers + enforcement script (already landed).
- Governance + status synchronization for `monte-carlo` branch Monte Carlo work (README, todo.md, AGENTS.md, prod.md, reports/phase0, instructions.md, user-manual.md).
- Post-landing doc hygiene for new strategies (structure-continuation, adaptive-hybrid) + indicators (ema_regime, stochastic, swing_detector): updated CLI `--help`, instructions.md, flags.md, user-manual.md, AGENTS.md. Initial MC smoke campaigns executed successfully against the new strategies (MC-05 partial).
- MC-02 Step A (per-trial win_rate / profit_factor distributions + enhanced reporter; tiny AnalyticsReport addition for exact `winning_trades` count; JSON + QuestDB campaign rows).
- Multi-agent consolidation of scattered todos/docs (this update): single root `todo.md`; docs/ purged of duplicate action lists (now pointers only); historical hardening guide archived or clearly marked; stale planned refs cleaned with explicit language.
- todos/ split (D-06 continuation): detailed items extracted verbatim to docs/todos/NN-*.md ; this file thinned to high-level stub + pointers (2026-07-03).

**Maintenance note**: When a phase exit is declared in `prod.md`, move or strike all items that were required for that exit (in the relevant docs/todos/ file), add any new follow-ups that the exit review surfaced, update "Last update" here + in the specific todos/*.md + prod + prereq, and reference the declaring PR. This thin file + full docs/todos/ are reviewed together with code changes that affect the safety surface. Update "Last update" with summary of changes (e.g. "todos/ split applied; see docs/todos/00-OVERVIEW.md").

**If you find a broken or stale cross-reference**, treat it as a documentation bug (per prod.md) and fix or open an issue with the exact string.

All operational detail, architecture decisions, current invariants, and the full development log live in `AGENTS.md`, `docs/governance/01-prod.md`, `docs/governance/02-prerequisites.md`, `docs/governance/03-todo.md` (thin high-level) + `docs/todos/` (detailed), summary.md (root) + AGENTS.md (for AI rules), `docs/reference/01-instructions.md`, `docs/reference/02-user-manual.md`, and the `docs/` tree (with root governance as SoT). Historical merge context: `docs/archive/MERGE_PLAN.md`. Consult `reports/phase0/` for Phase 0 evidence. Full task details authoritative in `docs/todos/`.
