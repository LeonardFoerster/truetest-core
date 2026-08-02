# Skill Proposal: phase-ritual-enforcer

**Proposed name**: `phase-ritual-enforcer`  
**Category**: Meta Guardian / Process Enforcer  
**Priority**: Medium-High  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)  
**Note**: Referenced in engine-decomposition and code-review planning docs.

---

## One-Sentence Description

A meta-process skill that knows the complete verification ritual for different classes of change (normal feature, frozen surface, hot-path refactor, Phase 0 operational work, docs-only, etc.) and actively guides or enforces that the full sequence of skills, scripts, subagent reviews, and evidence collection has been performed before work is considered complete.

---

## Why This Skill Is Needed

The project has accumulated a very powerful but complex set of verification steps. Different types of work require different subsets:

- Frozen surface → token + shadow run + CCB + specific skills
- Hot path change → alloc matrix + performance + zero-alloc-auditor
- Engine god class → full engine-decomposition ritual + multiple fresh subagents + re-run of code-review
- Phase 0 session → specific operational checklist

Currently this knowledge lives in several long SKILL.md files and in human memory (CLAUDE.md, governance). A dedicated enforcer can make the "did we do everything?" question mechanical.

---

## When to Use

- At the end of any non-trivial session (`/phase-ritual-enforcer` or automatically at end of `check-work`).
- When the user says "I think I'm done", "is this ready to commit?", "verify the ritual".
- Before creating a PR that touches sensitive areas.

---

## Core Responsibilities

1. Classify the work done in the current session (based on files touched + git diff + conversation).
2. Look up the required ritual for that class.
3. Check (by running commands, reading state, asking subagents) whether each step was actually performed.
4. For missing steps: either run them (if safe) or produce a precise checklist of what is still required.
5. For frozen surface work: explicitly verify the token is present in the planned commit message and that a clean shadow run was recorded.
6. Never let the user declare victory until the ritual for the change class is satisfied.

---

## Change Classification Examples (the skill must maintain this table)

- **Class A — Frozen Surface** (engine.cpp, live_safety.h, risk/*, binance futures live files, tt_target, worker_watchdog): Full Phase 1 ritual.
- **Class B — Hot Path Alloc Sensitive**: zero-alloc audit + matrix tests + performance.
- **Class C — Large Structural** (engine god class, major extractions): engine-decomposition flow + design doc + multiple subagents.
- **Class D — Phase 0 Operational**: phase0-ritual steps + artifact completeness.
- **Class E — Normal feature / test / docs**: testing + check-work + quality.
- **Class F — Pure docs/governance**: doc-hygiene + cross-ref validation + saftey if surface described.

---

## Workflow

1. Analyze session (files changed, skills used so far, commands run).
2. Determine class(es).
3. Run or simulate the checklist for the class.
4. Produce a report:
   ```
   Ritual Status for Class A (Frozen Surface)
   [x] LIVE_SAFETY_CCB_APPROVED planned in commit
   [ ] 4h+ clean engine_shadow run recorded
   [x] saftey skill invoked
   [ ] Fresh subagent cross-review (not the one who did the work)
   ...
   ```
5. Offer to execute missing safe steps.
6. Block "ready" verdict until green.

---

## Integration

- This is the "orchestrator" that calls or checks the output of other skills.
- Should be the last thing `check-work` recommends or runs.
- Works together with `repo-doctor`.
- Can be extended when new guardian skills are added (they register their ritual requirements).

---

## Success Criteria

- It becomes rare for a PR to be opened that later requires "oh we also need to run X".
- When someone asks "are we done?", the answer is a clear ritual report rather than a long discussion.
- New contributors get the full discipline without having to read 6 different SKILL.md files first.

---

## References

- `~/.grok/skills/engine-decomposition/SKILL.md` (Phase 4 — Verification Ritual)
- `~/.grok/skills/check-work/SKILL.md`
- `~/.grok/skills/saftey/SKILL.md`
- `docs/governance/01-prod.md` + `02-prerequisites.md`
- `scripts/check-live-safety-freeze.sh`

---

## SaaS Implication

As more people (or future team members) work on the codebase, ritual drift becomes the biggest risk to the safety culture. This skill is one of the best defenses.

In SaaS mode there will be additional ritual classes (e.g. "changes to the job scheduler boundary", "multi-tenant QuestDB schema changes"). The enforcer must be the central place that knows all of them.

---

*This skill turns the project's hard-won process knowledge into an active, enforceable participant in every session.*
