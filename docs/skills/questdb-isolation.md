# Skill Proposal: questdb-isolation

**Proposed name**: `questdb-isolation`  
**Category**: Refactor Guardian (like engine-decomposition)  
**Priority**: High  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)  
**Source**: ` (retired one-time review docs) 02-questdb-persistence-leakage.md` + engine-decomposition skill notes

---

## One-Sentence Description

A strict guardian skill whose sole purpose is to drive the complete isolation of QuestDB persistence behind a single clean seam (`IOrderAuditSink` / `audit_sink_`), eliminating all scattered raw `if (questdb_active_ && questdb_store_)` guards from engine.cpp and related call sites while preserving zero-alloc hot-path discipline and Phase 1 frozen surface rules.

---

## Why This Skill Is Needed

Current problems (from July 2026 code review):

- QuestDB write sites are guarded with repeated conditional checks throughout `engine.cpp`.
- This creates "spaghetti guards" and dual paths (sparse + rich rejection handling).
- The pattern violates the principle that logic should live in the canonical layer.
- Every new feature (funding, additional analytics, etc.) adds more guarded sites.
- The `engine-decomposition` skill already calls this out as a follow-up Phase after the initial god-class work.
- QuestDB is **observability only** — it must never affect hot-path behavior or safety decisions.

Without a dedicated skill, this cleanup will either be done incompletely or will risk introducing regressions in the frozen `engine.cpp`.

---

## When to Use

- User wants to work on QuestDB activation, persistence, or observability.
- Any refactor touching `questdb_*` members, `OrderAuditSink`, or audit paths in the engine.
- Before adding any new QuestDB write site.
- As a required cross-review when `engine-decomposition` work touches persistence.

---

## Non-Negotiable Invariants

1. **Single seam rule** (absolute):
   - All writes (submit, status, fill, rejection rich+sparse, cancel, amend, funding, events) MUST go **only** through `IOrderAuditSink` / `audit_sink_`.
   - Zero raw `if (questdb_active_ && questdb_store_)` (or equivalent) decision sites for recording inside engine.cpp after the work.
2. **No hot-path impact**:
   - The Noop implementation must remain cheap and unconditional from the engine's perspective.
   - No new allocations or virtual calls introduced on hot paths.
3. **Sparse handling** belongs inside the sink, never leaked to callers.
4. **Counters** for QuestDB activity must be owned by the sink (or a dedicated thin component), not scattered in engine.
5. **Activation semantics** (`#ifdef HAS_QUESTDB` + runtime flag) may only change at wiring time. The meaning for data capture must stay identical until the seam is complete.
6. **Frozen surface** — `engine.cpp` changes require the full Phase 1 ritual (`LIVE_SAFETY_CCB_APPROVED`, clean shadow run, cross-reviews).
7. **Contract preservation**: `publish_event`, object pool behavior, and public engine interface remain unchanged.

---

## Detailed Workflow

### Phase 0 — Read-Only Discovery (Mandatory)
- Start with clean `git status`.
- Read `src/engine/engine.cpp` (focus on all questdb sections + `drain_*` methods).
- Read `src/engine/order_audit_sink.*`
- Grep for all current guard patterns.
- Produce a **before map**: every call site + whether it is rich/sparse/funding/etc.
- Count current guard sites.
- Re-read ` (retired one-time review docs) 02-questdb-persistence-leakage.md`.
- Document hot vs cold paths for QuestDB writes.

### Phase 1 — Design (Use `design` Skill)
- Target architecture: engine calls only abstract sink methods.
- Sink implementations: `NoopOrderAuditSink`, `QuestDBOrderAuditSink`, possibly others.
- Decide ownership of counters and sparse logic.
- Produce before/after call-site count and LOC delta expectations (must be net reduction in engine.cpp).

### Phase 2 — Implementation Discipline
- Extract / strengthen the sink seam first (behind minimal interface).
- Migrate **cold** paths first.
- Only after the seam is solid and tested: migrate hot-path adjacent sites.
- Every migration wave must be followed by:
  - `./scripts/check-live-safety-freeze.sh`
  - `./scripts/check-layer-deps.sh`
  - Hot-path alloc tests
  - Relevant engine + integration tests
- Use `spawn_subagent` for each wave.

### Phase 3 — Verification Ritual (Extended)
In addition to normal ritual:
- Full grep for remaining raw questdb guards in engine (must be zero for data paths).
- Before/after guard count report.
- Prove that sparse rejection handling is now entirely inside the sink.
- Run QuestDB integration tests (`test_questdb_*`).
- Exercise with `--persist` + QuestDB enabled.
- Fresh subagent cross-review focused only on the QuestDB leakage issue.
- Re-run `/code-review` skill on the changed areas.

### Phase 4 — Documentation & Hygiene
- Update `docs/reference/03-db.md` and any architecture notes.
- Add regression guard comment or test that prevents new raw guards.
- Close the corresponding item in ` (retired one-time review docs) 02-questdb-persistence-leakage.md`.
- Reference `docs/todos/08-H-persistence-observability.md` (or relevant P1 items).

---

## Integration With Other Skills

- **Must** be used in conjunction with `engine-decomposition` when persistence is touched.
- Cross-invokes: `saftey`, `performance`, `testing`, `check-work`, `quality`.
- The skill itself should reference and extend rules from `engine-decomposition/SKILL.md`.
- Future `repo-doctor` should include a "QuestDB guard count" check.

---

## Success Criteria

- `engine.cpp` contains **zero** raw QuestDB activation guards for recording data after completion.
- All QuestDB writes flow through one documented seam.
- No regression in hot-path allocation matrix or benchmark numbers.
- Net reduction in complexity/LOC inside engine for persistence concerns.
- A fresh subagent can read the code and immediately understand "this is how you add a new observability write — you extend the sink".

---

## Key References

- ` (retired one-time review docs) 02-questdb-persistence-leakage.md` (primary)
- ` (retired one-time review docs) 01-engine-god-class.md`
- `~/.grok/skills/engine-decomposition/SKILL.md` (especially the QuestDB Isolation section)
- `src/engine/order_audit_sink.h` + `.cpp`
- `src/engine/engine.h` / `engine.cpp` (questdb_* members)
- `docs/todos/08-H-persistence-observability.md`
- `docs/reference/03-db.md`
- `scripts/check-live-safety-freeze.sh`

---

## SaaS Considerations

QuestDB (or a successor observability store) will become **per-tenant or namespaced** in a SaaS world.

This skill's seam is the correct place to later inject:
- Tenant ID
- Run isolation
- Quota / cost attribution

The skill should leave clear extension points in the `IOrderAuditSink` interface for future multi-tenancy without requiring changes to every call site again.

**Rule**: Any future multi-tenant work on persistence must go through an updated version of this skill (or a successor).

---

*This planning document must be the primary reference when the skill is created via `/create-skill`.*
