# Skill Proposal: saas-architecture-planner

**Proposed name**: `saas-architecture-planner`  
**Category**: SaaS / System Design (uses `design` + `execute-plan`)  
**Priority**: **Strategic**  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)

---

## One-Sentence Description

A strategic planning skill that, after (or together with) a `saas-readiness-audit`, produces a phased, executable architecture migration plan for evolving truetest-core toward SaaS capabilities — while treating the core engine's live-safety surface, hot-path discipline, and zero-alloc rules as non-negotiable constraints that the SaaS layer must work around rather than modify.

---

## Relationship to Other Work

This skill is the **second step** after `saas-readiness-audit`:

1. `saas-readiness-audit` → produces honest gap report (read-only, pessimistic).
2. `saas-architecture-planner` → turns the gaps into a concrete, phased plan + PR DAG (using the existing `design` and `execute-plan` skills).
3. Later guardian skills (`tenant-isolation-guard`, `engine-as-service-boundary`, etc.) protect the boundaries defined in the plan.

---

## Core Principles the Planner Must Enforce

1. **Engine purity** — The C++ engine remains a high-performance, zero-alloc, safety-gated compute engine. It does **not** gain auth, tenancy, HTTP, billing, or user strategy parsing.
2. **Isolation first** — Every user job (backtest, MC, shadow) runs in an isolated engine process or very strongly isolated context.
3. **Read-only web path for orders** remains absolute (even more important in SaaS).
4. **Attended live trading** stays a human ritual for a very long time (or forever for non-operator users).
5. **Phased rollout** with explicit "S0 / S1 / S2 / ..." gates. No big-bang rewrite.
6. **Governance translation** — Current Phase 0/1 rituals must be mapped to equivalent (or stronger) controls in the SaaS world.

---

## Recommended High-Level Phasing (the planner should refine this)

**S0 — Foundation (Internal SaaS-like usage)**
- Authenticated local or private web UI for the owner only
- Job queue for backtests and MC runs (still single user)
- Better resource accounting and cancellation
- No change to engine core

**S1 — Multi-user Backtest / Research SaaS (first real tenants)**
- Proper user accounts + API keys
- Per-user job isolation + quotas
- Isolated QuestDB schemas or namespacing
- Strategy library (pre-approved + upload with sandbox)
- Job history and result storage
- Still **no live trading** for regular users

**S2 — Advanced Features**
- Shadow trading per user (still heavily restricted, with strong disclaimers)
- Collaborative / shared strategies (with audit)
- Advanced analytics export

**S3+ — Live Trading (if ever)**
- This phase requires its own complete new governance model, legal review, capital tiers per user, etc.
- Likely only for a tiny set of highly vetted users or not at all.

The planner must make it **very hard** to justify moving live trading into the SaaS.

---

## How the Skill Should Work

1. Read the latest `saas-readiness-audit` report.
2. Work with the user (or autonomously in plan mode) to refine goals and constraints.
3. Invoke the `design` skill with a very large, carefully written prompt that includes:
   - All engine invariants
   - Current architecture seams (snapshot_dashboard, IProvider, etc.)
   - Job model requirements
4. Produce a design document.
5. Turn the design into an `execute-plan` compatible DAG (worktree-friendly PR stack).
6. The plan must explicitly call out new guardian skills that need to be created (`tenant-isolation-guard`, etc.).
7. Every step in the plan must have a verification ritual that protects the engine.

---

## Output Artifacts

- Updated or new architecture document under `docs/architecture/` or `docs/saas/`
- Executable plan (suitable for `/execute-plan`)
- List of new skills that should be implemented before or during each phase
- Risk register

---

## Integration

- Heavy user of the bundled `design` and `execute-plan` skills.
- Must consult `saas-readiness-audit`.
- Should reference and extend the patterns from `engine-decomposition` (read-only first, subagents, rituals).
- Later skills (`engine-as-service-boundary`, `tenant-isolation-guard`) will be created to protect decisions made by this planner.

---

## Success Criteria

- There exists a written, reviewed, phased plan that the team actually follows instead of ad-hoc SaaS experiments.
- Every proposed change in the plan has an explicit "how do we protect the engine invariants here?" answer.
- The plan makes it obvious what must be built **outside** the engine (orchestrator, API gateway, job worker supervisor, billing, etc.).

---

## Key References

- `saas-readiness-audit.md` (this directory)
- `docs/governance/01-prod.md` (the philosophy section is gold)
- Current web layer (`src/web/`)
- `src/bin/` entry points (how different targets are built)
- Monte Carlo simulation code (closest thing today to "run a job for a user")
- Threading and worker model (for isolation ideas)

---

## Warning for Implementers of This Skill

This skill must be written in a way that makes overly ambitious plans **fail its own review**.

It should be easier to get the planner to say "this phase is too risky, split it further" than to get it to approve a plan.

---

*One of the most important strategic skills for the long-term vision of the project.*
