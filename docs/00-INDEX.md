# TrueTest Documentation — Master 01-N Reading / Processing Order

**Status**: Aspirational index for the slimmed docs hierarchy (Doc Phases). Use as the canonical processing sequence for operators, reviewers, and implementers.

**Planned / extracted per reorg-spec**; current authoritative details remain in root governance (`CLAUDE.md`, `docs/governance/01-prod.md` etc.), `docs/reference/01-instructions.md`, `docs/reference/02-user-manual.md`, `reports/phase0/`, `docs/governance/03-todo.md` (thin) + `docs/todos/` (detailed per TODOS-SPLIT-SPEC). See `docs/README.md` + docs/todos/00-OVERVIEW.md for navigation.

**Last updated**: 2026-07 (new content impl: operations + architecture thin extracts + docs-impl-todos: docs/todos/ split + thin 03-todo.md)

## Recommended 01-N Order (read / process in this sequence)

01. Root `README.md` — High-level overview, binaries, phases table, safety surface, quick start.
02. `CLAUDE.md` — AI coding rules, model selection (Sonnet default vs Opus for safety), Phase 1 freeze mechanics, Documentation Maintenance Rules.
03. `docs/governance/01-prod.md` — Central contract: philosophy, invariants, Phase 0/1+ capital-tier gates, exact command template + "why each element", Go-Live Gate table (9 rows), ritual summary.
04. `docs/governance/02-prerequisites.md` — Mandatory pre-PR checklist for any frozen-surface edit (frozen file list incl. Bitget, token, scripts, shadow validation).
05. `docs/governance/03-todo.md` (thin high-level) + `docs/todos/` (00-OVERVIEW.md + 01-P0-phase0.md ... 09-...) — Living tasks (P0-*, P1-*, MC-*, R-*, S-*, D-*, A-*); reference items (e.g. docs/todos/01-P0-phase0.md#P0-01) in every safety PR. See 00-OVERVIEW.md .
06. `docs/governance/04-summary.md` — Condensed status + key excerpts.
07. `reports/phase0/README.md` + `PROGRESS.md` + `templates/phase0-session-note.md` — Phase 0 evidence requirements, tracker, session note template.
08. `docs/reference/01-instructions.md` — Master technical reference: CLI, providers, realism flags, MC usage, full checklists, operator rituals (pointers to prod for deep gates).
09. `docs/reference/02-user-manual.md` — High-level architecture, data flow diagram, core features (backtest/shadow/live/MC), operator overview.
10. `docs/architecture/01-target-architecture.md` — Thin high-level target architecture (extracted; see reference/ for current details).
11. `docs/architecture/02-model.md` — Model selection rules + explicit anti-patterns (extracted from CLAUDE + instructions).
12. `docs/architecture/03-realism.md` — Realism models (latency/impact/queue/fill/fee) details (extracted).
13. `docs/operations/01-futures-phase0-operator-sop.md` — Printable Phase 0 operator SOP (checklists, signatures).
14. `docs/operations/02-futures-testnet.md` — Futures testnet rehearsal guide for DMS/kill-switch/refusal drills (non-qualifying evidence).
14b. `docs/operations/03-bitget-demo.md` — Bitget UTA demo drill, geo precondition, DMS BD / account-wide caveats (not mainnet readiness).
15. `docs/architecture/04-performance.md` — Performance capacities, threading, limits.
16. `docs/reference/03-db.md`, `04-flags.md`, `05-web-ui.md`, `06-adaptive-hybrid-strategy.md` — Specialized reference (use as needed).
17. `docs/archive/` — Historical plans (MERGE_PLAN, production-readiness-gaps-2026-05, questdb guides, Edge1 plans). Do not use for current status.
18. `engine.md` — Detailed phased execution plan for `src/engine/engine.{h,cpp}` decomposition (see the `engine-decomposition` skill). Reference for god-class reduction work.

## Processing Notes
- **Operators preparing Phase 0**: 01 → 03 → 07 → 13 (SOP) → 08 (ritual details).
- **Safety PR reviewers**: 02 (CLAUDE) → 04 (prereqs) → 05 (todo refs + docs/todos/) → run freeze script.
- **Full technical picture**: 09 (user-manual) → 08 (instructions) → 10-12 (arch) → 14 (perf).
- **MC / research**: 08 (MC section) + 09 (stochastic subsection) + 05 (MC-* items in docs/todos/03-MC-simulation.md).
- Always cross-check "Last updated" and explicit "Planned for Doc Phase X" language. Broken links = doc bug. Root governance + reports/phase0/ are single source of truth for gates/tasks.

Cross-references use explicit planned language per CLAUDE rules. This index reflects the Planner-Structure extraction strategy (D- items in docs/governance/03-todo.md + docs/todos/06-D-documentation-structure.md). See TODOS-SPLIT-SPEC for split.

## New Files (this pass)
- `docs/00-INDEX.md` (this file)
- `docs/operations/01-futures-phase0-operator-sop.md`
- `docs/operations/02-futures-testnet.md`
- `docs/architecture/01-target-architecture.md`
- `docs/architecture/02-model.md`
- `docs/architecture/03-realism.md`

See `docs/README.md` updates for nav + structure.
