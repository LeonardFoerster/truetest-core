# Skills — Planning Documents (proposed agent skills)

**Purpose**: This directory contains detailed proposals and design specifications for new Grok skills that would significantly improve development velocity, safety, and governance in the truetest-core project.

These documents serve as:
- Input for the `/create-skill` workflow
- Reference material for future implementation of project-specific (or user) skills
- Living plans that capture "why this skill" + concrete invariants + workflows

**Status**: These are planning artifacts unless their own status says otherwise. `ui-refactor.md` records an executed 2026-08-14 refactor and is retained as historical reference. None of these proposals has a project `SKILL.md` under `.grok/skills/`.

---

## Philosophy

The existing skills (engine-decomposition, saftey, performance, testing, check-work, etc.) follow a proven pattern:

- Start **read-only**
- Encode **hard project invariants** (hot-path, LIVE-SAFETY SURFACE, zero-alloc, TT_TARGET, object pools, governance)
- Mandate **design first** (use `design` skill)
- Use **subagents + fresh cross-review**
- Require **full verification rituals** (gate scripts + testing + check-work chain)
- Produce **net positive maintainability**

New skills should follow the same high bar.

---

## Prioritized Upcoming Skills

### High Priority (Immediate Development Friction)

| File | Skill Name (proposed) | Category | Priority | Main Pain Point |
|------|-----------------------|----------|----------|-----------------|
| `phase0-ritual.md` | `phase0-ritual` | Operational / Ritual | **Critical** | Manual, error-prone Phase 0 session workflow (0/15 qualifying) |
| `questdb-isolation.md` | `questdb-isolation` | Refactor Guardian | High | Scattered `if (questdb_active_ && ...)` guards (see (retired 2026-07-16 one-time review) 02) |
| `doc-hygiene.md` | `doc-hygiene` | Governance / Docs | High | Cross-ref rot, "Planned for..." language, todo sync between thin + detailed files |
| `repo-doctor.md` | `repo-doctor` | Meta / Health | High | Running all gate scripts + health checks manually is tedious |

### Medium-High Priority (Velocity & Missing Guardians)

| File | Skill Name (proposed) | Category | Priority | Notes |
|------|-----------------------|----------|----------|-------|
| `scaffold-strategy.md` | `scaffold-strategy` | Velocity | Medium-High | New strategies are repetitive (registry, tests, MC, docs) |
| `zero-alloc-perf-auditor.md` | `zero-alloc-perf-auditor` | Performance Guardian | High | Explicitly referenced in engine-decomposition + (retired 2026-07-16 one-time review) but missing as dedicated skill |
| `phase-ritual-enforcer.md` | `phase-ritual-enforcer` | Meta Guardian | Medium-High | Referenced in multiple places; general "run the full ritual" orchestrator |

### SaaS Future Preparation (Strategic)

| File | Skill Name (proposed) | Category | Priority | Rationale |
|------|-----------------------|----------|----------|-----------|
| `saas-readiness-audit.md` | `saas-readiness-audit` | SaaS / Architecture | **Strategic** | Current architecture is single-user, attended, localhost-only. SaaS requires fundamental rethinking. |
| `saas-architecture-planner.md` | `saas-architecture-planner` | SaaS / Design | **Strategic** | Must produce a safe, phased plan that never relaxes engine invariants. Works with `design` + `execute-plan`. |
| `tenant-isolation-guard.md` | `tenant-isolation-guard` | SaaS Guardian | High (future) | Will become critical once multi-user state appears. |
| `engine-as-service-boundary.md` | `engine-as-service-boundary` | SaaS Guardian | High (future) | Protects the "engine is a pure, isolated compute worker" contract. |

**Additional candidates** (lower priority or combinable):
- `execution-adapter-cleanup`
- `web-api-extend`
- `add-provider`
- `job-orchestration-design`
- `trading-saas-compliance`
- `resource-cost-model`

### Executed plans retained as reference

- `ui-refactor.md` — executed 2026-08-14; its proposal body is historical, not the current UI contract.

---

## How to Use These Documents

1. Pick a high-priority skill.
2. Read the full document.
3. Run `/create-skill` (or manually scaffold under `.grok/skills/<name>/` — preferably **project scope**).
4. Use the content here as the basis for the generated `SKILL.md` (inject invariants, workflows, references).
5. After creation, test the skill on a small safe task.
6. Update this overview + close the corresponding planning item when the skill lands.

---

## Scoping Recommendation

- **Project skills** (`<repo>/.grok/skills/`) for anything that encodes truetest-core invariants (most of the above).
- Keep general SaaS planning skills at user level only if they are reusable across projects.
- Version control the `skills/` planning docs (they live under `docs/`).

---

## Related Existing Artifacts

- Retired one-time review records — source of several refactor-skill ideas
- `engine-decomposition` skill — current guardian for the engine decomposition workflow
- `docs/governance/01-prod.md`, `02-prerequisites.md`, `03-todo.md`
- `docs/todos/00-OVERVIEW.md`
- `scripts/check-*.sh` and `scripts/phase0/*.sh`

**Last updated**: 2026-08-14 (`ui-refactor` moved to executed-reference status; project-skill status clarified)

---

*These documents are intentionally placed under `docs/` (tracked) while the eventual skill implementations may live in `.grok/skills/` (which can be project-local).*
