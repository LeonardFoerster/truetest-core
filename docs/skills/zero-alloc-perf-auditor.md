# Skill Proposal: zero-alloc-perf-auditor

**Proposed name**: `zero-alloc-perf-auditor`  
**Category**: Performance Guardian  
**Priority**: High  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)  
**Note**: Explicitly referenced from `engine-decomposition/SKILL.md` and ` (retired one-time review docs) ` but does not exist as a standalone skill yet.

---

## One-Sentence Description

A focused performance guardian skill that performs deep zero-allocation and hot-path jitter audits on changes. It is stricter and more specialized than the general `performance` skill, with the explicit mandate to act as the "alloc police" for any code that could affect the critical path, object pools, ring buffers, or worker rings.

---

## Why This Skill Is Needed

The project has extremely strong hot-path requirements:

- No heap allocations on hot paths
- Object pool discipline (`acquire_pooled`, prewarm, `forbid_runtime_grow`)
- SPSC ring buffers with occupancy/drop tracking
- Stage timers for debugging only (disabled in release hot paths)
- `nlohmann::json` is completely banned on hot paths (enforced by script)

The general `performance` skill exists and is good, but the referenced `zero-alloc-perf-auditor` is intended to be the **specialist** that other guardian skills (engine-decomposition, questdb-isolation, new strategy work, etc.) explicitly delegate alloc audits to.

Without it, every large refactor has to re-explain the entire alloc matrix, `alloc_counter.h`, rebaseline rules, etc.

---

## When to Use

- Automatically invoked (or strongly recommended) by `engine-decomposition`, `questdb-isolation`, `scaffold-strategy`, `ui-refactor`, etc.
- Any change that touches:
  - `src/engine/`
  - `src/orderbook/`
  - `src/threading/`
  - `src/execution/` (portfolio, order tracker)
  - New strategies or indicators
  - Workers
- Before claiming "no new allocations".

---

## Non-Negotiable Invariants

1. **alloc_counter** is the source of truth for hot-path windows.
2. The three dedicated tests are sacred:
   - `test_hotpath_allocs`
   - `test_hotpath_alloc_matrix`
   - `test_hotpath_pool_prewarm`
3. Rebaselining (`TRUETEST_REBASELINE_ALLOCS=1`) is only allowed with strong justification + explicit note in the PR / commit.
4. No `std::string`, `std::vector` growth, `new`, or JSON construction in measured hot paths.
5. Ring buffer drops and high occupancy must be investigated, not ignored.
6. StageTimer usage must not be left enabled in production hot paths.

---

## Detailed Workflow

### Phase 0 — Impact Analysis
- Use `git diff` to identify files that could affect hot paths.
- Classify changes: hot / warm / cold.
- Identify all new call sites into pooled objects or ring publishing.

### Phase 1 — Build & Instrument
- Build with appropriate flags (`-DENABLE_DEBUG=ON` for timers, native opt where relevant).
- Run the full hot-path alloc matrix test.
- Run the specific pool prewarm test.
- If the change touches engine event publishing or workers: run with `--status-format ndjson` or equivalent and inspect ring stats.

### Phase 2 — Deep Audit
- Manually (or via subagent) review the diff for:
  - Any `std::` containers used in decision paths
  - Temporary string construction
  - Unconditional allocations in loops
  - Changes to pool prewarm or acquire paths
- Run benchmarks that cover the changed area (`truetest_benchmarks`).
- Compare before/after numbers (the skill should help capture previous baseline).

### Phase 3 — Cross-Review + Ritual
- Spawn a fresh subagent whose only job is "find any possible new allocation or jitter source".
- The auditor skill must produce a clear "Alloc Verdict": PASS / FAIL / CONDITIONAL (with justification).
- Feed results into `check-work` and `performance`.

### Phase 4 — Documentation
- If any rebaseline was done: document why.
- Update any comments around measured hot paths.
- Add the changed paths to the "known hot path files" mental model.

---

## Integration With Other Skills

- This skill is a **dependency** of `engine-decomposition` and similar large refactors.
- `performance` skill can delegate the alloc portion to this auditor.
- `testing` skill runs the alloc tests; this skill interprets the results strictly.
- `check-work` should treat a failing zero-alloc verdict as an automatic FAIL for relevant changes.

---

## Success Criteria

- When another skill says "we did a zero-alloc audit", it means this skill (or an equivalent invocation) was used.
- It becomes impossible to accidentally introduce a hot-path allocation without the auditor catching it during the ritual.
- The alloc matrix test remains the single source of truth and its output is consistently quoted in reviews.

---

## References

- `tests/helpers/alloc_counter.h`
- `tests/test_hotpath_allocs.cpp`, `test_hotpath_alloc_matrix.cpp`, `test_hotpath_pool_prewarm.cpp`
- `~/.grok/skills/performance/SKILL.md`
- `~/.grok/skills/engine-decomposition/SKILL.md` (the section that references this skill)
- `scripts/check-hotpath-json.sh`
- `src/threading/ring_buffer.h`
- Engine hot path sections (publish_event, process_order, etc.)

---

## SaaS Note

In a SaaS deployment, "hot path" will also include job startup / per-trial reset overhead (because many short MC or backtest jobs will be run).

This auditor skill will need to expand its definition of "hot" to include:
- Per-trial reset paths
- Job initialization costs
- Memory footprint per concurrent job

The core "no allocations during bar/tick processing" rule remains unchanged and non-negotiable.

---

*This should be created soon because it is already referenced as if it exists.*
