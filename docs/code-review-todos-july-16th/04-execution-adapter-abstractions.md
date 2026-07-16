# Issue 4 — Weak Execution Adapter Abstractions (Dynamic Cast Proliferation)

**Severity:** suggestion  
**Date:** 2026-07-16 (from `/code-review`)  
**Main File:** `src/engine/engine.cpp` (multiple `dynamic_cast` sites) + headers under `src/execution/` and `src/providers/`

## Context from Review
Grep for `dynamic_cast<` in engine.cpp revealed ~10 occurrences used for runtime type discrimination:

- `ExecutionBridge*`
- `TradeTapeShadowAdapter*`
- `LocalBookAdapter*`

Examples:
- In construction / wiring
- `drain_async_submit_results`
- Submit path to decide `async_submit`
- Cancel handling
- Local adapter special cases for mid price, etc.

The reviewed async drain change lands inside one of these cast-heavy functions.

This matches review rules:
- "Push hard on type and boundary cleanliness"
- "Prefer explicit typed models or shared contracts over loosely-shaped ad-hoc objects"
- "Thin abstractions, identity wrappers..."
- "Is this abstraction actually earning its keep, or is it just a wrapper?"

## Root Causes
1. `IExecutionAdapter` is too coarse.
2. Different providers / modes (live binance via bridge, local simulation, shadow adapters) have qualitatively different capabilities (async submit results, synth meta, queue awareness, etc.).
3. Instead of capability interfaces or visitor / double dispatch, engine does ad-hoc `dynamic_cast` to discover capabilities.
4. `ExecutionBridge` itself appears to be a composite that adds async + other behavior on top of a base adapter.

## Why Not as Urgent as 1 & 2, But Still Structural
It contributes to the god-class problem. Every time a new adapter capability appears, another cast or special `if` appears in engine. It is "spaghetti growth" by another name.

A good fix can often be done with small, well-typed interfaces without massive surgery.

## Step-by-Step Guide for Grok Build

### Phase 0: Inventory (Read-Only)
1. Run targeted greps:
   - `dynamic_cast<` across the whole `src/` (not just engine)
   - `IExecutionAdapter`, `get_execution_adapter`
   - `ExecutionBridge`, `LocalBookAdapter`, `TradeTapeShadowAdapter`
2. Read the interface definition: `src/execution/execution_adapter.h`
3. Read `src/execution/execution_bridge.h` (and its .cpp if separate)
4. Read usages in providers (binance, local, synthetic).
5. Read the engine sites that cast (at least 6-8 distinct places).
6. Understand the difference between:
   - Normal submit path
   - Async bridge poll path (`poll_submit_results`, `poll_synth_meta`)
   - Local vs remote book behavior
7. Check whether `queue_aware_book_adapter`, `trade_tape_shadow_adapter` etc. already use better patterns.

### Phase 1: Design Better Boundaries
Recommended approach (choose one or hybrid after analysis):
A. **Capability Interfaces** (preferred for C++):
   - `IAsyncSubmitSupport` (or `IAsyncExecution`)
   - `ISynthMetaProvider`
   - `ILocalBookControl`
   - etc.
   - Adapters implement the capabilities they support.
   - Engine does `if (auto* async = dynamic_cast<IAsync...>(adapter.get()))` — still a cast, but now **explicit and narrow**, and only where the capability is used.
   - Better: use a registration or `std::any` / type-erased holder, or just query methods that return optional.

B. **Visitor / Command pattern** on the adapter for operations that differ.

C. **Stronger base + optional methods** returning `std::optional` or null objects (but be careful with hot path cost).

D. Move more of the special logic **into** the bridge / adapters themselves so engine stays oblivious.

The design doc (use `design` skill) must evaluate hot-path cost of each option (zero alloc, no virtual in inner loops where possible).

### Phase 2: Concrete Resolution Steps
1. Define the new narrow capability interfaces in `src/execution/` (or a subdir).
2. Have `ExecutionBridge` (and any other) implement the relevant capability interfaces.
3. Update the places that currently cast to `ExecutionBridge` specifically to cast to (or query) the capability interface instead.
4. For the async drain function: take or obtain the capability interface rather than the full bridge.
5. For the "is async submit?" decision: prefer asking the adapter "bool supports_async_submits() const" or obtaining the capability once at wiring time and storing a separate pointer / flag.
6. Repeat for LocalBookAdapter special cases (mid price seeding, etc.).
7. Remove or reduce the number of raw `dynamic_cast<ConcreteType>` to only the new capability interfaces.
8. If possible, avoid casts entirely at runtime by storing capability pointers at construction time in engine (typed members like `IAsyncSubmitSupport* async_support_ = nullptr;`).

### Phase 3: Migration & Cleanup
- Update all providers that supply adapters to also expose the capabilities.
- Update tests that construct mock adapters (`tests/test_helpers/mock_transport.h` and bridge tests).
- Clean any now-unused `dynamic_cast<ExecutionBridge*>` that were only for async features.

### Phase 4: Verification
- All adapter-related tests pass (`test_execution_bridge`, `test_execution_adapter`, provider tests, engine integration).
- No increase in allocations (measure with alloc_counter tests).
- Re-grep shows reduction in problematic casts.
- The code in `drain_async_submit_results` (and submit path) is easier to read.
- `/code-review` skill run no longer flags the adapter boundary issues.
- Performance benchmarks (if any) in the area are unchanged.

### Optional Judo
If the design shows that most special behavior can live behind the bridge, consider making the async drain logic live **on** the bridge or a worker, and have engine only call a higher-level `adapter->drain_results(...)` that does the right thing internally. This would delete code from engine entirely.

## Success Criteria
- Engine no longer needs to know concrete adapter implementation names for normal operation.
- New capability interfaces are narrow, well-documented, and tested.
- Future adapter features require only extending the capability set, not engine changes.
- Measurable reduction in complexity / cast count in engine.cpp.

## References
- `01-engine-god-class.md`
- `src/execution/execution_adapter.h`
- `src/execution/execution_bridge.h`
- `src/execution/queue_aware_book_adapter.h`
- Provider implementations under `src/providers/`

**Status:** Planning only.

---
*July 16 2026 code-review todos.*
