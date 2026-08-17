# Skill Proposal: scaffold-strategy

**Proposed name**: `scaffold-strategy`  
**Category**: Velocity / Code Generation Guardian  
**Priority**: Medium-High  
**Target scope**: Project  
**Status**: Planning document (2026-07-16)

---

## One-Sentence Description

A development velocity skill that safely scaffolds a new trading strategy (including registry registration, header/implementation skeleton, basic tests, Monte Carlo compatibility, golden test hooks if appropriate, and required documentation updates) while enforcing all project conventions and hot-path / safety rules.

---

## Why This Skill Is Needed

Adding a new strategy today requires touching many places with very specific patterns:

- Per-translation-unit registration with `REGISTER_STRATEGY` from `src/strategy/strategy_registry.h`
- New `.h` + `.cpp` following the existing strategy interface
- Proper use of indicators
- MC compatibility (`IStrategy::reset(uint64_t)` and explicit reuse support, with no persistent state across trials)
- Registration in `cmake/Sources.cmake`
- Tests in `tests/test_strategies.cpp` or dedicated file
- Sometimes golden regression data
- Updates to `--help`, `docs/reference/01-instructions.md`, possibly user-manual or adaptive-hybrid reference
- Strategy must not introduce hot-path allocations

This is repetitive and error-prone. A good scaffold skill removes boilerplate while preventing the common mistakes.

---

## When to Use

- "Add a new strategy called X"
- "Create a breakout / mean-reversion v2 / structure continuation variant"
- Any time a developer wants to experiment with a new idea expressed as a strategy

---

## Non-Negotiable Rules

1. **Registry via macro only** — never manual registration.
2. **MC safety**:
   - Implement `reset(uint64_t)` and return true from `supports_mc_trial_reuse()` only when reuse is fully supported.
   - No heap allocations that grow across trials.
   - Must be safe to run under Monte Carlo with `--monte-carlo`.
3. **Hot path**:
   - The `on_bar` / `on_tick` / decision path must respect zero-alloc discipline.
   - Use existing indicator library where possible.
4. **No live order placement** — strategies only generate intents; execution is handled elsewhere.
5. **Documentation** — at minimum update the strategy list in reference docs and CLI help context.
6. **Tests** — skeleton must include at least one basic unit test + note about MC/golden if applicable.
7. Must not touch any frozen safety surface files.

---

## Detailed Workflow

### Phase 0 — Requirements Gathering (Interactive)
- Ask for:
  - Strategy name (kebab + class name)
  - Brief description / edge it exploits
  - Whether it needs custom parameters / config
  - Whether it is bar-based, tick-based, or hybrid
  - Whether it should support multi-symbol
- Check that name does not collide with existing strategies.

### Phase 1 — Scaffold Generation
- Create `src/strategy/<name>.h` and `.cpp` with:
  - Correct includes
  - Class inheriting from the strategy base
  - `REGISTER_STRATEGY` macro usage in the strategy `.cpp`
  - Placeholder `on_*` methods with comments
  - `reset(uint64_t)` implementation and an explicit reuse-support decision
  - Proper use of `IIndicator` types where relevant
- Add the new source files to `cmake/Sources.cmake`; registration itself remains local to the strategy `.cpp`.
- Create or extend a test file with a minimal passing test.
- Generate a short internal design note in the file header.

### Phase 2 — Documentation & CLI
- Update places that list strategies (reference docs, possibly instructions).
- Add a minimal example config snippet if the strategy takes parameters.
- Update the manual strategy list in CLI help and the matching reference docs.

### Phase 3 — Verification
- Build succeeds.
- New strategy appears in help / registry.
- Basic test passes.
- Run at least one backtest + one small MC campaign against the new strategy.
- `quality` review on the new files (naming, I- prefix if applicable, comments).
- `testing` skill with focus on the new strategy.
- Confirm no hot-path allocation introduced (use alloc tests + manual review).

### Phase 4 — Optional Polish
- Offer to set up a golden test (if the strategy is deterministic enough).
- Suggest a first-pass parameter tuning session using MC.
- Do not add new strategies to the retired Adaptive Hybrid surface. If a future
  rebuild composes strategies, update its explicit contract only after that
  feature is restored.

---

## Integration With Other Skills

- Works very well together with `testing` (the skill can invoke focused tests).
- Can call `performance` if the strategy does anything non-trivial on the decision path.
- After scaffolding, `check-work` is highly recommended.
- Future `repo-doctor` can list "strategies without tests" or "strategies never exercised in MC".

---

## Success Criteria

- A developer can go from "idea for new strategy" to "first backtest + MC run + basic test green" in under 15 minutes of actual coding (mostly filling in logic).
- New strategies consistently follow the same structure.
- No more "I forgot to register it" or "it breaks under MC" bugs for newly added strategies.

---

## References

- Existing strategies under `src/strategy/` (especially simpler ones like `sma*`, `breakout*`)
- `src/strategy/strategy_registry.h`
- `tests/test_strategies.cpp`
- `docs/reference/01-instructions.md` (strategy section)
- `docs/reference/06-adaptive-hybrid-strategy.md`
- `docs/todos/07-A-adaptive-hybrid.md`
- `docs/todos/03-MC-simulation.md` (MC requirements for strategies)

---

## SaaS Future

In a SaaS world, users will want to:
- Upload or configure custom strategies
- Have them run safely in isolated jobs

This skill (or a future evolution) becomes the internal template that user-facing strategy scaffolding must be compatible with.

Important constraint to preserve:
- The core engine's strategy interface and MC contract must stay stable.
- User strategies in SaaS will likely be expressed in a safer DSL or sandboxed plugin system on top of the same interface.

The scaffold skill should document extension points that are safe for future "user strategy" layers.

---

*Good candidate for early implementation because it has high day-to-day ROI.*
