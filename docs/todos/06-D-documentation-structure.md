# D: Documentation & Structure (this plan + D items)

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. This file's creation + split is execution of D-06. High-level also in thin `docs/governance/03-todo.md`. See docs/README.md , CLAUDE.md for rules. Use explicit planned language.

- **D-01** (Doc Phase 0 – core done): Create `prod.md`, `prerequisites.md`, `todo.md`, and `reports/phase0/` skeleton + update all cross-references + gov sync for MC branch (landed; see Completed).
- **D-02** (Doc Phase 1): Maintain `docs/operations/01-futures-phase0-operator-sop.md` (printable) and `docs/operations/02-futures-testnet.md`. (Also P0-03; use the SOP on real sessions. Current ritual + template live in `docs/governance/01-prod.md`, the SOP, and `reports/phase0/`.)
- **D-03** (Doc Phase 2): Create the core architecture files (`target-architecture.md`, `migration.md`, `MODEL.md`, `realism.md`) and begin extraction from `instructions.md`. (Aspirational hierarchy under `docs/architecture/` etc.; see docs/README.)
- **D-04**: Add "Documentation Maintenance Rules" + anti-rot process (checklist in `prerequisites.md`, phase-exit ritual, explicit "planned" language for aspirational links). (Core rules landed in CLAUDE + prereq; enforce on all cross-refs.)
- **D-05** (Future): Create remaining operations guides, reference material, archive population, and any lightweight link-check tooling.
- **D-06 (this consolidation)**: After multi-agent analysis (code structure + incompletes + full MD classification), extract all current points from scattered documents, purge the duplicate/outdated todo lists / phase details / action items from docs/ files (replace with thin pointers: "See root `todo.md` (P0-*/MC-*/R-* etc.) and `prod.md` for current tasks, phases, and gates. This file is the technical reference."), produce single authoritative `todo.md` in root. Clean stale "Doc Phase" / missing-dir refs (make explicit "Planned for Doc Phase X – current details live in prod.md / instructions.md §N" per CLAUDE rule). Move historical `questdb-multi-week-hardening-guide.md` to `docs/archive/` (or mark clearly). Resolve freeze-list count drift (historical 9/10; current mechanical SoT is **14 files** incl. Bitget — keep todos/prereq/AGENTS/script in sync). Enforce extraction rule (long-form in prod/SOP; pointers + quick templates in instructions). Sync "Last updated" + branch notes. (See plan for full details.)

  CMake modernization (2026-07) advanced build maintainability: lists moved to `cmake/Sources.cmake` (one place for core + tests), rich presets added, absolute paths reduced. This supports D-04 maintenance rules and makes adding files dramatically cheaper.

  **2026-07-30 cmake-update hygiene**: Confirmed lists complete (sole intentional orphan: `src/web/tools/dump_fixtures.cpp`). Added `linux-bitget` + `linux-providers-questdb` presets and matching `buildPresets`; documented `ENABLE_BITGET` + preset `out/build/<preset>` contract; aligned NATIVE_OPT comments with “all three engines when ON”. New core `.cpp` or unit test still requires **1–2 edits in `cmake/Sources.cmake` only**.

**Ongoing (CLAUDE "Documentation Maintenance Rules" + prod + prereq + todo)**:
- The three root governance files (`prod.md`, `prerequisites.md`, `todo.md`) + `reports/phase0/` + CLAUDE are the single source of truth. Keep them authoritative and up to date.
- Every PR touching the frozen safety surface (or the *description* of that surface in docs) must reference the relevant items in `todo.md` and run `./scripts/check-live-safety-freeze.sh`.
- On every phase exit declared in `prod.md`, also update `todo.md` (move/complete items), `prerequisites.md` if the checklist evolved, and the "Last updated" note in the affected docs.
- When a cross-reference is still aspirational, it **must** say so explicitly: "Planned for Doc Phase X – current details live in docs/governance/01-prod.md / docs/reference/01-instructions.md §N".
- Extraction rule: long-form phase/ritual/gate content lives in `prod.md` (or the dedicated SOP). `instructions.md` contains pointers + quick command templates, not duplicates.
- Anti-rot ritual: before increasing any capital tier, the exit review must include "docs verified + links resolve + `todo.md` updated".
- When new work lands in `src/simulation/` (Monte Carlo), the MC section in `docs/reference/01-instructions.md` and any governance mentions (README, `docs/governance/03-todo.md`, `docs/governance/01-prod.md`) must be updated in the same PR or immediate follow-up.
- If you find a broken or stale cross-reference, treat it as a documentation bug and either fix it or open an issue with the exact string that needs updating.

**Aspirational / missing dirs & files** (referenced in docs/README, instructions, CLAUDE, todo, reports, etc.; do not exist yet; use explicit planned language):
- `docs/operations/01-futures-phase0-operator-sop.md` and `docs/operations/02-futures-testnet.md` exist; `demo-trading-workflow.md` remains future/planned.
- `docs/architecture/` (target-architecture.md, migration.md, MODEL.md, realism.md, futures-order-lifecycle.md, ...)
- `docs/reference/`, `docs/archive/` (unless created for historical guides)
- Root: `archive/`, `decisions/`, `upcoming/`, `PHASE0_COMPLETION_PLAN.md`, Coiled_Spring...Guide.md
- Keep lists in docs/README + instructions in sync with realized state.

**Last updated**: 2026-07-30 (cmake-update light hygiene: Bitget presets + Sources.cmake registration still 1–2 places; see build note above).
