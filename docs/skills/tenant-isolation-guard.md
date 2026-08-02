# Skill Proposal: tenant-isolation-guard

**Proposed name**: `tenant-isolation-guard`  
**Category**: SaaS Guardian (Future)  
**Priority**: High (once SaaS work begins)  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)

---

## One-Sentence Description

A future guardian skill (analogous to `saftey`) whose job is to protect multi-tenant isolation boundaries. Any code change that could allow data, state, or resource leakage between users must be reviewed and approved by this skill using the same rigor currently applied to the live-safety surface.

---

## Why This Skill Is Needed

Once real multi-user functionality exists, tenant isolation becomes the new "frozen surface".

Violations would be:
- One user seeing another user's portfolios, fills, or event logs
- Resource usage by one tenant affecting others (noisy neighbor)
- User-supplied strategies being able to affect global state
- Cross-tenant pollution in QuestDB, caches, object pools, or ring buffers

This is as serious as (or more serious than) a live order safety bug in the current model.

The skill should be created **before** the first real multi-user code lands, not after.

---

## Scope (What It Guards)

- Any code that touches user identity, job ownership, or data namespacing
- QuestDB writer / schema selection
- File system paths for event logs and artifacts
- Shared caches or registries
- Object pool sharing across jobs
- Web session / API key handling
- Strategy execution context

---

## Non-Negotiable Rules (Draft)

1. **Default deny** between tenants.
2. Every job must have an explicit tenant / user context that flows through the entire call chain.
3. Shared mutable state (singletons, global registries, statics) must be proven tenant-safe or eliminated.
4. The engine process (or strong sandbox) should be the primary isolation mechanism — in-process sharing is suspect.
5. Audit logging of cross-tenant access attempts must be mandatory.
6. Changes in this area require the equivalent of a "TENANT_ISOLATION_CCB_APPROVED" token + fresh subagent reviews focused only on isolation.

---

## When It Should Become Active

- As soon as the first user/job context object is introduced.
- Any work coming out of `saas-architecture-planner` Phase S1.

Until then, this document serves as a specification that the architecture planner must respect.

---

## Relationship to Engine

The tenant isolation guard must **never** push tenant awareness deep into the engine hot path or safety surface.

Correct pattern:
- SaaS orchestrator / worker supervisor sets up an isolated engine instance (or very clean reset) for the tenant's job.
- The engine itself stays as "pure compute for one logical run".

Any design that tries to make a single engine process handle multiple tenants simultaneously should be viewed with extreme skepticism by this skill.

---

## Integration

- Will become a required cross-review skill alongside `saftey`, `performance`, etc. for relevant changes.
- Should be invocable by `phase-ritual-enforcer` once multi-tenancy work starts.
- Will likely need its own enforcement script (similar to `check-live-safety-freeze.sh`).

---

## Success Criteria

- When someone tries to "quickly share a cache for performance", the tenant-isolation-guard refuses unless isolation is proven.
- Tenant isolation bugs become as culturally unacceptable as weakening the halt_flag_ or adding JSON to hot paths.

---

## References (to be expanded when the skill is created)

- `saas-readiness-audit.md`
- `saas-architecture-planner.md`
- `engine-as-service-boundary.md`
- Current single-process assumptions in engine, workers, and QuestDB usage

---

*Create this skill proactively, before the first tenant-aware code is written.*
