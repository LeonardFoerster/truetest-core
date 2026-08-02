# Skill Proposal: repo-doctor

**Proposed name**: `repo-doctor`  
**Category**: Meta / Project Health Guardian  
**Priority**: High  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)

---

## One-Sentence Description

A meta-health skill that runs a comprehensive, project-specific "doctor" check: executes all gate scripts, reports Phase 0/1/MC status, measures god-class LOC, checks documentation hygiene, hot-path alloc status, QuestDB guard count, and produces a single actionable health report — replacing the need to remember and run 8+ different commands manually.

---

## Why This Skill Is Needed

Developers and reviewers currently must remember (or look up) a long list of commands and checks:

- `./scripts/check-live-safety-freeze.sh`
- `./scripts/check-hotpath-json.sh`
- `./scripts/check-layer-deps.sh`
- Full test suite + hotpath filters
- `ctest`
- Manual inspection of `reports/phase0/PROGRESS.md`
- `engine.cpp` line count
- QuestDB pattern grep
- Doc cross-ref health
- Memory / sanitizer runs

This is exactly the kind of repetitive, high-value, low-creativity work that a skill should own.

A good `repo-doctor` becomes the single entry point for "is the project in a healthy state right now?"

---

## When to Use

- At the start of almost every session (`/repo-doctor`)
- Before committing after significant work
- As part of `check-work` final verification
- When onboarding or handing off context
- As a pre-PR ritual

---

## Non-Negotiable Behavior

1. **Must actually execute** the real gate scripts (never just parse).
2. Must report **current** numbers (0/15 for Phase 0, current engine LOC, etc.).
3. Must be **fast to run** in the common case (provide `--quick` / `--full` modes).
4. Output must be **actionable** — not just "bad", but "run this next" or "this file is the problem".
5. Must never suggest bypassing a failing gate.
6. Must respect that some checks (long shadow runs, full sanitizers) are expensive and should be opt-in or summarized.

---

## Detailed Workflow

### Phase 0 — Quick Health (Default)
- Git status + branch + ahead/behind
- Run the three fast check-*.sh scripts and report PASS/FAIL + last output
- Current Phase 0 status (0/15 + last few entries)
- `engine.cpp` LOC (and whether it is close to the CMake guard)
- Quick build status (does `cmake --build` succeed with current flags?)
- Hot-path alloc test summary (last known good or run light version)

### Phase 1 — Extended Checks (on demand)
- Full test suite status (or focused + summary)
- QuestDB guard count in engine (for future `questdb-isolation` tracking)
- Documentation hygiene quick scan (stale references, missing last-updated)
- UI file sizes
- Strategy count + which ones have no MC coverage (heuristic)
- Web UI build status (if ENABLE_WEB relevant)

### Phase 2 — Report Generation
- Produce a clean, structured markdown or terminal report.
- Categorize: **Green / Yellow / Red**
- For every Red/Yellow item: give the exact command to investigate or fix.
- Optionally write a timestamped report to `check-ups/` or similar.

### Phase 3 — Ritual Integration
- The skill can be invoked by other skills (e.g. at the end of `testing` or before `check-work`).
- For frozen surface work, it should remind about the full Phase 1 ritual (shadow run etc.).

---

## Integration With Existing Skills

- `check-work` should strongly recommend or automatically run `repo-doctor` as part of verification.
- `testing`, `performance`, `quality`, `saftey` can feed their results into the doctor report.
- `doc-hygiene` can be called as a sub-step.
- Future skills (`phase0-ritual`, `questdb-isolation`) can register themselves so the doctor knows to check their specific health indicators.

---

## Success Criteria

- Running `/repo-doctor` at the beginning of a session gives the operator an accurate mental model of project state in < 30 seconds for the quick path.
- The report is so useful that people start pasting it into session notes and PR descriptions.
- It catches regressions early (e.g. "engine.cpp grew by 180 lines since last doctor run").
- New contributors can run one command and immediately see what the project cares about.

---

## References & Data Sources

- All `scripts/check-*.sh`
- `scripts/phase0/*`
- `reports/phase0/PROGRESS.md`
- CMakeLists.txt (engine LOC guard)
- `docs/governance/01-prod.md` (current phase definitions)
- `docs/todos/`
- `src/engine/engine.cpp` (LOC)
- `tests/` (for test counts)
- ` (retired one-time review docs) 00-OVERVIEW.md`

---

## SaaS Future

In a SaaS context this skill becomes even more valuable:

- It can evolve into a "service health" doctor that also checks:
  - Job queue depth
  - Tenant isolation status
  - Cost per job metrics
  - Web API contract health
- The core engine doctor part must remain the first section of the report (the engine invariants never become less important).

---

**Recommendation**: Implement this relatively early. It multiplies the value of all other guardian skills by making them easy to invoke as a set.
