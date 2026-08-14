# C++ Code Review — Full Lens Checklists (truetest-core)

Use during Phase C. Not every item applies to every diff — skip N/A explicitly
in **Non-findings** when something looks relevant but is fine.

Canonical policy lives in `AGENTS.md` and `docs/architecture/*`. This file is a
review aid, not a second policy source.

---

## A. Correctness checklist

### Logic & contracts

- [ ] Behavior matches stated intent (PR body, comments, flag docs)
- [ ] Edge cases: empty book, zero qty, max qty, missing symbol, duplicate id
- [ ] Error paths do not double-apply fills / double-free / skip risk
- [ ] Comparisons use correct types (price ticks, integer qty, time units)
- [ ] Off-by-one on rings, windows, SMA periods, indices

### Lifetime & memory

- [ ] Pool acquire → publish ownership transfer is clear
- [ ] No use-after-recycle / dangling pointers across handoff
- [ ] No iterator invalidation after container mutation
- [ ] `const` correctness truthful; no `const_cast` smuggling mutation
- [ ] ASAN-sensitive patterns (OOB, lifetime) called out when present

### Concurrency

- [ ] Each SPSC ring has exactly one producer for engine-originated traffic
- [ ] Atomics: appropriate `memory_order`; no data race on non-atomic shared
- [ ] No lock on HOT path unless pre-existing and justified
- [ ] Worker shutdown / join does not race with last events
- [ ] Watchdog / heartbeat interactions remain fail-closed

### Safety & live gates

- [ ] Halt is write-once terminal (no auto-clear, no retry-to-resume)
- [ ] Kill / DMS / reconciler: loud, non-retrying, fail-closed
- [ ] No runtime “allow live” switch; `TT_TARGET` / constexpr gate intact
- [ ] Freeze-file edits flagged for CCB + `LIVE_SAFETY_CCB_APPROVED`
- [ ] Reconciler default-refuse; user-data stream SoT when relevant
- [ ] Futures: venue risk check before generic risk manager

### Determinism (backtest / MC)

- [ ] Same seed + inputs + flags → same results
- [ ] MC: per-trial seed from base + trial id; report `seed_used`
- [ ] No hidden shared mutable state between parallel trials
- [ ] `--mc-parallel` only with compatible thread preset (`inline`)
- [ ] Time/source of randomness not wall-clock unless intentional & documented

### API / layer

- [ ] New includes respect layer graph (`check-layer-deps.sh`)
- [ ] Venue logic stays in provider adapters, not generic engine/core
- [ ] Interfaces use project `I` prefix when introducing ports
- [ ] New `.cpp` registered in `cmake/Sources.cmake`

### Tests

- [ ] Bug class has or needs a test (unit / integration / property)
- [ ] Tests are not circular (asserting the implementation’s private accident)
- [ ] Failure mode tested (pool exhaust, ring full, risk reject, halt)

---

## B. Performance checklist

### Path classification

- [ ] Each changed function tagged HOT / WARM / COLD with call-chain evidence
- [ ] No cold helper secretly invoked from HOT without note

### Zero-alloc / pools

- [ ] No new heap on HOT (`new`, `malloc`, growing containers, string build)
- [ ] Uses `acquire_pooled` / existing pools where appropriate
- [ ] `forbid_runtime_grow` remains true after prewarm
- [ ] Exhaust path fail-closed (not silent grow)
- [ ] Prewarm sizes justified (max concurrent + headroom)

### Rings & threading

- [ ] SPSC only; sole producer preserved
- [ ] No MPMC introduced “for convenience”
- [ ] Shared atomics padded if false-sharing risk is real (measure/justify)
- [ ] Affinity / thread presets not broken by new threads

### Hot-path ban list

- [ ] No `nlohmann::json` on HOT (`check-hotpath-json.sh`)
- [ ] No heavy `fmt` → `std::string` / sync logging on HOT
- [ ] No exceptions for control flow on tight loops
- [ ] No new virtual dispatch in micro-hot loops without measurement
- [ ] No unbounded locks / priority inversion on event path

### Design reuse

- [ ] Reuses publish_event / workers / adapters — no parallel bus
- [ ] I/O, config, symbol resolve stay at startup when possible
- [ ] Contiguous layouts preferred over node maps on hot structures

### Measurement

- [ ] Perf claims cite bench/filter or explicit “unmeasured hypothesis”
- [ ] p99 / jitter considered, not mean-only
- [ ] Optional: hotpath ctests when pools/rings touched

---

## C. Simplicity checklist

- [ ] Smallest change that fixes the problem
- [ ] No dual systems (second pool, second logger, second event path)
- [ ] No framework/factory/visitor for a single call site
- [ ] Dead code / flags / paths removed rather than left “just in case”
- [ ] Branch complexity could be data or named predicates — if clearer
- [ ] No speculative generality (“for future venues”) without need
- [ ] Extraction reduces net complexity (not file shuffle)
- [ ] Dependencies: no new third-party lib without human approval

---

## D. Readability checklist

- [ ] Names match surrounding module and domain language
- [ ] Functions do one job; hot path readable top-to-bottom
- [ ] Invariants documented where non-obvious (not narrating comments)
- [ ] Ownership visible in types/signatures
- [ ] Headers: minimal includes; forward declare when enough
- [ ] Consistent error reporting style with neighbors
- [ ] File length: flag ~800+ lines; justify further growth
- [ ] No drive-by renames / format of unrelated code

---

## E. Gate scripts (orchestrator)

From `core/`:

```bash
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

| Script | Fail means |
|--------|------------|
| hotpath-json | JSON on disallowed hot path or allow-list break |
| layer-deps | Illegal include direction / layer edge |
| live-safety-freeze | Frozen surface changed without process |

Optional focus tests:

```bash
ctest --test-dir out/build/linux-tests -R 'hotpath|Hotpath|ObjectPool|Ring' --output-on-failure
```

---

## F. Severity quick assign

| Signal | Typical severity |
|--------|------------------|
| Silent event loss / wrong fills / UB | blocker |
| Halt auto-clear / live gate bypass | blocker |
| Gate script fail | blocker |
| HOT heap / JSON on HOT / second SPSC producer | high |
| MC non-determinism / shared trial state | high |
| Missing risk order on futures path | high |
| Dual subsystem / over-abstraction | medium (simplicity) |
| Naming / comment / pure style | low or nit |

---

## G. Subagent output self-check

Before returning, each lens verifies:

1. Findings reference `file:line` (right side / current file).
2. Each finding has severity, problem, why, fix direction, falsify idea.
3. HOT tags only when call-chain supports it.
4. No invented issues to fill quota.
5. Lens verdict is set: `PASS` | `FAIL` | `PASS_WITH_NITS`.
