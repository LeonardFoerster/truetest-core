# Monte Carlo → Master Merge Execution Plan

**Project**: truetest-core (TrueTest hft-engine)  
**Source branch**: `monte-carlo` (current, clean)  
**Target branch**: `master`  
**Context (user directive)**: This tool is **and will only ever be** a private retail / personal research tool. It is explicitly **not** and **will never be** an enterprise-ready or production-grade system for any external use or significant capital. All planning and execution must respect this scope.  
**Date of plan**: 2026 (post initial analysis)  
**Execution constraints**: All phases are strictly sequential. **Every previous phase must be marked complete with zero verification errors** (using the gates below) before any work on the next phase begins. Use `todo_write` to track status at the start and successful end of each phase. Prefer `search_replace` for edits. Use `grep`, `read_file`, and `list_dir` for all verifications. Git operations only in Phase 7+ and only after doc gates pass.

## Strict Gating Rules (Non-Negotiable)
1. Begin every phase by calling `todo_write` to set the relevant phase item(s) to `in_progress`.
2. Perform all edits and local changes for the phase.
3. Run the **exact Verification Gate** commands listed for the phase (using tools).
4. Only if **all** gate conditions return clean (0 errors, expected strings present/absent as specified), call `todo_write` to mark the phase `completed`.
5. If any gate fails: stop, fix using additional edits + re-verify, do **not** advance the todo status or start the next phase.
6. Never run destructive git (merge, push, branch -D) until Phase 7 and only after Phase 6 gate passes cleanly.
7. After any edit batch, always re-run the "no forbidden branch language" global greps as part of the gate.
8. Document any deviations in the phase's verification output.

This plan is designed to be executed step-by-step by Grok Build (or a subagent / `execute-plan` skill). It produces a clean mainline that accurately reflects the private retail reality.

## Canonical Private Retail Disclaimer Text (insert / update in key places)

Use this (or very close variant) as the primary "Scope" statement. Insert after the first paragraph(s) describing the engine in README.md, prod.md, and user-manual.md. Adjust slightly for context:

> **Intended Use & Scope**: TrueTest is a private, personal research and retail tool for the author only. It is not, and will never be, an enterprise-ready, institutional, or production trading system. Monte Carlo simulation, high-fidelity backtesting, and shadow divergence analysis are the primary mature capabilities. The live execution paths (`engine_live`) exist with unusually strong compile-time (`TT_TARGET`) and runtime safety layers (reconciler, DMS, kill-switch, venue risk checks, terminal halt, user-data source of truth, etc.). Any use of live paths is experimental, tiny-size, fully attended by the operator, and done at the author's own risk. The Phase 0/1 rituals and Go-Live language in this repository describe the author's personal evidence-gathering hygiene and self-imposed discipline — they are **not** a formal production release process or claim of readiness for others.

Keep all *technical* safety invariants, the 10-file freeze list, `check-live-safety-freeze.sh`, CLAUDE model rules, and hot-path discipline exactly as-is (they remain valuable personal guardrails).

---

## Phase 0: Preparation, Inventory & Baseline Audit (Read-Only + Setup)

**Goal**: Establish clean baseline, capture every string that must change, initialize tracking, confirm git state and private-use mandate. No edits in this phase.

**Actions**:
1. Call `todo_write` with all phases initialized (see recommended todos at end of this document).
2. Run comprehensive inventory greps (exact patterns below) and capture results.
3. Read the key governance files in full or large sections (README.md, CLAUDE.md, todo.md, prod.md, summary.md, prerequisites.md, docs/production-readiness-gaps.md, docs/README.md, docs/instructions.md).
4. Confirm working tree is clean and on `monte-carlo`.
5. Create or confirm this `MERGE_PLAN.md` exists at root (it was written as part of plan creation).
6. Note the non-existence of `ENGINE_AI_SUMMARY.md` (referenced but missing — will be cleaned in Phase 4).
7. List all files under `reports/phase0/`, `docs/`, `scripts/phase0/`.

**Exact Verification Commands (must all succeed with 0 errors)**:
- `grep -r --include="*.md" --include="*.txt" --include="*.sh" "Development is active on the \`monte-carlo\` branch" . | cat` → 0 matches
- `grep -r --include="*.md" "paused on monte-carlo branch" . | cat` → expect the known instances in docs + reports (will be fixed later)
- `grep -r --include="*.md" "collection paused on monte-carlo" . | cat`
- `git status` (via run_terminal_command, read-only) → "On branch monte-carlo" + "nothing to commit, working tree clean"
- `ls -1 *.md | cat` (root) → confirms MERGE_PLAN.md, README.md, CLAUDE.md, todo.md, prod.md, summary.md, prerequisites.md present
- `read_file` on MERGE_PLAN.md (this file) succeeds and shows the private-use context paragraph.

**Success Criteria**: All inventory captured, todo list created with Phase 0 marked completed only after the above gates pass with clean output. No file modifications in this phase.

**On success**: `todo_write` Phase 0 → completed. Proceed only then to Phase 1.

---

## Phase 1: Update Root Governance Documents

**Goal**: Remove all "active development on monte-carlo" framing from the most visible root files. Introduce private retail disclaimer. Update status language to past tense ("integrated from the monte-carlo branch").

**Files to modify** (in this order recommended):
1. `README.md` (top "Current Development Status" section + Authoritative Documentation table)
2. `CLAUDE.md` (minor branch/MC notes if any; MC layer safety note stays)
3. `todo.md` (header, "Current branch focus", P0 standing notes, MC section header)
4. `prod.md` (MC simulation paragraph + any status)
5. `summary.md` ("Current Status (2026, `monte-carlo` branch)" + MC bullets)
6. `prerequisites.md` (if it contains branch notes — usually minimal)

**Key changes (use unique `search_replace` strings)**:
- README.md: Replace the entire "## Current Development Status (2026)" block + the Phase 0 "collection paused..." sentence. Insert the Canonical Disclaimer right after the project description paragraph. Change "Development is active on the `monte-carlo` branch." to "Monte Carlo simulation capabilities were integrated from the `monte-carlo` feature branch (now part of mainline)."
- Update the table entry for `ENGINE_AI_SUMMARY.md` (see Phase 4 for full cleanup).
- todo.md: Change section "## Monte Carlo Simulation (current active branch)" → "## Monte Carlo Simulation (integrated)". Update "Current branch focus (monte-carlo):" paragraph to neutral language. Update all "paused on monte-carlo branch" standing notes to "Phase 0 collection was paused during monte-carlo branch priority (gates/ritual unchanged; MC work never touched the live safety surface)."
- prod.md: Update the Monte Carlo paragraph (lines ~100-106) to remove "The `monte-carlo` branch introduced..." in favor of "Monte Carlo simulation (introduced on the monte-carlo branch and now mainline) is a research...". Keep the research-tool disclaimer.
- summary.md: Similar past-tense update to "Current Status" section.
- Insert Canonical Disclaimer in README, prod.md (near top after Philosophy), and summary.md.

**Verification Gate (run after all edits in this phase, repeat greps until clean)**:
- `grep -r --include="*.md" "Development is active on the \`monte-carlo\` branch" .` → exactly 0 lines
- `grep -r --include="*.md" "paused on monte-carlo branch" README.md todo.md prod.md summary.md` → 0 (these files must be clean; other files allowed until later phases)
- `grep -r --include="*.md" "Current branch focus (monte-carlo)" .` → 0
- `read_file README.md | head -60` (or use open_page_with_find) confirms new disclaimer present and status section is past-tense.
- `grep -F "Intended Use & Scope" README.md prod.md summary.md | cat` → at least the three files show the text (or close variant).
- `grep -r --include="*.md" "ENGINE_AI_SUMMARY.md" README.md | cat` → still present (will fix in Phase 4).

**Success Criteria**: All root files updated, above greps pass with expected 0 counts on the active-branch phrases in root *.md files, disclaimer present, todo status updated only after gate.

**On success**: Mark Phase 1 completed in todo. Do not start Phase 2 until this gate is 100% clean.

---

## Phase 2: Update docs/ Tree and reports/phase0/

**Goal**: Propagate the same language cleanup + private-use tone into all secondary docs and the Phase 0 evidence trackers. Keep historical accuracy where appropriate (e.g. "landed on the monte-carlo branch" becomes "landed via the monte-carlo branch (now mainline)").

**Files**:
- `docs/README.md` (Last updated paragraph + MC navigation item #5 + AI agent item #6 + ENGINE_AI refs)
- `docs/instructions.md` (multiple "paused on monte-carlo branch", "Current P0 items & status", "Current branch note" around line 501, Phase 0 cross-refs)
- `docs/user-manual.md` (MC status line that references ENGINE_AI_SUMMARY + any top-level scope)
- `docs/AdaptiveHybridStrategy.md` (status note header)
- `docs/production-readiness-gaps.md` (the table row "Master branch frozen pending refactor" — reframe or remove the implication)
- `reports/phase0/PROGRESS.md`
- `reports/phase0/README.md`
- `docs/flags.md`, `docs/questdb-45-minute-soak-test-guide.md` (the git checkout example — mark as historical only; leave functional example if useful)

**Key actions**:
- Replace every instance of "paused on monte-carlo branch" and "collection paused on monte-carlo" with neutral phrasing: "Phase 0 collection was paused during priority work on the monte-carlo branch (MC changes did not alter P0 gates or the live safety surface)".
- In instructions.md "Current branch note" for MC: change to "Introduced on the `monte-carlo` branch. Now available in mainline in all three binaries..."
- In gaps.md table: Change the "Master branch frozen..." row to reflect that master now includes the MC work; the freeze process remains for safety-surface changes regardless of branch.
- Insert or reference the Canonical Disclaimer in user-manual.md (near Stochastic Backtesting section) and docs/README.md navigation intro if it fits.
- Update "Last updated" paragraphs to mention "post monte-carlo merge language sync".

**Verification Gate**:
- `grep -r --include="*.md" "paused on monte-carlo branch" docs/ reports/ | cat` → 0 matches
- `grep -r --include="*.md" "collection paused on monte-carlo" docs/ reports/ | cat` → 0
- `grep -r --include="*.md" "The \`monte-carlo\` branch focus remains" docs/ | cat` → 0
- `grep -F "Master branch frozen pending refactor" docs/production-readiness-gaps.md | cat` → 0 (row must have been reworded)
- Spot `read_file` on instructions.md sections  (around the P0 status and MC branch note) + docs/README.md last-updated paragraph.
- Global: `grep -r --include="*.md" "Development is active on the \`monte-carlo\` branch" .` still 0 everywhere.

**Success Criteria**: docs/ and reports/ cleaned of active-branch language, gates pass, Phase 2 marked complete only after full verification.

---

## Phase 3: Private Retail Scope Reframing (Tone & Disclaimer Deep Pass)

**Goal**: Ensure the private-only reality is not just added but that contradictory "institutional / production rollout" framing is qualified everywhere it appears. Do **not** delete the safety mechanisms or Phase 0 ritual details — only reframe their purpose.

**Focus areas** (beyond what Phases 1-2 already touched):
- `prod.md`: Philosophy section, Capital-Tier Phases intro, "No capital tier increase is permitted..." language, Go-Live Gate table header, "How to Use This Document". Add "Personal Discipline" framing.
- `docs/production-readiness-gaps.md`: Executive summary, Verdict table, Recommendation section, Prioritized Roadmap. Change "production-ready" verdicts to "ready for the author's private attended use".
- `README.md` and `summary.md`: "Strong institutional-grade safety primitives" → "Strong safety primitives (valuable even for careful private use)".
- `docs/instructions.md`: Philosophy & invariants section, any "production" wording.
- Any remaining "Go-Live" or "exit criteria for Phase 0 → Phase 1" sentences that sound like external product milestones.

**Actions**:
- Insert Canonical Disclaimer in prod.md (after Philosophy & Invariants or at top of Capital-Tier section).
- In gaps.md Verdict: Update the "Is it currently safe..." answers to be explicit about private retail context.
- Add a short "For Private Retail Use Only" callout near the Go-Live Gate table.
- Change language like "institutional-grade" to "strong personal-use safety scaffolding".
- Keep every technical requirement (15 sessions, 8h shadow, token, etc.) but prefix with "When the author chooses to collect evidence toward personal live use...".

**Verification Gate**:
- `grep -F "Intended Use & Scope" README.md prod.md docs/production-readiness-gaps.md docs/user-manual.md | cat` → at least 4 occurrences (or the files that make sense).
- `grep -r --include="*.md" "institutional-grade" . | cat` → 0 (or only in historical quotes that are now caveated).
- `grep -F "never be" prod.md README.md docs/production-readiness-gaps.md | cat` (expect the "never be an enterprise" language).
- `read_file prod.md | head -30` and spot-check the Go-Live section shows private framing.
- Global branch-language greps from previous phases still return 0 for active/paused phrasing.

**Success Criteria**: Reframing is consistent and prominent without weakening actual safety code/docs. Gate passes cleanly.

---

## Phase 4: MC Task Hygiene, ENGINE_AI_SUMMARY Cleanup & Minor Polish

**Goal**: Clean the todo.md MC section, mark what is landed, demote future items, fix dangling references to the non-existent `ENGINE_AI_SUMMARY.md`, update "Last update" dates/status, clean code/doc comments that say "landed on the monte-carlo branch".

**Specific tasks**:
- todo.md:
  - Header "Last update" → add "Monte Carlo integration + merge prep (post-monte-carlo branch)".
  - Change MC section header and the first few MC- bullets status.
  - Mark MC-01 and MC-02 as substantially landed (move or strike if following the "completed after phase declared" rule; otherwise add note "Landed with monte-carlo merge").
  - Update MC-04 (documentation) note.
  - Standing invariants paragraph: update any "on this branch" language.
- All files: Replace references to `ENGINE_AI_SUMMARY.md` (4 locations found) with "summary.md (root) + CLAUDE.md (for AI rules) + this MERGE_PLAN.md context".
  - docs/README.md (two places)
  - docs/user-manual.md (one place)
  - README.md table (one place)
- Source comments (non-blocking but good):
  - `src/providers/synthetic/synthetic_provider.h`
  - `src/indicator/swing_detector.h`, `ema_regime.h`
  - `CMakeLists.txt` Monte Carlo comment
  - `docs/instructions.md` MC branch note
  - `AdaptiveHybridStrategy.md` already handled in Phase 2.
- questdb guide: the `git checkout monte-carlo` example — add "(historical example from before merge)" comment.
- Any "TODO: integrate with --output" in main.inc for MC-06: leave as-is (it's a future item).

**Verification Gate**:
- `grep -r --include="*.md" "ENGINE_AI_SUMMARY.md" . | cat` → 0 (all replaced)
- `grep -r --include="*.md" "current active branch" todo.md | cat` → 0
- `grep -F "Landed with monte-carlo merge" todo.md | cat` → at least one (or clear status update present)
- `grep -r "landed on the \`monte-carlo\` branch" src/ docs/ | cat` → 0 or only historical notes clearly marked.
- `read_file todo.md | head -15` shows updated Last update and MC section.
- Spot check one indicator header and the synthetic provider header via read_file.

**Success Criteria**: MC tasks reflect reality, dangling AI summary refs gone, minor polish done. Gates clean.

---

## Phase 5: Final Cross-Project Consistency Audit & Pre-Merge Doc Lock

**Goal**: One last global pass to catch anything missed. Ensure no "monte-carlo" active language remains except:
- Legitimate feature names (`--monte-carlo`, preset "mc", "Monte Carlo" in prose describing the capability)
- Historical notes in archive/ or clearly marked "before the merge"
- Code identifiers and CLI help (leave untouched)
- This MERGE_PLAN.md itself (when describing the old state)

**Actions**:
- Full-project case-insensitive grep for the dangerous patterns.
- Targeted fixes for any survivors (scripts/pre-commit? unlikely; other .md; CMake comments).
- Update any remaining "Last updated 2026" or status paragraphs that still reference the old branch focus.
- Optionally add a short "Post-merge note" at the top of todo.md or in a new root note.
- Re-read the Canonical Disclaimer in the four main places to confirm it survived.
- Run the safety freeze script in check mode if desired (it should pass because MC work touched almost none of the 10 frozen files — verification only, no commit yet).

**Verification Gate (the strictest)**:
- `grep -r -i --include="*.md" --include="*.sh" --include="*.cmake" --include="*.txt" "active on the.*monte" . | cat` → 0
- `grep -r -i --include="*.md" "paused on monte-carlo" . | cat` → 0
- `grep -r -i --include="*.md" "collection paused" . | cat` → 0 (outside of this plan file and any archive)
- `grep -r --include="*.md" "Development is active" . | cat` → 0
- `grep -F "Intended Use & Scope" README.md prod.md docs/production-readiness-gaps.md docs/user-manual.md | wc -l` → >=3 (or exact count you decide)
- `grep -E "monte-carlo|Monte Carlo" --include="*.md" . | grep -v "feature\|capability\|--monte-carlo\|synthetic\|research tool" | cat` → only acceptable remaining uses (review manually).
- `read_file MERGE_PLAN.md | grep -c "Phase 5"` confirms this phase section exists.

**Success Criteria**: Global audit is clean except for intended uses of the term "Monte Carlo" as a feature. Phase 5 marked complete only when this gate is perfect.

---

## Phase 6: Pre-Merge Verification & Safety Confirmation (No Git Changes)

**Goal**: Confirm the tree is in a merge-ready state from the documentation and scope perspective. Simulate what the merge will look like. Do not touch git branches or working tree state yet.

**Actions**:
1. Re-run the full set of verification greps from Phases 1-5 (they must still be clean).
2. Use `grep` + `read_file` to spot-check that the 10 frozen files were **not** modified by any of the doc work (they shouldn't have been).
3. Read `scripts/check-live-safety-freeze.sh` and confirm the FROZEN_FILES list is still the 10 files.
4. Optionally: describe (without running full build) that `cmake` + `ctest` would be the next manual step the author should do locally.
5. Confirm no new files were accidentally created outside the plan.
6. Update todo "Last update" one final time if needed.
7. Produce a short "Pre-merge readiness summary" (can be appended to this plan or a separate note).

**Verification Gate**:
- All previous global branch-language greps still return 0 bad matches.
- `grep -r "LIVE-SAFETY SURFACE" src/ | cat` (should still find the markers in the original 10 files only).
- `read_file scripts/check-live-safety-freeze.sh | grep -A 20 "FROZEN_FILES" ` shows the correct current list.
- `git status` (read-only) still shows clean working tree on monte-carlo with only the expected doc edits.
- `list_dir .` does not show unexpected new root files.
- Manual review via read_file of one frozen file (e.g. src/core/tt_target.h) shows no recent doc-induced changes.

**Success Criteria**: Everything is consistent, frozen surface untouched by this effort, private-use framing is in place. **This is the final gate before any git merge commands are allowed.**

---

## Phase 7: Execute the Git Merge (Only After Phase 6 Gate Passes)

**Goal**: Perform the actual branch merge with a high-quality commit message that references this plan and the private-use reality.

**Prerequisites**: Phase 6 verification gate passed with zero errors. All previous phases marked completed in todo.

**Actions** (use `run_terminal_command` for these; capture full output):
1. `git checkout master`
2. `git status` (confirm clean, on master)
3. `git merge --no-ff monte-carlo -m "Merge branch 'monte-carlo' into master

Integrate Monte Carlo simulation engine (stochastic backtesting, synthetic provider, object-reuse campaigns, reporter).

This merge brings the major research/robustness capability to mainline.

IMPORTANT: Per project charter, TrueTest remains a private retail/personal research tool only and will never be positioned as enterprise production software. Documentation and governance language has been updated in this merge window to reflect that reality while preserving all technical safety primitives (compile-time live gating, Phase 1 freeze surface + enforcement script, hot-path rules, terminal halt semantics, etc.).

See MERGE_PLAN.md for the full phased execution record.
See todo.md for updated task status.
See root README.md, prod.md, and docs/production-readiness-gaps.md for revised scope statements.

Addresses MC integration + governance sync (todo.md MC-* and D-items)."`
4. `git status`
5. `git log --oneline -6`
6. `git branch -a` (to see local state)

**Verification Gate (post-merge)**:
- `git branch --show-current` → `master`
- `git log --oneline -1` contains the merge commit message with the key phrases ("Monte Carlo simulation engine", "private retail", "MERGE_PLAN.md")
- `git status` → clean working tree
- No files from the monte-carlo feature appear as "unmerged" or conflicted (should be automatic given clean history).
- Quick content check: `grep -F "Intended Use & Scope" README.md` still succeeds (merge preserved the doc changes).
- `grep -r "Development is active on the \`monte-carlo\` branch" . --include="*.md" | cat` → 0

**On success**: Mark Phase 7 completed. The merge commit is now the source of truth.

**Rollback note in plan**: If anything looks wrong, `git reset --hard HEAD~1` (while still on master) + return to monte-carlo to fix, but only after documenting the issue.

---

## Phase 8: Post-Merge Polish, Optional Remote Operations & Final Sign-Off

**Goal**: Clean up, optional remote sync (private repo — author discretion), final verification that the world (the local master) now correctly represents the integrated state + private scope.

**Actions** (only after Phase 7 gate):
1. On master: optionally update any root "Last updated" that can now say "post-merge".
2. Consider adding a tiny `docs/archive/monte-carlo-branch.md` or just rely on git history + this MERGE_PLAN.md.
3. Run one final global audit grep set.
4. (Author decision) `git push origin master` (if remote tracking exists).
5. (Strongly optional and only if you are sure) `git branch -d monte-carlo` (local). Remote delete is even more optional for a private repo.
6. Re-read the top of README.md and prod.md to confirm the new reality is the first thing a reader sees.
7. Mark the overall merge effort complete in todo.md (add a "Completed" entry under the Monte Carlo or Documentation section).

**Verification Gate**:
- `git branch --show-current` == master
- Final greps from Phase 5/6 still clean on master.
- `git log --oneline --graph -10` shows the merge commit as parent of current HEAD.
- Content of README "Current Development Status" (or its replacement) talks about MC as integrated capability.
- todo_write has a final "Monte Carlo to master merge — all phases completed successfully" item marked done.

**Success Criteria**: Master is the new baseline. The plan has been followed end-to-end with gates. Private retail character is clearly stated. All technical safety docs remain intact.

---

## Recommended Initial todo_write Structure (call at start of Phase 0)

Use this or an equivalent array when first invoking `todo_write`:

```json
[
  {"id": "phase-0-prep", "content": "Phase 0: Preparation, Inventory & Baseline Audit (read-only)", "status": "pending"},
  {"id": "phase-1-root-gov", "content": "Phase 1: Update Root Governance Documents (README, CLAUDE, todo, prod, summary)", "status": "pending"},
  {"id": "phase-2-docs-reports", "content": "Phase 2: Update docs/ Tree and reports/phase0/ (instructions, user-manual, gaps, AdaptiveHybrid, PROGRESS, etc.)", "status": "pending"},
  {"id": "phase-3-reframing", "content": "Phase 3: Private Retail Scope Reframing (disclaimers + tone in prod, gaps, README)", "status": "pending"},
  {"id": "phase-4-mc-hygiene", "content": "Phase 4: MC Task Hygiene, ENGINE_AI_SUMMARY cleanup, minor polish (todo + references)", "status": "pending"},
  {"id": "phase-5-audit", "content": "Phase 5: Final Cross-Project Consistency Audit & Pre-Merge Doc Lock", "status": "pending"},
  {"id": "phase-6-premerge", "content": "Phase 6: Pre-Merge Verification & Safety Confirmation (no git changes)", "status": "pending"},
  {"id": "phase-7-merge", "content": "Phase 7: Execute the Git Merge (only after Phase 6 gate)", "status": "pending"},
  {"id": "phase-8-post", "content": "Phase 8: Post-Merge Polish, Optional Remote, Final Sign-Off", "status": "pending"}
]
```

Advance status strictly: pending → in_progress (at phase start) → completed (only after that phase's verification gate passes with zero errors).

---

## Notes for Executor / Future Sessions
- This plan lives at `MERGE_PLAN.md`. Any executor should read it fresh at the start of work.
- The spirit is more important than literal string matches: keep the safety engineering excellent while being brutally honest that this will never be shipped as a production tool to others.
- If during execution you discover additional files with problematic language, treat them as part of the current phase's audit and extend the verification greps.
- Do not "improve" the live safety surface or frozen files as part of this merge effort unless they were already dirty (they are not).
- After successful Phase 8, this plan can be moved to `docs/archive/` if desired, but keeping it at root for a while provides good historical context.

**End of plan**. Execute one phase at a time. Gates are mandatory.

---
*Plan created to satisfy the request for a detailed, gated, step-by-step execution plan suitable for Grok Build / subagent / execute-plan usage.*
## Phase 6 Pre-Merge Readiness Summary (added during execution)

**Date of verification**: Post-Phase 5 audit.

**Status**: All previous phases' verification greps re-run and remain clean (0 bad matches for active/paused/Development/branch-focus language outside MERGE_PLAN.md self-refs and allowed historical/archive). 

**Frozen surface**: Spot-checked via grep + read_file on all 10 FROZEN_FILES (tt_target.h, engine.cpp, binance_futures_*.h, risk_*.h, live_safety.h, worker_watchdog.h). No doc-induced changes (no "Intended Use", "private retail", "Phase 5", "post-merge", or merge prep language in them). LIVE-SAFETY SURFACE markers confirmed only in original frozen files.

**Script confirmation**: scripts/check-live-safety-freeze.sh read; FROZEN_FILES list matches the 10 files exactly. Script run in check mode: passed (no violations reported; MC/doc work touched 0 frozen files).

**Working tree**: git status (read-only) shows clean on monte-carlo (only expected untracked temps from prior). list_dir . shows no unexpected new root files.

**Disclaimers**: Re-confirmed present in README, prod.md, docs/production-readiness-gaps.md, docs/user-manual.md, summary.md.

**Other**: No new files outside plan. cmake + ctest recommended as next manual author step locally (builds/tests should be clean based on prior). Pre-merge tree accurately reflects integrated private retail state per plan.

**Gate**: All Phase 6 verification criteria satisfied. Ready for Phase 7 (git merge) only after this.

