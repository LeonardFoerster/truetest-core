# Phase 0 Evidence – reports/phase0/

**Purpose**: Single source of truth for all tiny-size mainnet futures validation sessions that qualify toward the 15+ required for Phase 0 → Phase 1 exit.

**Layout**
- `PROGRESS.md` — Master tracker table (one row per qualifying session + summary). This is the document reviewers and the CCB consult.
- `templates/phase0-session-note.md` — Printable/fillable one-page note template (signed after each session).
- `ops/` — Batch review notes (every 5 sessions) and volatility logs.
- Individual session artifacts live here or are referenced by run-tag (the zstd `.bin`, QuestDB `run_tag`, post-session `grep` output, etc.).

**Process (see `prod.md` for the authoritative command template + ritual + "why each element"; see `reports/phase0/` files + `scripts/phase0/` for operational machinery. Printable SOP planned in `docs/operations/futures-phase0-operator-sop.md` — Doc Phase 1; current details + template in `prod.md` + these files per CLAUDE "planned" rule)**
1. Run `./scripts/phase0/new-session.sh` (prints exact command + target dir under this tree).
2. Execute the session with all safety nets armed.
3. Run `post-session.sh`, volatility classifier, mandatory post-halt `grep`.
4. Commit the filled session note + any local artifacts under a dated subdir or directly here.
5. Update `PROGRESS.md` row.
6. Every 5 sessions: batch review in `ops/`, two-person sign-off on the batch.

**Status (2026)**: 0/15 qualifying sessions. Scripts and templates ready. Monte Carlo simulation capabilities (integrated from the monte-carlo branch, now mainline) are available for research and strategy robustness (see root `todo.md` MC-* section and `docs/instructions.md`). Phase 0 evidence collection remains at zero sessions; the requirements and ritual defined in `prod.md` are unchanged. (Language sync post monte-carlo merge.)

All entries must survive a clean 4-hour+ `engine_shadow` mainnet run with zero unexplained drift before counting toward the gate.

See `prod.md` (Phase 0 section) and the operator SOP for exact exit criteria and artifact checklist.