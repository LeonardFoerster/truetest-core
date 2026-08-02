# TrueTest Documentation

**Welcome to the documentation for the TrueTest (hft-engine) modular C++23 trading engine.**

This directory contains the full documentation set for the project. The documentation is intentionally split between a few **living governance documents** at the repository root and focused technical documents here.

## Primary Starting Points

| Document | Purpose | Audience |
|----------|---------|----------|
| **[reference/01-instructions.md](reference/01-instructions.md)** | **Master Consolidated Reference** — Build, run, CLI, providers, realism models, safety layers, QuestDB, threading, Phase 0 procedures, everything in one place. | Everyone (operators, developers, reviewers) |
| **[reference/02-user-manual.md](reference/02-user-manual.md)** | High-level technical overview and architecture | New users & operators |
| **[governance/01-prod.md](governance/01-prod.md)** | Production readiness playbook, capital-tier phases, Go-Live gates | Anyone increasing live size |
| [../AGENTS.md](../AGENTS.md) | AI/agent coding rules, live-safety freeze policy, hot-path invariants (root) | AI assistants + human reviewers |
| **[00-INDEX.md](00-INDEX.md)** | Master 01-N reading/processing order (recommended sequence) | All users (start here for structured navigation) |

---

## Documentation Structure (Governance-First Pragmatic Approach)

The full aspirational hierarchy is being populated gradually. `docs/archive/` holds historical plans and snapshots (e.g. old gaps, merge plans, not-yet-started feature specs like Edge 1 cointegration).

**Root governance files**:
- **`governance/01-prod.md`** — Central production contract, phases, Go-Live Gate, Phase 0 ritual + command template.
- **`governance/02-prerequisites.md`** — Mandatory pre-PR checklist for frozen safety surface edits.
- **`governance/03-todo.md`** (thin canonical high-level) + `docs/todos/` (numbered detailed e.g. 01-P0-phase0.md#P0-01) — Living task list (P0/P1/MC/...); every safety PR must reference relevant items (see docs/todos/00-OVERVIEW.md).
- **`../AGENTS.md`** — AI/agent coding rules, freeze mechanics, hot-path + layer invariants.
- **`reports/phase0/`** — Evidence + templates (operational, not pure docs).

**Docs/ (current realized files)**:
- `reference/01-instructions.md` — Master how-to / CLI / build reference (pointers to `governance/01-prod.md` for deep phase material; MC section; checklists point to gov). Includes MC usage.
- `reference/02-user-manual.md` — High-level architecture + operator overview. Includes "Stochastic Backtesting (Monte Carlo)" subsection.
- `reference/05-web-ui.md` — Opt-in browser cockpit + backtest review (`-DENABLE_WEB=ON`, `--web`).
- `reference/03-db.md`, `reference/04-flags.md`, `reference/06-adaptive-hybrid-strategy.md`, this `README.md`.
- `architecture/` — `01-target-architecture.md`, `02-model.md`, `03-realism.md`, and `04-performance.md` realized; deeper architecture files remain aspirational.
- `data.md` — Phased redesign plan for market-data ingress (`data_handler` → independent series store + multi-format `DataWrapper`).
- `engine.md` — Phased decomposition plan for `engine.{h,cpp}`.
- `operations/` — `01-futures-phase0-operator-sop.md`, `02-futures-testnet.md`, `03-bitget-demo.md`.
- `upcoming_platform/` — Multi-venue map (`README.md`, `bitget.md` shipped, `bitunix.md` Phase 0–1).
- `governance/` — `01-prod.md`, `02-prerequisites.md`, `03-todo.md`, `04-summary.md` (authoritative; content reduction + pointers applied in 2026-07).

**Note on "info files"**: `reference/06-adaptive-hybrid-strategy.md` (lower-priority strategy spec with demo caveats — see governance/03-todo.md A-*). `docs/archive/questdb-multi-week-hardening-guide.md` (historical).

Aspirational items (e.g. full architecture/ , operations/ SOPs) use "Planned for Doc Phase X – current details live in docs/governance/01-prod.md / reference/01-instructions.md".

**docs/ is now the central authoritative documentation home.** (See REORG plan; this pass is slim+pointers+links only.)

**Build system note (2026-07, hygiene 2026-08):** Source lists live in `cmake/Sources.cmake` (single obvious registration point for core + tests). `CMakePresets.json` covers venues (`linux-bitget`, `linux-bitunix`, `linux-venues`, `linux-providers-questdb`), sanitizers, web, benchmarks, and release-native, with matching **buildPresets** so `cmake --build --preset <name>` works. Preset binaries land in `out/build/<preset>/`; classic `cmake -B build ...` remains supported. See `reference/01-instructions.md` §5.

See `governance/03-todo.md` (D-01..D-06 + consolidation) + this README (nav) for the phased rollout, extraction strategy, and 2026 doc hygiene (pointers + minimal duplication). "Session plan file" references point to internal agent session plans (not committed).

### Production Governance & Phase 0 Evidence
See the governance: `docs/governance/01-prod.md`, `docs/governance/02-prerequisites.md`, `docs/governance/03-todo.md`. (Full details/tasks here; aspirational todos/ dirs use "Planned...").

---

## Historical & Archived Material

Located in [`docs/archive/`](archive/):

- `questdb-multi-week-hardening-guide.md` — Historical QuestDB hardening log (phases largely landed; current in `reference/03-db.md`).
- `MERGE_PLAN.md` — Monte Carlo merge execution record (historical).
- `production-readiness-gaps-2026-05.md` — May 2026 snapshot of gaps (current status in `docs/governance/03-todo.md` + `docs/governance/01-prod.md`).
- Edge 1 plans: `Edge1_Dynamic_Cointegration_*` + source PDF guideline (future implementation work; not yet started. When ready to implement, pull from here + PDF).

Older aspirational refs cleaned. Archive is for completed/historical plans. Active reference material is in root `AGENTS.md` + `docs/governance/` + `reference/` + `architecture/` + `operations/` + `upcoming_platform/` + `00-INDEX.md`. Active engine decomp plan: `docs/engine.md` (not under archive).

---

## How to Navigate

See also the master recommended sequence in [`00-INDEX.md`](00-INDEX.md) (01-N reading/processing order per Planner-Structure spec).

1. **I just want to build and run the engine** → Start with [reference/01-instructions.md](reference/01-instructions.md)
2. **I'm preparing for live trading or Phase 0** → Read [governance/01-prod.md](governance/01-prod.md) (current ritual + command template + full exit criteria + why each element) + `reports/phase0/` (evidence + templates) + [operations/01-futures-phase0-operator-sop.md](operations/01-futures-phase0-operator-sop.md) + [governance/03-todo.md](governance/03-todo.md) (P0-01..P0-04; 0/15 status). Use [operations/02-futures-testnet.md](operations/02-futures-testnet.md) for non-qualifying testnet rehearsal drills. Bitget demo: [operations/03-bitget-demo.md](operations/03-bitget-demo.md) (not Phase 0 qualifying).
3. **I'm reviewing a PR that touches safety** → Read [../AGENTS.md](../AGENTS.md) + [governance/02-prerequisites.md](governance/02-prerequisites.md) (mandatory checklist) + [governance/03-todo.md](governance/03-todo.md) (P1-* + frozen files + process; or docs/todos/02-P1-freeze.md#P1-02). See AGENTS.md + governance/01-prod.md (model rules + anti-patterns).
4. **I need the full technical picture** → [reference/02-user-manual.md](reference/02-user-manual.md) + [reference/01-instructions.md](reference/01-instructions.md) + [governance/01-prod.md](governance/01-prod.md) + [architecture/](architecture/).
5. **I want to understand the Monte Carlo / stochastic backtesting capability** → Start with the Monte Carlo section in [reference/01-instructions.md](reference/01-instructions.md), the "Stochastic Backtesting" subsection in [reference/02-user-manual.md](reference/02-user-manual.md), and the MC-* items in [governance/03-todo.md](governance/03-todo.md) (high-level) or docs/todos/03-MC-simulation.md. (MC is research/robustness tool; does not relax Phase 0/1 gates. Historical merge plan in `docs/archive/MERGE_PLAN.md`).
6. **I am an AI coding agent that needs a dense power + constraint summary** → Read [../AGENTS.md](../AGENTS.md) first, then governance/01-prod.md + governance/02-prerequisites.md + this nav + [00-INDEX.md](00-INDEX.md).
7. **I need multi-venue (Bitget / Bitunix) status** → [upcoming_platform/README.md](upcoming_platform/README.md) + venue guides; ops SOP for Bitget demo above.

---

**Last updated**: 2026-08-02 (agent SoT `AGENTS.md`; venue ops + upcoming_platform nav; CLAUDE.md references removed).

Cross-refs updated to avoid rot. Broken links or stale refs = doc bug. Root `AGENTS.md`, `docs/governance/`, `reports/phase0/`, `reference/01-instructions.md` + `reference/02-user-manual.md` + architecture/ops/upcoming_platform are the practical active set. Large future feature plans belong in archive/ until active implementation. "Planned for Doc Phase X – current details live in ..." language used for aspirational paths.
