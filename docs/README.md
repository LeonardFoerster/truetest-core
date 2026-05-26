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
- `instructions.md` — Master how-to / CLI / build reference (pointers to `prod.md` for deep phase material). Now includes detailed Monte Carlo section (flags, object reuse, parallel caveats, synthetic provider, reporter).
- `user-manual.md` — High-level architecture + operator overview. Now includes "Stochastic Backtesting (Monte Carlo)" subsection.
- `production-readiness-gaps.md`, `AdaptiveHybridStrategy.md`, `Coiled_Spring...Guide.md`, this `README.md`.

**Planned slim sub-structure (Doc Phases 1–3)**:
- `docs/architecture/` (starting with `target-architecture.md`, `migration.md`, `MODEL.md`, `realism.md`)
- `docs/operations/` (starting with `futures-phase0-operator-sop.md` and `futures-testnet.md`)
- `docs/reference/` (deferred until real content is extracted)

See the approved documentation plan (in the session plan file or `todo.md` #D- items) for the exact phased rollout and extraction strategy. The goal is 80/20 value with minimal duplication.

### Production Governance & Phase 0 Evidence
See the root files listed above (`prod.md`, `prerequisites.md`, `todo.md`, `reports/phase0/`).

---

## Historical & Archived Material

Located in [`archive/`](archive/):

- **realism-wiring.md** — Original implementation plan (tasks completed)
- **grok.md** — Dual-portfolio shadow P&L implementation log
- **drift-claude-analysis.md** + **drift-grok-analysis.md** — Two detailed plans for a future Solana/Drift liquidation keeper bot (highly overlapping)

These are retained for audit and historical context but are no longer the active reference.

---

## How to Navigate

1. **I just want to build and run the engine** → Start with [instructions.md](instructions.md)
2. **I'm preparing for live trading or Phase 0** → Read [prod.md](../prod.md) (current ritual + command template) + the printable SOP (planned in `docs/operations/futures-phase0-operator-sop.md` — Doc Phase 1)
3. **I'm reviewing a PR that touches safety** → Read [CLAUDE.md](../CLAUDE.md) + [prerequisites.md](../prerequisites.md) (MODEL.md and full architecture/ docs are planned for Doc Phase 2)
4. **I need the full technical picture** → [user-manual.md](user-manual.md) + [instructions.md](instructions.md) + (target architecture / realism docs planned in `docs/architecture/`)
5. **I want to understand the Monte Carlo / stochastic backtesting capability** (landed on `monte-carlo` branch) → Start with the Monte Carlo section in [instructions.md](instructions.md), the new "Stochastic Backtesting" subsection in [user-manual.md](user-manual.md), and the MC-* items in root [todo.md](../todo.md). Governance context in root [README.md](../README.md) and [prod.md](../prod.md).

---

**Last updated**: 2026 (Governance + master reference sync for `monte-carlo` branch Monte Carlo simulation work; governance-first pragmatic structure from prior doc plan preserved). The `monte-carlo` branch introduced a full Monte Carlo engine (`MonteCarloController`, GBM generator, synthetic provider, object reuse, experimental parallelism). Governance files (README, prod.md, todo.md, CLAUDE.md) and high-level docs (instructions.md, user-manual.md) were updated in this pass. Full aspirational subdirs (`architecture/`, `operations/`, etc.) remain deferred.

All cross-references in the master `instructions.md` and scripts have been updated (or explicitly marked as "planned for Doc Phase X – current details in prod.md / instructions.md"). If you find a broken link, please open an issue or PR. See `prod.md`, `CLAUDE.md`, and `todo.md` for the current state of the documentation roadmap.