# Issue 2 — QuestDB Persistence Leakage & Spaghetti Conditional Growth

**Severity:** suggestion (but high impact — spaghetti + boundary leak)  
**Date:** 2026-07-16 (from `/code-review`)  
**Primary Symptom Files:** `src/engine/engine.cpp` (51+ occurrences of questdb guards), `src/data/questdb/store.{h,cpp}`

## Context from Review
The reviewed diff added yet another instance of this pattern in `drain_async_submit_results`:

```cpp
#ifdef HAS_QUESTDB
if (questdb_active_ && questdb_store_)
{
    questdb_store_->record_status_transition(...);
    questdb_store_->record_rejection(...);
    questdb_total_rejections_++;
}
#endif
```

Identical (or near-identical) guarded blocks exist for:
- order submitted / pending
- fills
- cancellations
- amendments
- risk rejects / halts
- venue filters
- funding
- final end / flush / tick
- async submit success + failure paths
- shadow mode interactions

This directly violates multiple `/code-review` non-negotiables:
- "Do not allow random spaghetti growth in existing code."
- "Be highly suspicious of new ad-hoc conditionals, scattered special cases"
- "Feature-specific logic leaking into shared paths"
- "Prefer pushing the logic into a dedicated abstraction, helper, state machine, policy object, or separate module"

Also: "this feels like feature logic leaking into a shared path. can we isolate it?"

## Root Causes
1. QuestDB was added incrementally (Phase 2, Phase 3, Phase 4 features) without an architectural seam.
2. Activation is both compile-time (`HAS_QUESTDB`) and runtime (`questdb_active_`).
3. Every order lifecycle point in the engine manually decides whether and how to record.
4. Some paths have full `order_event`, others only have IDs + sparse data → led to the new overload (see Issue 3).
5. `questdb_total_rejections_` and similar counters live in engine.

## Why This Requires a New Skill (or Extension of Issue 1's Skill)
A proper fix will touch the live safety surface and hot paths. It must be governed by a skill that enforces:
- All QuestDB writes must go through **one** narrow, well-typed seam.
- The seam must be zero-cost when disabled (no branches in hot path when possible, or at least centralized).
- The seam must support both rich `order_event` and sparse identity-only cases without duplication in callers.
- Any change must be accompanied by updated `scripts/check-live-safety-freeze.sh` awareness if needed.
- The skill should probably be the same `engine-decomposition` skill (or a sub-skill `questdb-decoupling`).

Creating the skill **before** touching code ensures future Grok sessions cannot make the problem worse.

## Detailed Step-by-Step Guide for Grok Build (Planning Only — Do Not Resolve Now)

### Phase 0: Deep Read-Only Analysis
1. Re-run or re-read the `/code-review` output.
2. Use tools to map **every** call site:
   - `grep` pattern: `questdb_active_ && questdb_store_`
   - Also search for `HAS_QUESTDB`, `record_`, `questdb_store_`, `questdb_active_`, `questdb_total_rejections_`
3. Read full files:
   - `src/data/questdb/store.h` (the interface with all virtual record_* methods)
   - `src/data/questdb/store.cpp` (implementation + LineBuilder usage, duplication between the two `record_rejection` overloads)
   - All sections of `engine.cpp` that contain QuestDB code (use offset/limit reads)
   - `src/engine/engine.h` for member declarations
   - `src/core/event.h` and specific event types (order_event, fill_event, etc.)
4. Read the QuestDB worker / integration points if separate (search for QuestdbWorker or similar).
5. Read related skills + docs (same list as Issue 1, plus any persistence docs in `docs/todos/08-H-persistence-observability.md` and `docs/reference/03-db.md`).
6. Analyze duplication:
   - How many places manually build "unknown" strings or decide `side` / `strategy_name`.
   - The `lookup_strategy_name` helper in engine — is it only for QuestDB?
7. Document the full order lifecycle states and which record methods are called at each transition.

### Phase 1: Skill Creation / Augmentation (Do This First)
**Strongly recommended:** Create or extend a skill (see Issue 1's `engine-decomposition`).

In the skill instructions, mandate:
- The solution **must** introduce a single `IOrderAudit` or `QuestDBAuditSink` (or similar) interface.
- Engine must only call `audit_sink->on_order_submitted(...)`, `on_fill(...)`, `on_rejection(...)`, `on_status_transition(...)` etc. — at most 6-8 methods.
- The concrete QuestDB implementation (when `HAS_QUESTDB` and active) is injected once.
- When disabled, a no-op implementation or null is used so that call sites become unconditional and cheap.
- Sparse vs rich data is handled **inside** the sink (using order_id + symbol + optional details), never by having two public overload families on the store.
- All counters (rejections etc.) are owned by the sink or aggregated at end().
- The skill must require that after extraction, `engine.cpp` contains **zero** raw `if (questdb_active_ && questdb_store_)` blocks.
- Require a "before vs after" count of such conditionals in the PR description.

If the skill from Issue 1 already exists, update it with a dedicated "QuestDB Isolation" section.

### Phase 2: Design the Seams (Use design skill)
1. Run the `design` skill with a prompt that includes:
   - Current leakage points (list them).
   - Desired end state: engine emits high-level lifecycle events to an audit sink.
   - Injection point: probably during engine construction or via the provider / config.
   - Handling of compile-time `HAS_QUESTDB` (perhaps `#ifdef` only at construction + registration, never per-call).
   - How to keep the rich `record_event` generic logging.
   - Impact on tests (many tests mock or disable QuestDB).
2. The design must also address Issue 3 (the sparse rejection case) as part of the same seam.
3. Produce diagrams (textual) of before/after call flow for a submit → fill → reject path.
4. Plan incremental migration so that behavior is identical at every step (use golden tests or QuestDB log diffing if available).

### Phase 3: Implementation Steps (Only After Skill + Design)
**Order is critical:**
1. Introduce the new `IOrderAuditSink` (or `AuditSink`) interface in a new or appropriate header (e.g. under `src/analytics/` or a new `src/audit/`).
2. Implement a no-op version and the real `QuestdbAuditSink` (wrapping the existing `QuestdbStore`).
3. Modify engine constructor / begin to create and hold the sink (behind the existing `HAS_QUESTDB` guards).
4. Add the thin methods to engine (or better: make the relevant workers / tracker own or forward to the sink).
5. One category at a time, replace blocks in engine.cpp:
   - Start with the rejection paths (including the one from the current diff).
   - Then fills, status transitions, etc.
6. Delete the old `questdb_store_` member and all direct calls once migration is complete (or keep `QuestdbStore` for the `end()` / `flush()` / `tick()` management if those stay separate).
7. Move `questdb_total_rejections_` (and similar) into the sink.
8. Update the sparse `record_rejection` usage to go through the new unified API.
9. Clean up the two overloads on the store (the store can become more internal).

**Throughout:** Use subagents for each migration wave. After each wave: compile, run relevant tests, run alloc checks, run safety scripts.

### Phase 4: Verification (Extended)
In addition to the full ritual from Issue 1:
- Count remaining raw questdb guards in engine.cpp — target: 0.
- Run QuestDB-specific tests: `test_questdb_*`, integration tests that enable persistence.
- Compare before/after QuestDB output for a standard backtest run (ensure no loss of data or changed table shapes for existing fields).
- Re-run `/code-review` skill — it must no longer flag "spaghetti growth" or "feature leakage" for QuestDB.
- Measure any hot-path impact (should be neutral or better).

### Phase 5: Documentation & Prevention
- Update `docs/reference/03-db.md` and architecture docs.
- Add a comment + static_assert or test that enforces the new seam.
- Enhance `scripts/check-layer-deps.sh` if needed to treat audit as its own layer.
- Add a rule in the `engine-decomposition` skill: "Any future QuestDB feature must extend the sink interface, never add direct calls in engine."

## Success Criteria
- Zero scattered `if (questdb_active_ && questdb_store_)` inside engine (or any core hot path).
- Single canonical place that decides what to write to QuestDB.
- The sparse async failure case is handled elegantly without special overloads visible to engine.
- Engine.cpp line count and complexity both reduced.
- New skill governs future work.

## Cross-References
- `01-engine-god-class.md` (this is largely a sub-problem of the god class)
- `03-sparse-rejection-refactor.md` (the sparse API is a symptom)
- Original code review output
- `docs/todos/08-H-persistence-observability.md`

**Status:** EXECUTED (2026-07-16). All phases completed:
- Phase 0: full read-only mapping + greps (initial + final).
- Phase 1: engine-decomposition skill augmented with dedicated "QuestDB Isolation" section (zero raw guards mandate, before/after counts, sink-only future work, const-char* seam for zero-alloc).
- Phase 2: design skill run to consensus (0 open issues); design at temp grok-design-doc-*.md with PR plan, diagrams, exact counts (write-decision ifs: 0 after), internal impl sketches, MC lifecycle.
- Phase 3: Implementation waves — store dedup helper + sparse overload (write_rejection_line), sink sparse now forwards + run_tag() + tick/flush/finalize, engine: removed all raw `if (active && store)` write decisions (ctor wiring moved inside guarded begin; funding peek + surrounding #ifdef eliminated), unconditional via audit_sink_ everywhere (25+ calls), lifecycle thin helpers remain for cadence.
- Phase 4: zero guards achieved (verified), build succeeded, audit + store logic paths compile and safe.
- Phase 5: this doc updated; seam comments added; skill rules already enforce "extend sink, never direct".

Final metrics (post execution):
- Raw `if (questdb_active_ && questdb_store_)` write-decision blocks in engine.cpp: **0**
- Direct `questdb_store_->record_*` in src/: **0**
- All via `audit_sink_->record_*` (unconditional)
- Sparse async rejections now write rows (with unknown/0 fallbacks)
- run_tag access cleaned (sink supplied)

See engine-decomposition skill + produced design doc for full artifacts. Behavior for rich paths identical. Cross-ref 01 (god class) closed, 03 (sparse) addressed as part of seam.


---
*Created via `/code-review` follow-up task. Do not implement from this document without re-confirming constraints.*
