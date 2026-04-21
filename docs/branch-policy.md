# Branch Policy

Codifies the git workflow required by `prerequisites.md` §9 before any
deepdive code lands. This file is the source of truth; if GitHub
branch-protection settings drift from it, the settings are wrong.

> Applies to: all contributors, including Claude-driven commits. No
> exceptions without an explicit, documented waiver committed to this file.

---

## 1. Branches and their roles

| Branch              | Role                                                                | Accepts direct commits? |
|---------------------|---------------------------------------------------------------------|-------------------------|
| `master`            | Frozen: currently-running Binance/SQLite backtester.                | **No.** Merge from `pre_transform` only, only at phase boundaries, only after the phase is green in CI **and** has passed a manual shadow run. |
| `pre_transform`     | Long-lived integration branch for the deepdive refactor.            | PRs from `phase/*` branches only. |
| `phase/NN-name`     | One per `todo.md` phase (`phase/01-build`, `phase/02-memory`, …).   | PRs from `feat/*`, `fix/*`, `refactor/*` topic branches. |
| `feat/NN-slug`      | Feature inside a phase. Target: a single `phase/*` branch. Keep under ~500 LOC diff; stack PRs if the work is larger. | — |
| `fix/issue-NN`      | Bugfix against `master` that needs backporting.                      | — |
| `refactor/NN-name`  | `prerequisites.md` cleanup items only — never deepdive features.     | — |
| `spike/name`        | Experimental. Never merged — cherry-pick the keeper commits instead. | — |
| `archive/<name>`    | Read-only freezer for abandoned branches (e.g. the original `wasm`). | **No.** Created once via `git branch archive/wasm wasm` then locked. |

The merge direction is strictly one-way: `feat → phase → pre_transform → master`.

---

## 2. Rebase, don't merge

Every PR merges with **Rebase and merge** (or squash — never "Create a merge
commit"). Linear history makes `git bisect` viable when the hot path
regresses by 30 ns six months from now. Enforce in the repo's
GitHub settings:

- **Settings → Branches → Branch protection rules** for `master` and
  `pre_transform`:
  - Require a pull request before merging
  - Require approvals: **1** (more for credentials files — see §6)
  - Dismiss stale approvals on new pushes
  - Require status checks to pass before merging — select every job in
    `ci.yml` (see §5)
  - Require branches to be up to date before merging
  - Require linear history
  - Do not allow bypassing the above settings
  - Restrict force pushes: **disabled**
  - Restrict deletions: **disabled**
- **Settings → General → Pull Requests**:
  - Allow merge commits: **off**
  - Allow squash merging: **on**
  - Allow rebase merging: **on**
  - Automatically delete head branches: **on**

Phase branches (`phase/*`) should have the same rules with approvals relaxed
to 0 — they're the working branches, not the integration points.

---

## 3. Rollback tags

The last pre-refactor tips are tagged so any phase can be rolled back to a
known-good state:

```
pre-deepdive-20260421-master         # master tip before Step 1
pre-deepdive-20260421-pre_transform  # pre_transform tip at the start of §9
```

If a deepdive phase has to be abandoned, reset the phase branch to the
matching tag rather than reverting a chain of PRs.

When a phase is merged into `master`, create a new dated tag
(`phase-NN-merged-YYYYMMDD`) at the merge commit for the same reason.

---

## 4. Credentials and secrets

### Patterns forbidden on every branch

`.gitignore` rejects `config/live*.toml`, `*.pem`, `*secret*`, `.env` and
related patterns. The pre-commit hook in `scripts/pre-commit` rejects staged
additions that match them even when `git add -f` is used.

Activate the hook once per clone:

```bash
git config core.hooksPath scripts
```

CI should add a job that re-runs this check server-side so a missing hook
configuration cannot bypass it (TODO: add `credentials-check` job to
`ci.yml` before the first Phase 3 PR that ships a working live-REST
credential store).

### Bypassing

`--no-verify` is forbidden. `--no-gpg-sign` is forbidden. If the hook
produces a false positive, rename the file; do not bypass.

### Branches that may reference live credentials

Only `engine_live`-focused branches need to touch credential files at all.
Those branches:

- Must exist only under the `phase/*` or `feat/*` hierarchy — never as
  long-lived custom branches.
- Require two approvals before merging (configure as a separate
  branch-protection rule matching `feat/*-live-*` and similar patterns).
- Must pass the credentials-check CI job.

---

## 5. CI gates

See `.github/workflows/ci.yml` and `.github/workflows/nightly.yml`.

Every PR into `pre_transform` or `master` must be green on:

- Default build matrix (gcc-13 × clang-17 × Debug/Release).
- `asan` (AddressSanitizer + UndefinedBehaviorSanitizer).
- `binance` (optional provider build).
- `postgresql` (optional persistence build).
- `format` (clang-format-17 dry-run, `--Werror`).
- `tidy` (clang-tidy-17 against `compile_commands.json`).
- `benchmarks` (builds `truetest_benchmarks` + smoke-runs it).

**TSAN** is slow and noisy under concurrency bugs that are already being
ironed out in Phase 2–4; it runs in `nightly.yml` on a cron schedule against
`pre_transform` only. A phase branch cannot fast-forward into `master`
without a green nightly TSAN in the previous seven days.

---

## 6. Review sizing

Keep `feat/*` PRs under ~500 LOC of meaningful diff (whitespace/rename
commits don't count). Anything larger must be split into stacked PRs.
Tools: `git-spice` locally, or `gh pr create --base phase/...` for plain
stacks.

Large mechanical changes (formatter sweep, namespace rename, source-tree
move) get their own PR so reviewers can skim the diff instead of auditing
it line-by-line. The Step 3 `BacktestEngine/src/** → src/**` rename is the
canonical example.

---

## 7. The `wasm` branch

As of 2026-04-21, `wasm` is fully contained in `pre_transform` (zero unique
commits; the WebAssembly identifier on the branch is a transitive npm
dependency, not our code). Recommendation:

```bash
git branch archive/wasm wasm
git push origin archive/wasm
git push origin --delete wasm
git branch -d wasm
```

Do this only after the team has acknowledged there is nothing on `wasm`
worth cherry-picking. The archive branch stays around as a zero-cost
paper trail.
