# TrueTest Documentation

**Welcome to the documentation for the TrueTest (hft-engine) modular C++23 trading engine.**

This directory contains the full documentation set for the project. The documentation is intentionally split between a few **living governance documents** at the repository root and focused technical documents here.

## Primary Starting Points

| Document | Purpose | Audience |
|----------|---------|----------|
| **[instructions.md](instructions.md)** | **Master Consolidated Reference** — Build, run, CLI, providers, realism models, safety layers, QuestDB, threading, Phase 0 procedures, everything in one place. | Everyone (operators, developers, reviewers) |
| **[user-manual.md](user-manual.md)** | High-level technical overview and architecture | New users & operators |
| [prod.md](../prod.md) | Production readiness playbook, capital-tier phases, Go-Live gates (root) | Anyone increasing live size |
| [CLAUDE.md](../CLAUDE.md) | AI coding rules, model selection, live-safety freeze policy (root) | AI assistants + human reviewers |

---

## Documentation Structure (Governance-First Pragmatic Approach)

The full aspirational hierarchy (`architecture/`, `operations/`, `reference/`, `archive/`, etc.) is described in the original plan but is being populated in phases to avoid rot and respect solo-maintainer reality.

**Root governance files (highest visibility – created in Doc Phase 0)**:
- **`prod.md`** — The central production contract, phases, Go-Live Gate (9 rows), Phase 0 command template + ritual, and philosophy.
- **`prerequisites.md`** — Living mandatory checklist before any edit to the Phase 1 frozen safety surface.
- **`todo.md`** — Current active items; every frozen-surface PR must reference relevant entries.
- **`reports/phase0/`** — Evidence tracker (`PROGRESS.md`), templates, batch reviews (not "docs" per se, but operational artifacts).

**Docs/ (current realized files)**:
- `instructions.md` — Master how-to / CLI / build reference (pointers to `prod.md` for deep phase material; MC section; checklists now point to root gov). Now includes detailed Monte Carlo section (flags, object reuse, parallel caveats, synthetic provider, reporter).
- `user-manual.md` — High-level architecture + operator overview. Now includes "Stochastic Backtesting (Monte Carlo)" subsection.
- `production-readiness-gaps.md`, `AdaptiveHybridStrategy.md`, `db.md`, `flags.md`, questdb guides, this `README.md`.

**Note on "info files"**: `AdaptiveHybridStrategy.md` (detailed spec for lower-priority strategy with demo caveats in code — see root `todo.md` A-*); `production-readiness-gaps.md` (May 2026 honest snapshot, still referenced; many items map to open R/S/H in todo). `questdb-multi-week-hardening-guide.md` moved to `docs/archive/` post-consolidation (historical; phases largely landed — see `db.md` for current).

**Planned slim sub-structure (Doc Phases 1–3; see root `todo.md` #D- items for current status)**:
- `docs/architecture/` (starting with `target-architecture.md`, `migration.md`, `MODEL.md`, `realism.md` — D-03)
- `docs/operations/` (starting with `futures-phase0-operator-sop.md` (P0-03 / D-02) and `futures-testnet.md`)
- `docs/reference/` (deferred)
- `docs/archive/` (for completed historical plans like the QuestDB hardening guide)

**Explicit planned language required** (per CLAUDE "Documentation Maintenance Rules"): When a cross-ref is aspirational, it **must** say "Planned for Doc Phase X – current details live in prod.md / instructions.md §N (or root todo.md for tasks)". Full aspirational hierarchy described in original plan but populated in phases to avoid rot.

See root `todo.md` (D-01..D-06 + this consolidation) + `docs/README.md` (nav) for the exact phased rollout, extraction strategy, and 2026 doc hygiene (purge of scattered action lists into single root todo.md; 80/20 value, minimal duplication). "Session plan file" references point to internal agent session plans (not committed).

### Production Governance & Phase 0 Evidence
See the root files listed above (`prod.md`, `prerequisites.md`, `todo.md`, `reports/phase0/`).

---

## Historical & Archived Material

Located in [`docs/archive/`](archive/) (or referenced from older notes in `docs/archive/`):

- **questdb-multi-week-hardening-guide.md** — Phased implementation log for QuestDB (v0.5; most phases 0-5 landed per code + `db.md` + scripts; moved here post-2026 consolidation as historical).
- Older notes reference (but files/dirs do not exist in tree): `realism-wiring.md`, `grok.md`, drift-*.md (Solana/Drift keeper plans), under root `archive/` or `docs/archive/`.

These (when present) are retained for audit/historical context but are no longer the active reference. See root `todo.md` (D-05) + `docs/README.md` for archive population tasks.

---

## How to Navigate

1. **I just want to build and run the engine** → Start with [instructions.md](instructions.md)
2. **I'm preparing for live trading or Phase 0** → Read [prod.md](../prod.md) (current ritual + command template + full exit criteria) + `reports/phase0/` (evidence + templates) + root [todo.md](../todo.md) (P0-01..P0-04; 0/15 status). Printable SOP planned in `docs/operations/futures-phase0-operator-sop.md` — Doc Phase 1 (current details in prod.md + reports/phase0/).
3. **I'm reviewing a PR that touches safety** → Read [CLAUDE.md](../CLAUDE.md) + [prerequisites.md](../prerequisites.md) (mandatory checklist) + root [todo.md](../todo.md) (P1-* + frozen files + process). MODEL.md / full architecture/ planned for Doc Phase 2 (D-03; current in CLAUDE + prod + instructions).
4. **I need the full technical picture** → [user-manual.md](user-manual.md) + [instructions.md](instructions.md) + root governance (`prod.md`, `CLAUDE.md`, `todo.md`). Target architecture / realism docs planned in `docs/architecture/` (D-03).
5. **I want to understand the Monte Carlo / stochastic backtesting capability** (landed on `monte-carlo` branch) → Start with the Monte Carlo section in [instructions.md](instructions.md), the "Stochastic Backtesting" subsection in [user-manual.md](user-manual.md), and the MC-* items in root [todo.md](../todo.md). Governance context in root [README.md](../README.md), [prod.md](../prod.md), and [ENGINE_AI_SUMMARY.md](../ENGINE_AI_SUMMARY.md). (MC is research/robustness tool; does not relax Phase 0/1 gates.)
6. **I am an AI coding agent that needs a dense power + constraint summary** → Read the root [ENGINE_AI_SUMMARY.md](../ENGINE_AI_SUMMARY.md) first (then CLAUDE.md + prod.md + this nav). It is the canonical "grasp the engine's powers" briefing.

---

**Last updated**: 2026 (multi-agent consolidation pass + post monte-carlo merge language sync: root `todo.md` is now the single authoritative task list; scattered action items / duplicate phase/todo lists purged from docs/ files and replaced with pointers per CLAUDE extraction + maintenance rules; `questdb-multi-week-hardening-guide.md` moved to `docs/archive/` as historical; stale Coiled_Spring + non-existent dir refs cleaned with explicit "Planned..." language + links to root `todo.md` D-*; "9 vs 10 files" + planned SOP notes noted for follow-up; MC + Phase 0/1 status + gov sync preserved). Monte Carlo capabilities were integrated from the monte-carlo branch (now mainline). Governance files (root prod/prereq/todo/CLAUDE/ENGINE + reports/phase0/) + high-level docs (instructions, user-manual) + db/flags are the active set. Full aspirational subdirs remain deferred (D-03 etc.; see root todo + docs/README planned section).

All cross-references in `instructions.md`, scripts, and docs/ have been (or are being) updated or explicitly marked as "Planned for Doc Phase X – current details live in prod.md / instructions.md §N (or root todo.md for tasks)". If you find a broken link or stale todo list, treat as doc bug per prod.md and fix / open issue with exact string. See root `prod.md`, `CLAUDE.md`, `prerequisites.md`, `todo.md`, and `ENGINE_AI_SUMMARY.md` for the current state of phases, tasks, rules, and roadmap. Root `todo.md` (this file after consolidation) is the living SoT for all current points.