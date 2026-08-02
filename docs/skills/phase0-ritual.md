# Skill Proposal: phase0-ritual

**Proposed name**: `phase0-ritual`  
**Category**: Operational / Ritual Guardian  
**Priority**: **Critical** (unblocks Phase 0 progress)  
**Target scope**: Project (`.grok/skills/phase0-ritual/`)  
**Status**: Planning document (2026-07-16)

---

## One-Sentence Description

A dedicated guardian skill that orchestrates and enforces the complete Phase 0 tiny-size mainnet futures validation ritual from session creation through artifact collection, replay analysis, signed notes, PROGRESS updates, and batch review preparation — without ever relaxing safety or governance rules.

---

## Why This Skill Is Needed

Current state (see `docs/todos/01-P0-phase0.md`, `docs/governance/01-prod.md`, `reports/phase0/`):

- Phase 0 is at **0/15** qualifying sessions.
- The process is spread across multiple bash scripts:
  - `scripts/phase0/new-session.sh`
  - `scripts/phase0/post-session.sh`
  - `scripts/phase0/analyze-log.sh`
  - `scripts/phase0/volatility-classifier.sh`
  - `scripts/phase0/create-evidence-bundle.sh`
  - `scripts/phase0/dry-run-phase0.sh`
- Operator must manually:
  - Copy-paste complex command templates
  - Keep math-captcha visible
  - Run post-halt `grep` for specific strings
  - Execute replay through engine_backtest for objective metrics
  - Fill blanks in `session-note.md`
  - Update `PROGRESS.md`
  - Prepare batch reviews every 5 sessions
- High risk of human error (missing artifacts, wrong run-tag, incomplete notes, forgotten signatures).
- The ritual is the **single most important current operational activity**.

A skill can turn this from "follow many scripts + remember checklist" into "Grok guides me through the ritual with verification at each step".

---

## When to Use This Skill

- User says: "start a new phase 0 session", "run phase0 ritual", "post-process the live run", "prepare phase 0 evidence"
- Automatically when the user is working inside `reports/phase0/` or mentions `--live` + `binance-futures` in context of Phase 0.
- Before every qualifying mainnet futures session.

---

## Non-Negotiable Invariants (Never Violate)

1. **Never suggest or allow any change to the safety command template** without explicit reference to a governance update in `prod.md`.
2. The skill **must** always surface the current conservative caps from `docs/governance/01-prod.md`.
3. Must require/verify:
   - Math-captcha visible reminder
   - One-way mode confirmation
   - DMS + reconciler parameters
   - `--persist` + proper `--run-tag`
4. **Never** allow skipping the mandatory post-halt grep or replay step for qualifying sessions.
5. All generated session notes must contain the exact declaration checkboxes from the current SOP.
6. The skill may never weaken `LIVE_SAFETY_CCB_APPROVED` or frozen surface rules (even indirectly).
7. Must integrate with (or at minimum remind about) `scripts/check-live-safety-freeze.sh` if any related code/docs are touched.

---

## Detailed Workflow (Phased)

### Phase 0 — Session Preparation (Read-Only + Guidance)
1. Confirm clean git state and current Phase 0 status (read `reports/phase0/PROGRESS.md` + `docs/governance/01-prod.md`).
2. Ask for: symbol(s), regime (high/medium/low), optional notes.
3. Invoke (or simulate) `new-session.sh` logic and present the exact copy-paste command.
4. Print the pre-session SOP checklist header (from the script + printable SOP).
5. Remind operator of all mandatory live safety elements.
6. Create the target directory under `reports/phase0/` if it doesn't exist.

### Phase 1 — During / Immediately After Live Run
- Guide the operator through the mandatory post-halt `grep` command.
- Remind to keep the terminal scrollback.
- Suggest next command: `./scripts/phase0/post-session.sh <run-tag> ...`

### Phase 2 — Artifact Collection & Replay (Core Value)
1. Locate (or ask for) the event log.
2. Run (or instruct) post-session.sh.
3. Generate / enhance the `session-note.md` draft.
4. **Automatically** propose the replay command using `engine_backtest --replay` with good defaults (strategy, thread-preset, output paths).
5. After replay, help the operator extract objective numbers (equity, drawdown, fills, drift) and fill the note.
6. Validate that all required sections of the note are populated before allowing "done".

### Phase 3 — Governance Updates
- Generate the exact row for `PROGRESS.md`.
- Remind about volatility classification.
- Prepare batch review summary when 5-session threshold is reached.
- Cross-check that the session references the correct P0-* todo items.

### Phase 4 — Verification & Hygiene
- Run relevant gate scripts where applicable.
- Suggest commit message (following git-push skill conventions).
- Update any cross-references if the ritual itself changed.

---

## Integration with Existing Skills & Tools

- Uses / cooperates with:
  - `testing` + `check-work` (if any tooling around replay changes)
  - `saftey` (always surface frozen surface warnings)
  - `repo-doctor` (future)
- Directly drives the existing `scripts/phase0/*` tools rather than duplicating logic.
- Can call `spawn_subagent` for "analyze this session log for anomalies" tasks.
- Should be invocable both as a full ritual and for sub-steps (`/phase0-ritual post-session`).

---

## Success Criteria (for the eventual skill)

- An operator can complete a full qualifying session artifact package using primarily guidance from the skill.
- Zero missing mandatory artifacts in generated notes (enforced by the skill).
- Generated session notes pass human + (future) automated review without structural fixes.
- The skill refuses to "mark complete" if the post-halt grep was not performed or the note is incomplete.
- Clear before/after evidence that using the skill reduces operator cognitive load.

---

## References (Must Be Read When Implementing)

- `docs/governance/01-prod.md` (exact command template + why each flag + exit criteria)
- `docs/operations/01-futures-phase0-operator-sop.md` (printable SOP)
- `docs/todos/01-P0-phase0.md`
- `reports/phase0/PROGRESS.md` + `templates/phase0-session-note.md`
- `scripts/phase0/new-session.sh`, `post-session.sh`, `volatility-classifier.sh`
- `docs/governance/02-prerequisites.md`
- Existing `saftey` and `engine-decomposition` skills (style reference)

---

## SaaS Future Implications

When (if) this project evolves toward SaaS:

- This ritual skill will need a **counterpart** for "simulated / paper Phase 0 for new users".
- The core live ritual must remain extremely strict.
- A separate `phase0-saas-onboarding` skill may be needed later that teaches users the same discipline without giving them real mainnet keys immediately.

**Never** allow the SaaS layer to bypass or "helpfully" relax any of the Phase 0 evidence requirements for the core engine team.

---

**Implementation Notes**:
- Prefer project scope.
- After creation, the skill should be tested on a dry-run first (`dry-run-phase0.sh`).
- Consider adding a "simulate full ritual" mode for training.

*This document should be used as primary source when running `/create-skill`.*
