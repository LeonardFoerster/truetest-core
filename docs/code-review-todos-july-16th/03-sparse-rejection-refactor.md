# Issue 3 — Sparse Rejection Recording & Missed Code-Judo Opportunity

**Severity:** suggestion  
**Date:** 2026-07-16 (from `/code-review`)  
**Files Touched in Diff:** `src/data/questdb/store.h`, `src/data/questdb/store.cpp`, `src/engine/engine.cpp`

## Context from Review
The change under review added:

```cpp
// New overload
virtual void record_rejection(uint64_t order_id, const std::string& symbol, ...);

// Call site update
questdb_store_->record_status_transition(...);
questdb_store_->record_rejection(sr.engine_id, sr.symbol, "transport_error", sr.error);
questdb_total_rejections_++;
```

Previously the rejection path used the rich `const order_event&` version.

Inside `store.cpp`, the two implementations are nearly duplicate (same LineBuilder construction, only data source and timestamp differ).

This is a classic symptom of "narrow edge-case handling implemented in the middle of an already busy function" and "bespoke helpers".

Per skill:
- "this works, but it makes the surrounding code more spaghetti."
- "Prefer the solution that makes the code feel inevitable in hindsight."
- "If you see a path to delete complexity rather than rearrange it, push hard for that path."

## Root Causes
1. Async submit results (`ExecutionBridge::submit_result`) carry only sparse data (id, symbol, error, ok).
2. The order_event may have been moved or is not easily reconstructible at drain time.
3. No unified "rejection context" model that both rich events and sparse failures can populate.
4. The `record_*` interface on `QuestdbStore` was grown by adding overloads instead of rethinking the model.
5. Callers (engine) are responsible for knowing when to use which overload.

## Relationship to Other Issues
- Directly caused by (and worsens) Issue 2 (leakage).
- Part of the god-class problem in Issue 1.
- A good fix here can be a small "code judo" win that also demonstrates the larger seam from Issue 2.

## Step-by-Step Guide for Grok Build (Planning Document — Do Not Implement)

### Phase 0: Analysis (Read-Only)
1. Read the exact diff that introduced the change.
2. Read the two `record_rejection` implementations side-by-side (store.cpp ~294-340).
3. Read the call site in `drain_async_submit_results` (around line 2850) and compare to other rejection sites:
   - Risk reject paths (~2000 area)
   - Venue filter (~2610)
   - Normal order flow
4. Find all places that construct `rejection_event` or call `record_rejection`.
5. Examine `ExecutionBridge::submit_result` struct (in `execution_bridge.h`).
6. Look for existing "context" or "meta" structs in engine (there is already `order_meta_` and `lookup_strategy_name`).
7. Grep for other "sparse" patterns or "unknown" fallbacks.

### Phase 1: Identify the Judo Move
The ambitious reframing:
- Introduce a small value type `RejectionContext` (or reuse/extend `rejection_event` more cleverly).
- Or, better: make `record_rejection` take a lightweight identity + optional rich data.
- Or (best if Issue 2 seam is built): the audit sink's `on_rejection(uint64_t id, std::string symbol, std::string category, std::string detail)` is the only API. Internally the sink decides tags/fields. Rich `order_event` paths populate extra fields before calling the sink.

Goal: delete the second overload entirely, or make the store's public surface smaller.

Alternative judo: enhance the async result path to carry (or look up) more context so the rich path can always be used.

### Phase 2: Detailed Resolution Steps
1. **After** the skill from Issue 1/2 exists and the audit sink design is done:
   - Define a minimal struct in an appropriate place (perhaps next to events or inside the audit header):
     ```cpp
     struct RejectionInfo {
         uint64_t order_id;
         std::string symbol;
         std::string category;
         std::string detail;
         // optional: side, strategy, qty, price, timestamp if known
     };
     ```
2. Change the store (or the new sink) to have one primary method:
   `record_rejection(const RejectionInfo& info);` (or take the struct + optional event).
3. Update the rich path: populate a `RejectionInfo` from the `order_event` + reason and call the unified method.
4. Update the sparse async path to populate the same struct (with unknowns/zeros) and call the same method.
5. Remove the second overload from the public interface (or deprecate internally).
6. Remove the near-duplicate code in the implementation (one builder path).
7. In engine, simplify the call site in the diff area — it should now look similar to other rejection recordings.
8. Update any tests that directly exercise the store.

### Phase 3: Verification Specific to This Change
- The async transport error rejection must still appear correctly in QuestDB (symbol present, order_id, reason, zeros for qty/price).
- No behavior change for rich rejections.
- Run full rejection-related tests: `test_exit_manager`, `test_risk_manager`, `test_engine_brackets`, binance futures safety tests, etc.
- Re-inspect the diff area — it should be shorter and less special-cased.
- Re-run `/code-review` — this issue should no longer be called out.

### Alternative Simpler Paths (If Full Sink Is Deferred)
If full Issue 2 refactor is later:
- At minimum, extract a private helper in `QuestdbStore`:
  `void write_rejection_line(const std::string& symbol, const std::string& side, const std::string& strategy, uint64_t order_id, double qty, double price, ...);`
- Both public overloads delegate to it.
- This at least removes duplication even if the interface stays.

But the skill should push for the deeper judo when possible.

## Success Criteria
- Only one `record_rejection` shape visible to engine / callers.
- No duplicated LineBuilder logic.
- The async failure path no longer feels like a special case bolted on.
- Ideally, this small change is the first demonstration of the new audit seam.

## References
- `02-questdb-persistence-leakage.md`
- `01-engine-god-class.md`
- `src/data/questdb/store.cpp` (the duplicate functions)
- Execution bridge submit result handling

**Status:** Pure planning artifact. Do not begin changes.

---
*Part of the July 16, 2026 code-review todo set.*
