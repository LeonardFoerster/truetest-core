# Skill Proposal: engine-as-service-boundary

**Proposed name**: `engine-as-service-boundary`  
**Category**: SaaS Guardian / Architectural Boundary Enforcer  
**Priority**: High (future)  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)

---

## One-Sentence Description

A guardian skill that protects the contract that the C++ engine is a **pure, isolated, high-performance compute worker** for a single logical run (backtest, MC trial, or attended live session). It prevents "SaaS convenience" features from leaking into the engine or weakening its invariants in the name of easier orchestration.

---

## Core Mandate

> The engine does not know about users, tenants, jobs, billing, HTTP, or multi-tenancy.
> All of that lives in the orchestrator / supervisor / API layer around the engine.

This skill is the active enforcement of that boundary.

---

## Why This Skill Is Needed

Historical pattern in many projects: the core engine starts clean and powerful. Then people add:
- "Just a little config for which user this run belongs to"
- "We need to pass the job ID through for logging"
- "Can we make the engine emit Prometheus metrics directly?"
- "Let's add a small HTTP health endpoint inside the engine process"

Each change seems harmless. Over time the engine becomes a distributed systems participant instead of a pure trading engine.

Given how much discipline has gone into keeping the engine zero-alloc, safety-gated, and simple in its threading model, this boundary must be actively guarded.

---

## What the Skill Must Detect and Block

- Passing user/tenant/job identity into the engine constructor or config in a way that affects hot paths or safety logic.
- Adding new dependencies to the engine for observability, control planes, or RPC.
- Making reset behavior or pool behavior conditional on "which user" is running.
- Adding direct network I/O, file system assumptions about multi-user layouts, or shared global state that the orchestrator should manage.
- Weakening `TT_TARGET`, halt semantics, or reconciler rules "because in the cloud we can restart easier".

---

## Correct Patterns the Skill Should Encourage

- The orchestrator starts a fresh (or cleanly reset) engine process / context for a job.
- All user/job metadata lives in the supervisor.
- Communication with the engine happens through the existing seams (command line flags, snapshot consumption, event log output, QuestDB with namespaced tags).
- The engine binary remains the same three targets (backtest / shadow / live) with the same compile-time gating.

---

## Workflow (when active)

1. Any proposed change that touches engine public surface, main entry points, or config is classified for boundary impact.
2. The skill performs a "boundary review" (similar to hot-path review).
3. If the change would make the engine "SaaS-aware", it must propose an alternative design that keeps the boundary clean.
4. The skill can require an explicit "BOUNDARY_APPROVED" marker or review for borderline cases.
5. Works with `tenant-isolation-guard` and `phase-ritual-enforcer`.

---

## Relationship to Existing Code

The current `src/bin/main.inc` + `TT_TARGET` system is already a good example of strong boundaries (live orders are compile-time impossible in most binaries).

The skill should treat this as the correct precedent and protect that spirit.

---

## Success Criteria

- Five years from now, a developer looking at the engine code still cannot tell whether it is being used by a single person or by a 1000-user SaaS.
- All SaaS-specific concerns live in clearly separate crates / services / supervisor processes.
- When in doubt, the default answer from the skill is "keep it out of the engine".

---

## When to Create the Skill

Ideally **before** the first serious SaaS orchestration code is written — as part of the output of `saas-architecture-planner`.

Even before that, the `saas-readiness-audit` and `saas-architecture-planner` skills should behave as if this guardian already exists.

---

## References

- `src/core/tt_target.h` (excellent existing boundary example)
- `src/bin/` entry points
- Engine public interface (`engine.h`)
- `saas-readiness-audit.md`
- `saas-architecture-planner.md`
- `tenant-isolation-guard.md`
- Current usage of environment variables and CLI flags for configuration (these are the allowed "boundary crossing" mechanisms)

---

*This skill is the long-term immune system for the engine's design integrity.*
