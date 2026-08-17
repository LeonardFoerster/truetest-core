# D: Documentation & Structure (this plan + D items)

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. This file's creation + split is execution of D-06. High-level also in thin `docs/governance/03-todo.md`. See docs/README.md , AGENTS.md for rules. Use explicit planned language.

- **D-01** (Doc Phase 0 – complete): Establish `docs/governance/{01-prod,02-prerequisites,03-todo,04-summary}.md` and the `reports/phase0/` skeleton; maintain cross-references as the tree evolves.
- **D-02** (Doc Phase 1): Maintain `docs/operations/01-futures-phase0-operator-sop.md` (printable) and `docs/operations/02-futures-testnet.md`. (Also P0-03; use the SOP on real sessions. Current ritual + template live in `docs/governance/01-prod.md`, the SOP, and `reports/phase0/`.)
- **D-03** (Doc Phase 2 – substantially complete): Maintain the numbered architecture references (`01-target-architecture.md`, `02-model.md`, `03-realism.md`, `04-performance.md`). Removed or never-created companions such as `migration.md` must be labeled planned rather than linked as current files.
- **D-04**: Add "Documentation Maintenance Rules" + anti-rot process (checklist in `prerequisites.md`, phase-exit ritual, explicit "planned" language for aspirational links). (Core rules landed in `AGENTS.md` + prereq; enforce on all cross-refs.)
- **D-05** (Future): Create remaining operations guides, reference material, archive population, and any lightweight link-check tooling.
- **D-06 (consolidation – complete, maintenance ongoing)**: Keep `docs/governance/03-todo.md` as the thin high-level entry and `docs/todos/` as the detailed task source. Remove stale pre-migration paths, label unbuilt documents as planned, keep historical guides under `docs/archive/`, and keep both navigators synchronized.

  CMake modernization (2026-07) advanced build maintainability: lists moved to `cmake/Sources.cmake` (one place for core + tests), rich presets added, absolute paths reduced. This supports D-04 maintenance rules and makes adding files dramatically cheaper. Hygiene pass (2026-08-02): matching `buildPresets`, venue presets (`linux-bitget` / `linux-bitunix` / `linux-venues` / `linux-providers-questdb`) documented in main refs, preset path contract (`out/build/<preset>` vs ad-hoc `build/`) clarified; NATIVE_OPT wording aligned with “all three engines”. Low-memory hygiene (2026-08-16): build/test presets default to one job, launchers no longer request unbounded parallel builds, build-profile flags are target-scoped, and `linux-release-low-memory` disables LTO for portable Release validation.

**Ongoing (`AGENTS.md` documentation hygiene + prod + prereq + todo)**:
- Authoritative set: root `AGENTS.md`, `docs/governance/{01-prod,02-prerequisites,03-todo,04-summary}.md`, `docs/todos/`, `reports/phase0/`. Keep them up to date.
- Every PR touching the frozen safety surface (or the *description* of that surface in docs) must reference the relevant items in `todo.md` and run `./scripts/check-live-safety-freeze.sh`.
- On every phase exit declared in `prod.md`, also update `todo.md` (move/complete items), `prerequisites.md` if the checklist evolved, and the "Last updated" note in the affected docs.
- When a cross-reference is still aspirational, it **must** say so explicitly: "Planned for Doc Phase X – current details live in docs/governance/01-prod.md / docs/reference/01-instructions.md §N".
- Extraction rule: long-form phase/ritual/gate content lives in `prod.md` (or the dedicated SOP). `instructions.md` contains pointers + quick command templates, not duplicates.
- Anti-rot ritual: before increasing any capital tier, the exit review must include "docs verified + links resolve + `todo.md` updated".
- When new work lands in `src/simulation/` (Monte Carlo), the MC section in `docs/reference/01-instructions.md` and any governance mentions (README, `docs/governance/03-todo.md`, `docs/governance/01-prod.md`) must be updated in the same PR or immediate follow-up.
- If you find a broken or stale cross-reference, treat it as a documentation bug and either fix it or open an issue with the exact string that needs updating.

**Aspirational / missing files** (use explicit planned language):
- `docs/operations/01-futures-phase0-operator-sop.md` and `docs/operations/02-futures-testnet.md` exist; `demo-trading-workflow.md` remains future/planned.
- Architecture companions that do not exist, including `docs/architecture/migration.md`, `perf-baseline.md`, and `futures-order-lifecycle.md`.
- Root: `archive/`, `decisions/`, `upcoming/`, `PHASE0_COMPLETION_PLAN.md`, Coiled_Spring...Guide.md
- Keep lists in docs/README + instructions in sync with realized state.

**Last updated**: 2026-08-16 (serial build/test preset and launcher documentation synchronized).
