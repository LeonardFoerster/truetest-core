# Code Review TODOs - July 16, 2026

**Source:** Strict maintainability code review executed via `/code-review` skill on 2026-07-16 against current changes + codebase health in `truetest-core`.

**Branch State at Review:** Working tree had small uncommitted changes (questdb store + engine async drain). Review took a broad ambitious view per skill rules.

**Review Focus (per `/code-review` SKILL.md):**
- Ambition for structural simplification ("code judo")
- No file growth past 1k lines without strong justification
- No spaghetti conditional growth
- Bias toward cleaning design over "it works"
- Prefer direct maintainable code
- Keep logic in canonical layers; reuse helpers
- High bar for approval on structural issues

**Issues Identified (5):**

1. **Engine God Class** (`src/engine/engine.cpp` — 4439 LOC) — primary structural blocker.
2. **QuestDB Persistence Leakage / Spaghetti Guards** — repeated `if (questdb_active_ && questdb_store_)` + `#ifdef` everywhere.
3. **Missed Code Judo on Sparse Rejection** — duplicated logic + workaround instead of simplification.
4. **Weak Execution Adapter Abstractions** — heavy `dynamic_cast` usage.
5. **Oversized UI Files** — `tabbed_dashboard.cpp` (1215) and `console_dashboard.cpp` (1097).

**Important Notes:**
- Issues 1 and 2 are systemic and **highly likely to require one or more dedicated new Grok skills** before any code changes. This follows patterns of existing project skills (e.g. `zero-alloc-perf-auditor`, `saftey`, `phase-ritual-enforcer`, `tt-target-guard`, `cpp23-hft-architect`).
- **Do not resolve any of these now.** These documents contain detailed, step-by-step guides for future Grok Build sessions.
- All work must respect:
  - LIVE-SAFETY SURFACE (see top of `engine.cpp` and `docs/governance/`)
  - Zero-alloc / hot-path discipline
  - Phase 0/1 gates and CCB rules
  - Existing object pool, SPSC, ring buffer, worker patterns

**How to Use These Documents:**
- Read the relevant per-issue `.md`.
- When ready to work on one, start a fresh session or use `/plan` + follow the numbered steps exactly.
- Cross-reference with `~/.grok/skills/` and `docs/todos/`.
- After changes, always run the verification ritual described.

**Directory Contents:**
- `00-OVERVIEW.md` (this file)
- `01-engine-god-class.md`
- `02-questdb-persistence-leakage.md`
- `03-sparse-rejection-refactor.md`
- `04-execution-adapter-abstractions.md`
- `05-ui-large-files.md`

**Next Actions (when executing):**
Use the individual guides. Start with exploration in read-only mode (as the original `/code-review` was).

---
*Generated as part of `/code-review` follow-up. All content is planning only.*
