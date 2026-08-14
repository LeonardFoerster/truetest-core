---
name: cpp-code-review
description: >
  Structured multi-lens C++ code review for truetest-core. Reviews diffs, files,
  branches, or PRs for correctness, performance (hot-path / zero-alloc / SPSC),
  readability, and simplicity — with concrete bad/good examples, parallel
  specialist subagents, gate scripts, and a severity-ranked report ending in
  CPP-CODE-REVIEW VERDICT. Triggers: /cpp-code-review, C++ code review, cpp
  review, review this diff, review engine change, hot-path review, zero-alloc
  review, readability review, simplicity review, correctness review, review
  backtest code, review orderbook, review pool/ring change. Use when the user
  runs /cpp-code-review or asks for a thorough C++ review of core changes.
metadata:
  short-description: "Multi-lens C++ review (perf, correctness, simplicity)"
---

# /cpp-code-review — truetest-core C++ Review

**Goal:** Produce an evidence-based review of C++ changes that optimizes for
**correctness**, **performance**, **readability**, and **simplicity** — in that
priority order when they conflict — against this repo’s `AGENTS.md` and freeze /
hot-path rules.

**Not a substitute for:** `/testing` (full gate), `/safety` (freeze/live surface),
`/check-work` (session verification), or the generic `/review` orchestrator
(PR posting). This skill **reviews**; it does not implement fixes unless the
user explicitly asks after the verdict.

---

## 0. Invocation

```
/cpp-code-review                         # local uncommitted changes (default)
/cpp-code-review --local
/cpp-code-review --branch <name>
/cpp-code-review --base <ref>            # compare working tree / HEAD to ref
/cpp-code-review --files path1 path2     # explicit file set
/cpp-code-review --pr <n|url>            # GitHub PR (review only; no auto-post)
/cpp-code-review --quick                 # skip adversarial pass; still run lenses
/cpp-code-review <focus notes>           # free-text focus after flags
```

**Parse order:** flags first (`--local`, `--branch`, `--base`, `--files`, `--pr`,
`--quick`); remaining text is **focus** (e.g. “pool prewarm”, “determinism”,
“layer deps”).

If the working tree is clean and no `--branch` / `--files` / `--pr` / `--base`
is given, stop with: “Nothing to review (clean tree). Pass `--branch`, `--files`,
or `--pr`.”

---

## 1. Priority model (non-negotiable)

When lenses disagree, rank findings and recommendations by:

| Rank | Lens | Wins when… |
|------|------|------------|
| 1 | **Correctness** | Logic, races, UB, wrong results, fail-open safety |
| 2 | **Performance** | Hot-path alloc/jitter/throughput regressions |
| 3 | **Simplicity** | Unnecessary abstraction, dual systems, cleverness |
| 4 | **Readability** | Naming, structure, comments that aid understanding |

Rules:

- Never “optimize” by weakening safety, determinism, or fail-closed behavior.
- Never “simplify” by deleting required risk/freeze checks.
- Prefer the **smallest correct change** over a clever general framework.
- Style nits that do not affect the four lenses are optional (severity `nit`).

---

## 2. Workflow overview

```
Phase A  Scope & classify          (orchestrator, read-only)
Phase B  Mechanical gates          (orchestrator runs scripts)
Phase C  Parallel specialist lenses (3 subagents, read-only)
Phase D  Adversarial contradiction  (1 fresh subagent, optional but default on)
Phase E  Synthesize report + verdict (orchestrator only)
```

- **Orchestrator** never edits source during a pure review.
- **All review subagents** are read-only (`capability_mode: "read-only"` when available; otherwise instruct: do not write/edit/commit).
- Prefer **fresh** subagents that did **not** implement the change under review.
- Run Phase C lenses **in parallel** via multiple `spawn_subagent` calls in one turn.

---

## 3. Phase A — Scope & classify

### 3.1 Collect the change set

| Mode | How |
|------|-----|
| local | `git status --porcelain`; `git diff HEAD` + untracked via `git ls-files --others --exclude-standard` |
| branch | merge-base vs `origin/main` (or `origin/master`); `git diff MERGE_BASE..branch` |
| base | `git diff <ref>` (and untracked if reviewing working tree) |
| files | read listed paths; `git diff -- path…` if tracked |
| pr | `gh pr diff <n>` + `gh pr view <n> --json title,body,files,baseRefName,headRefName` |

Also:

```bash
git diff --stat
git diff --name-only
# or equivalent for the chosen mode
```

### 3.2 Classify every touched path

Tag each path with one or more:

| Tag | Meaning | Examples |
|-----|---------|----------|
| `HOT` | Per-event / per-order / per-tick | `engine` publish, orderbook apply, strategy `on_*`, pre-trade risk, SPSC push |
| `WARM` | Frequent but not micro-hot | stats workers, dashboard snapshot, MC trial setup |
| `COLD` | Startup / CLI / reports / tests | `main.inc`, config, JSON report, unit tests |
| `FREEZE` | Live-safety freeze surface | files in `scripts/check-live-safety-freeze.sh` |
| `SAFETY` | Halt / kill / DMS / reconciler / risk | even if not on the frozen ten |
| `THREAD` | Rings, affinity, workers, sole-producer | `src/threading/*`, engine ring producers |
| `LAYER` | Cross-layer includes / new deps | any new `#include` across layers |
| `DET` | Determinism / MC / seed / replay | MC controller, generators, seed plumbing |
| `DOC` | Docs only | `docs/**` |

**Hot-path definition (repo):** engine event loop & publish; provider parse → handoff;
strategy callbacks on market/order events; orderbook apply/match; pre-trade risk on
the event; critical SPSC push/pop on safety/market paths.

### 3.3 Risk tier for this review

| Tier | When | Required subagents |
|------|------|--------------------|
| **T1** | COLD only, docs/tests/strategies pure cold | Correctness + Simplicity (2) |
| **T2** | Any HOT/WARM/THREAD/DET/LAYER | All 3 lenses + gates (default) |
| **T3** | Any FREEZE/SAFETY or `engine.cpp` / `tt_target.h` | All 3 + adversarial + **flag `/safety`** |

If T3: state clearly that freeze edits require `LIVE_SAFETY_CCB_APPROVED`, human CCB,
and the multi-agent protocol in root/`core` `AGENTS.md`. Do not rubber-stamp freeze diffs.

### 3.4 Progress line

Tell the user:

```
Reviewing <mode>: <N> files (<hot> HOT, <warm> WARM, <cold> COLD; tier T?).
Running gates, then parallel lenses…
```

---

## 4. Phase B — Mechanical gates (orchestrator)

Always run from `core/` when any `src/` path is in scope (or when unsure):

```bash
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

Record pass/fail verbatim. A gate fail is at least severity **blocker** in the report
(unless the change intentionally updates the allow-list / freeze list with required
process — still flag as process risk).

Optional, when HOT/THREAD/pool files touched and a build tree exists:

```bash
# Prefer project presets when available
ctest --test-dir out/build/linux-tests -R 'hotpath|Hotpath|ObjectPool|Ring' --output-on-failure
# or: ctest --test-dir build -R 'hotpath|Hotpath|ObjectPool|Ring' --output-on-failure
```

Do **not** start a full clean rebuild solely for review unless the user asked or
no usable build dir exists and correctness of a compile-sensitive change is unclear.
Note “not run” honestly in the report.

Static smell scan on changed HOT/WARM files (adjust paths to the diff):

```bash
rg -n 'new |malloc\(|make_shared|std::string|push_back|emplace_back|nlohmann::json|std::mutex|shared_ptr' \
  --glob '*.cpp' --glob '*.h' <changed-hot-paths>
```

Treat hits as **candidates**, not automatic defects — classify hot vs cold via call context.

---

## 5. Phase C — Parallel specialist subagents

Spawn **three** read-only subagents in one turn for T2/T3 (for T1, omit Performance
unless HOT appears). Use `subagent_type: "general-purpose"` (or `"explore"` if
read-only explore is sufficient for pure inspection). Prefix descriptions:

| Description prefix | Lens |
|--------------------|------|
| `[cpp-review:correctness]` | Correctness |
| `[cpp-review:performance]` | Performance |
| `[cpp-review:clarity]` | Readability + Simplicity |

Each prompt must include:

1. **Mode + scope** (file list, diff location or `git` command to reproduce).
2. **Classification map** from Phase A (path → tags).
3. **Focus notes** from the user (if any).
4. **Instruction:** read the diff **and** surrounding source (`read_file`); do not trust the diff alone.
5. **Instruction:** do not modify files; do not run destructive commands.
6. **Required output schema** (below).
7. **Repo pointers:** `AGENTS.md` (core), hot-path R1–R10, safety S1–S10, prefer existing pools/rings/publish APIs.
8. **Examples file:** `.grok/skills/cpp-code-review/references/examples.md`
9. **Checklist file:** `.grok/skills/cpp-code-review/references/checklist.md`

### 5.1 Shared finding schema (every lens)

Each subagent returns markdown:

```markdown
## Lens: <correctness|performance|clarity>
## Scope summary
<2-4 sentences: what changed and what you inspected>

## Findings
### F1 — <title>
- Severity: blocker | high | medium | low | nit
- Lens: correctness | performance | simplicity | readability
- File: path/to/file.ext:LINE
- Path tags: HOT|WARM|COLD|...
- Problem: <what is wrong, concrete>
- Why it matters: <user-visible / system impact>
- Evidence: <code quote, invariant, gate, or missing test>
- Fix direction: <smallest concrete fix; not a redesign essay>
- Counterexample / test idea: <how to falsify or lock behavior>

## Non-findings (explicit OK)
- <things that look scary but are fine, with reason>

## Residual risks
- <accepted or out-of-scope risks>

## Lens verdict
PASS | FAIL | PASS_WITH_NITS
```

**Severity guide:**

| Severity | Use for |
|----------|---------|
| `blocker` | Wrong results, UB likely, safety fail-open, freeze violation, sole-producer break, gate fail |
| `high` | Hot-path heap, determinism break, missing mandatory risk order, data race plausible |
| `medium` | Real bug class or maintainability trap with plausible production impact |
| `low` | Solid improvement; limited blast radius |
| `nit` | Pure taste / micro-style |

**Do not invent findings.** Empty `## Findings` is valid. Prefer fewer true issues
over a laundry list of hypotheticals.

### 5.2 Correctness lens — prompt body (paste)

```
You are the CORRECTNESS lens for truetest-core C++ review.
Priorities: logic bugs, edge cases, lifetime/ownership, concurrency, determinism,
fail-closed safety, API contracts, test gaps.

Inspect:
- Off-by-one, empty/full containers, error paths that drop events or double-apply
- Ownership: pools vs unique ownership; use-after-recycle; iterator invalidation
- Concurrency: SPSC sole-producer, atomics memory order, false sharing only if real
- Halt/kill/DMS: write-once halt; no retry loops; no auto-clear of halt
- TT_TARGET / live gates: no runtime bypass that resurrects live code in backtest/shadow
- Determinism: seed + trial id; no hidden shared mutable state across MC trials
- Risk order: venue FuturesRiskCheck before RiskManager on futures hot path when relevant
- Layer rules: provider is venue extension point; no venue leakage into generic core

Read references/examples.md sections on correctness.
Return the shared finding schema only.
```

### 5.3 Performance lens — prompt body (paste)

```
You are the PERFORMANCE lens for truetest-core C++ review.
Priorities: zero heap on HOT path, low jitter, pools, SPSC, no JSON/heavy fmt on HOT,
measured claims only.

Hot path includes: engine event loop/publish, provider parse→handoff, strategy on-event,
orderbook apply/match, pre-trade risk on-event, critical SPSC push/pop.

Ban-list on HOT (unless proven cold):
- new/delete/malloc, std::string build-up, vector growth, make_shared,
  nlohmann::json, sync logging/formatting, unbounded locks, exceptions for control flow

Require:
- acquire_pooled / publish_event / existing workers — no parallel subsystem invention
- forbid_runtime_grow stays on after prewarm; exhaust → fail closed
- No second producer on SPSC rings
- Cold-path alloc OK if truly cold and not sneakily called from HOT

Classify each finding as HOT vs false positive (cold).
Read references/examples.md sections on performance.
Return the shared finding schema only.
```

### 5.4 Clarity lens (readability + simplicity) — prompt body (paste)

```
You are the CLARITY lens (readability + simplicity) for truetest-core C++ review.
Priorities: simplest correct design; delete dead paths; naming; structure; comments
that capture invariants — never “clean up” freeze/hot code into something slower
or less safe.

Flag:
- Dual systems / parallel frameworks that should reuse existing APIs
- God functions, deep nesting, boolean soup without named predicates
- Over-abstraction (factories/visitors) for one call site
- Unclear ownership or lifetime expressed only in prose
- Comments that narrate (“increment i”) instead of invariants
- File growth past ~800 lines without extraction plan (flag, don't demand rewrite)
- Inconsistent naming vs surrounding module (I* interfaces, etc.)

Do NOT demand style churn unrelated to the diff.
Prefer deletion and extraction over new layers.
Read references/examples.md sections on simplicity/readability.
Return the shared finding schema only. Tag lens as simplicity or readability per finding.
```

---

## 6. Phase D — Adversarial pass (default on for T2/T3)

Spawn one **fresh** subagent that did not run in Phase C:

- Description: `[cpp-review:adversarial]`
- Goal: ≥3 **specific, falsifiable** contradictions to the claim “this change is merge-ready”, **or** an explicit statement that after attack no material contradiction remains (rare; must justify).

Prompt core:

```
Devil’s-advocate review of the same change set.
Steelman the change in ≤3 bullets, then attack it.
Each contradiction needs: Risk, Why plausible, Falsify with (test/metric/log), Severity.
No vague “be careful”. Prefer concrete counterexamples.
Output: steelman, contradictions, missing tests, residual risks, merge-ready|merge-after|do-not-merge.
```

If Phase C already produced `blocker`/`high` findings, still run adversarial — it often
finds second-order issues (e.g. fix pattern that reintroduces a race).

Skip Phase D only if user said `--quick` or scope is T1 docs-only.

---

## 7. Phase E — Synthesize (orchestrator only)

Merge all lens outputs. Deduplicate by (file, line, problem). When two lenses report
the same issue, keep the **higher severity** and note both lenses.

### 7.1 Report format (user-facing)

Write the full report to the user (and optionally to
`check-ups/YYYY-MM-DD-cpp-code-review.md` only if the user asked to save it).

```markdown
# C++ Code Review — <short title>

- **Mode:** local | branch | base | files | pr
- **Tier:** T1 | T2 | T3
- **Scope:** <N> files; tags summary
- **Gates:** hotpath-json ✅/❌ · layer-deps ✅/❌ · live-safety-freeze ✅/❌
- **Tests run:** <list or “not run”>
- **Focus:** <user focus or “—”>

## Executive summary
<3–6 sentences. Lead with merge recommendation and worst risk.>

## Priority findings
### P1 — <title>  `[blocker|high]` · <lens>
- **Where:** `path:line`
- **Problem:** …
- **Why it matters:** …
- **Fix:** …
- **Falsify:** …

### P2 — …
(only material findings; order by severity then hot-path impact)

## Lens scorecard
| Lens | Verdict | Blockers | High | Med | Low/Nit |
|------|---------|----------|------|-----|---------|
| Correctness | … | | | | |
| Performance | … | | | | |
| Clarity | … | | | | |
| Adversarial | merge-* | — | | | |

## Explicit non-issues
- <scary-looking but OK items, so authors do not thrash>

## Simplicity / readability notes
- <optional non-blocking structural advice>

## Suggested tests
- [ ] <concrete test names or properties>

## Residual risks
- …

## CPP-CODE-REVIEW VERDICT: PASS | PASS_WITH_NITS | FAIL

### Verdict rules
- **FAIL** if any blocker/high remains, any gate failed, or adversarial says do-not-merge for a material reason
- **PASS_WITH_NITS** if only medium/low/nit remain and no hot-path/safety correctness hole
- **PASS** if no material findings (nits optional)
```

### 7.2 How to talk about examples in the report

When a finding matches a known anti-pattern, cite the example id from
`references/examples.md` (e.g. “same class as **PERF-01** heap on hot path”).

### 7.3 After FAIL

Do **not** silently implement fixes during the review skill unless the user says
“fix the review findings” (or similar). Offer a short ordered fix list:

1. Blockers / safety  
2. Hot-path performance  
3. Correctness medium  
4. Simplicity cleanups  

---

## 8. Orchestrator checklist (do not skip)

- [ ] Scope collected; empty scope handled  
- [ ] Paths classified (HOT/WARM/COLD/…)  
- [ ] Tier chosen; T3 called out with safety process  
- [ ] Three gate scripts run when `src/` involved  
- [ ] Parallel specialist subagents launched (tier-appropriate)  
- [ ] Adversarial pass for T2/T3 (unless `--quick`)  
- [ ] Findings deduped, severities reconciled  
- [ ] Report uses the format above and ends with `CPP-CODE-REVIEW VERDICT:`  
- [ ] No source edits; no commits; no PR review posts unless user asked another skill  

---

## 9. Relationship to other skills

| Skill | Use instead / after when… |
|-------|---------------------------|
| `/review` | Need PR PENDING comments on GitHub |
| `/performance` | Deep perf ritual + benchmarks as primary task |
| `/cpp-performance` | Technique playbook (layout, atomics, SIMD) |
| `/safety` | Freeze surface / kill / DMS / reconciler edits |
| `/testing` | Full test + gate hard stop before commit |
| `/check-work` | “Did this session finish the user’s request?” |
| `/quality` | Format/lint/file-size only |
| `/memory-checks` | ASAN/leak/race investigation report |
| adversarial-code-review | Standalone red-team without full four-lens report |

This skill **embeds** a lighter adversarial pass; for money-path freeze work still run `/safety`.

---

## 10. Quick reference — repo red lines (compressed)

**Performance (R):** zero heap HOT; `forbid_runtime_grow`; SPSC sole producer; pad atomics;
no exceptions/RTTI/virtual in tight loops without measurement; no JSON/heavy fmt HOT;
contiguous layouts; move I/O to startup; measure p99; reuse pools/publish/workers.

**Safety (S):** freeze sacred; compile-time live gate; halt write-once; loud non-retrying
fail-closed kill/DMS/reconciler/watchdog; do not collapse safety mechanisms; reconciler
default-refuse; pre-trade risk ordered; fixed DMS conservatism; no venue `HAS_*` in generic layers.

Full detail: `AGENTS.md`, `docs/architecture/02-model.md`, `docs/architecture/04-performance.md`.

---

## 11. Minimal example (what good output looks like)

```markdown
# C++ Code Review — queue-aware adapter fill path

- Mode: local · Tier: T2 · Gates: all ✅ · Tests: hotpath* not run
- Scope: 4 files (2 HOT, 1 WARM, 1 COLD)

## Executive summary
Fill notification now allocates a temporary `std::vector` on the match path (HOT).
Correctness of queue position math looks sound; simplify the dual “fast/slow”
logging branches before merge.

## Priority findings
### P1 — temporary vector on match  `[high]` · performance
- Where: `src/execution/queue_aware_book_adapter.h:218`
- Problem: `std::vector<Level> levels` built per fill (PERF-01 class)
- Fix: pre-sized array or stack buffer; or reuse thread-local/cold diagnostics only
- Falsify: hotpath alloc test / bench filter Orderbook

## CPP-CODE-REVIEW VERDICT: FAIL
```

See `references/examples.md` for bad/good code pairs used by the lenses.
See `references/checklist.md` for the full per-lens checklist.
