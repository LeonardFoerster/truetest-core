# Skill Proposal: saas-readiness-audit

**Proposed name**: `saas-readiness-audit`  
**Category**: SaaS Preparation / Architecture Review (Read-Only First)  
**Priority**: **Strategic**  
**Target scope**: Project (and later user-level reusable)  
**Status**: Planning document (2026-07-16)

---

## One-Sentence Description

A comprehensive, strictly read-only first architecture and process audit skill that produces a detailed gap analysis of the current truetest-core system against the requirements of a future multi-user SaaS product — with special emphasis on never relaxing the engine's core safety, hot-path, and governance invariants.

---

## Why This Skill Is Needed

The current system (as documented in root README, `docs/governance/01-prod.md`, etc.) is explicitly designed as:

- Private, personal, attended research + tiny-size retail tool
- Single operator at the terminal
- Localhost-only web UI (read-only)
- Compile-time `TT_TARGET` gating for live orders
- Heavy human-in-the-loop rituals (math captcha, DMS watching, signatures)

Turning "the whole thing" into a SaaS changes almost every assumption:

- Multiple concurrent users / tenants
- Asynchronous backtest + MC jobs
- Potentially unattended or lightly-attended shadow/live modes (extremely dangerous)
- Authentication, authorization, audit logs
- Resource isolation and accounting
- Strategy upload / execution sandboxing
- Data isolation (QuestDB, event logs, portfolios)
- Billing / quotas
- Public or semi-public API surface

A naive "just add auth to the web UI" approach would be catastrophic for the safety culture that has been built.

This skill forces a full, honest, read-only assessment before any implementation begins.

---

## Mandatory First Step: Read-Only Mode

Like `engine-decomposition`, this skill **must** start with a long read-only phase:

- Full architecture review using `docs/architecture/*.md`, `reference/02-user-manual.md`, `01-instructions.md`
- Deep reading of the web layer (`src/web/`, frontend, snapshot contracts)
- Analysis of all state that is currently process-global or single-user
- Review of live safety surface and how it interacts with "user context"
- Review of object pool / memory model in light of concurrent jobs
- Review of Phase 0/1 rituals and which parts are human-attended only

Only after producing a written gap report may any design or implementation discussion begin.

---

## Key Areas the Audit Must Cover

1. **Engine Isolation & Job Model**
   - Can multiple engine instances (or trials) safely run concurrently for different users?
   - What is the cost (memory, CPU) per backtest / MC job?
   - Reset / cleanup guarantees between jobs.

2. **Web / API Surface**
   - Current web is read-only and localhost. What would a real authenticated multi-user API look like?
   - Snapshot streaming, result retrieval, job submission, strategy management.
   - Never allowing order placement through the web path.

3. **Live Trading in SaaS Context**
   - Enormous additional risk. The audit must explicitly call out why "retail users running live through the SaaS" is fundamentally different from the current attended Phase 0 model.
   - Likely conclusion in early phases: live trading remains operator-only or is heavily restricted.

4. **Data & Persistence Isolation**
   - QuestDB multi-tenancy / schema separation
   - Event log storage and access control
   - Portfolio / analytics per user

5. **Safety & Governance Translation**
   - How do the current Phase 0/1 rituals, CCB, frozen surface, and token rules translate (or not) to a hosted service?
   - Audit logging requirements for compliance.

6. **Frontend Evolution**
   - Current React SPA is a thin consumer of snapshots.
   - Future needs: job history, strategy library, user settings, billing, etc.

7. **Threat Model**
   - Malicious or buggy user strategies
   - Resource exhaustion (fork bombs via MC, memory)
   - Data leakage between tenants

---

## Output of the Skill

The skill must produce (or update) a living document, e.g.:

- `docs/skills/saas-readiness-report-YYYY-MM-DD.md` (or a dedicated `docs/saas/` tree later)

The report must contain:
- Executive summary (can this even become a responsible SaaS?)
- Detailed gap table (Current | Required for SaaS | Risk if ignored | Difficulty)
- Recommended phasing (S0, S1, S2...) that keeps the engine pure
- Explicit list of things that should **never** be done

---

## Integration

- Works together with the `design` skill for the planning phase after the audit.
- Should invoke or reference `saas-architecture-planner`.
- Cross-reviews with `saftey` (even though it's read-only) because describing the surface has governance implications.
- Later `tenant-isolation-guard` and `engine-as-service-boundary` will be direct children of the findings from this audit.

---

## Success Criteria

- After running this skill, the team has a clear, written, shared understanding of the distance between today's system and a responsible SaaS.
- No one can claim "we'll just add multi-tenancy later" without having read the gaps.
- The audit report becomes the north star document for any SaaS-related work.

---

## Key References (the skill must internalize these)

- Root `README.md` ("Intended use" and "never be enterprise" paragraphs)
- `docs/governance/01-prod.md` (philosophy & invariants)
- `docs/reference/05-web-ui.md`
- `src/web/` + frontend
- All frozen surface files
- Object pool and threading code
- Monte Carlo simulation architecture (jobs will look similar to MC trials)

---

## Important Philosophical Stance

This skill must be **pessimistic by default**. It should be harder to convince the skill that SaaS is feasible than to convince a human.

It must repeatedly emphasize:

> The engine's safety and hot-path properties were designed for a single attended operator. Any SaaS architecture must treat the engine as a **black-box, isolated, resource-accounted compute worker** whose internal rules are never relaxed for convenience.

---

*This is one of the most important upcoming skills for the stated future direction.*
