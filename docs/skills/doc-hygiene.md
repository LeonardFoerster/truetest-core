# Skill Proposal: doc-hygiene

**Proposed name**: `doc-hygiene`  
**Category**: Governance + Documentation Guardian  
**Priority**: High  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)

---

## One-Sentence Description

A documentation and governance hygiene skill that enforces the project's strict rules for cross-references, "Planned for..." language, todo synchronization between thin root files and `docs/todos/`, date updates, and link health — preventing the documentation rot that has historically required large reorg passes.

---

## Why This Skill Is Needed

The project has invested significant effort in documentation structure (2026-07 docs overhaul):

- Thin `docs/governance/03-todo.md` + detailed `docs/todos/*.md`
- Explicit anchors (`#P0-01`)
- "Planned for Doc Phase X – current details live in ..." language
- Many cross-references across README, governance, reference, CLAUDE.md (gitignored), scripts, and code comments
- `docs/00-INDEX.md`, `docs/README.md`, `docs/todos/00-OVERVIEW.md`

Despite rules, rot happens. Every large change (MC merge, todos split, governance reorg) required manual cleanup.

A dedicated skill can:
- Scan for common rot patterns
- Guide fixes
- Act as a reviewer that refuses to let stale references through

---

## When to Use

- Before any PR that touches governance, todos, reference docs, or architecture.
- After large refactors or feature merges.
- When user says "fix the docs", "update todos", "check cross-refs".
- As part of the verification ritual in `check-work` or `saftey` when governance files are modified.
- Periodically as maintenance (`/doc-hygiene audit`).

---

## Non-Negotiable Rules the Skill Must Enforce

1. **Reference format**:
   - Fine-grained references should cite a file and item ID, e.g. `docs/todos/01-P0-phase0.md` P0-01.
   - Avoid fragment-style todo references unless the target defines an explicit anchor.
2. **Planned language**:
   - Aspirational content must use explicit planned phrasing.
   - No dangling "will be documented in X" without the planned language.
3. **Sync between thin + detailed**:
   - Changes to status in `docs/todos/` must be reflected in the thin `governance/03-todo.md` (high-level) and vice versa.
4. **Last updated** fields must be touched on meaningful changes.
5. **Broken links / anchors** are treated as documentation bugs (per `docs/README.md`).
6. **Governance files** (`prod.md`, `prerequisites.md`, root README, CLAUDE.md references) must stay consistent on frozen surface descriptions.
7. Phase exit declarations must update all related status locations.

---

## Detailed Workflow

### Phase 0 — Audit (Read-Only)
- Walk the documentation tree using the rules in `docs/todos/00-OVERVIEW.md` and `docs/README.md`.
- Collect all references to:
  - `todo.md`, `governance/03-todo.md`
  - `docs/todos/`
  - P0-*, P1-*, MC-*, R-*, S-*, D-*, etc. items
  - "Planned for"
- Detect:
  - References to non-existent anchors
  - Out-of-date status numbers (e.g. still says 0/15 when progress was made)
  - Missing "Last updated"
  - Inconsistent frozen file lists
- Produce a clear "Documentation Debt Report".

### Phase 1 — Guided Fixes
- For each class of issue, propose precise edits.
- When editing todo files, follow the maintenance rules exactly (edit the specific numbered file first).
- For cross-repo references (scripts, code comments), suggest the minimal accurate update.
- Use `search_replace` with high precision.

### Phase 2 — Verification
- Re-run the audit pass — zero new issues introduced.
- Manually spot-check a sample of important links.
- If governance or Phase status changed: invoke `saftey` + `check-work` with docs focus.
- Confirm that `00-INDEX.md` and `docs/README.md` navigation still makes sense.

### Phase 3 — Hygiene Automation Ideas (Future)
- The skill may propose (but not implement in first version) lightweight scripts or CMake custom targets that fail CI on obvious rot.
- Update `quality` skill to include doc hygiene as a review dimension.

---

## Integration

- Works closely with `check-work` (the check-work skill already mentions governance cross-refs).
- Should be called by `saftey` when governance files are touched.
- Can be chained after `engine-decomposition`, `phase0-ritual`, or any large change.
- Complements human reviewers who are required to check docs on safety PRs.

---

## Success Criteria

- A PR that touches governance or todos can pass the skill's audit with zero findings (or only intentional, documented exceptions).
- The number of "documentation bug" comments during PRs drops significantly.
- After a major merge, running the skill quickly surfaces exactly what needs syncing.
- All explicit rules from `docs/todos/00-OVERVIEW.md` "Maintenance Rules" section are programmatically encouraged or checked.

---

## Key References

- `docs/todos/00-OVERVIEW.md` (the bible for maintenance rules)
- `docs/README.md`
- `docs/00-INDEX.md`
- `docs/governance/01-prod.md`, `02-prerequisites.md`, `03-todo.md`, `04-summary.md`
- ` (retired one-time review docs) ` (various docs mentions)
- Root `README.md`
- `scripts/phase0/create-evidence-bundle.sh` (references todo.md)

---

## SaaS Implications

As the project grows toward SaaS:

- Documentation surface will explode (user docs, API docs, operator runbooks, compliance docs).
- The same hygiene rules will become even more important.
- This skill can later evolve to also cover:
  - OpenAPI / API contract consistency
  - User-facing docs vs internal governance separation
  - Versioned documentation for different "tiers" of the SaaS

Start enforcing the discipline now while the surface is still manageable.

---

*Primary source material for creating the actual skill.*
