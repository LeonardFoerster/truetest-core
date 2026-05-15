# grok.md — Implementation Log: Dual-Portfolio Shadow P&L

This document records all changes made while implementing **Dual-Portfolio Shadow P&L** in the TrueTest engine. It serves as a clear audit trail of modifications for future reference.

---

## Overview

**Feature Goal**:  
In shadow mode, maintain two separate `Portfolio` + `Analytics` instances:
- **Sim Portfolio** (`portfolio_`): What the strategy believes happened.
- **Exchange Portfolio** (`exchange_portfolio_`): What would have happened against the real trade tape.

This enables proper economic comparison (P&L delta, Sharpe, drawdown, etc.) between simulated and real fills.

**Key Design Decisions**:
- Use `std::optional<>` for the exchange-side objects (only allocated in shadow mode).
- Real tape fills (`exchange_filled`) are routed to the exchange portfolio/analytics.
- Text reporting is added for non-TUI runs.
- Rich ncurses TUI support is prepared via `dashboard_snapshot` but implemented later.
- Existing `ShadowTracker` behavior is preserved.

---

## Phase 0 – Design Finalization

**Date**: During planning session

**Decisions Made**:
- Naming: `exchange_portfolio_` and `exchange_analytics_`
- QuestDB strategy: Use suffixed run tags (`my_run_exch`)
- TUI approach: Enhance existing panels (Overview, Risk, Positions) instead of a new tab
- Analytics duplication: Yes — create a second `Analytics` instance for proper metrics on the exchange view

**Files Modified**: None (design only)

---

## Phase 1.1 – Core Data Model

**Goal**: Add the second portfolio and analytics, initialized only in shadow mode.

### Changes

**File: `src/engine/engine.h`**
- Added two new member variables:
  ```cpp
  std::optional<portfolio> exchange_portfolio_;     // only used in shadow mode
  std::optional<Analytics> exchange_analytics_;     // only used in shadow mode
  ```

**File: `src/engine/engine.cpp`**
- In the `engine` constructor, added conditional initialization:
  ```cpp
  if (config_.mode == engine_mode::shadow)
  {
      exchange_portfolio_.emplace(config_.initial_balance);
      exchange_analytics_.emplace(...);
  }
  ```

**Verification**:
- Project builds successfully
- All Engine-related tests pass

---

## Phase 1.2 – Accessors + Fill Routing

**Goal**: Expose the exchange objects and route real tape fills into them.

### Changes

**File: `src/engine/engine.h`**
- Added public accessor methods:
  ```cpp
  const portfolio* get_exchange_portfolio() const;
  const Analytics* get_exchange_analytics() const;
  ```

**File: `src/engine/engine.cpp`**
- Implemented the two accessor methods (return `nullptr` when not in shadow mode).
- Routed real/exchange fills to the second portfolio and analytics in **three locations**:

  1. **Batch exchange fills polling** (inside `process_single_tick` or similar):
     - Added `exchange_portfolio_->on_fill(...)` for each `ef` in `exchange_fills`

  2. **Provider fills loop #1** (shadow mode path):
     - Added routing to `exchange_portfolio_` and `exchange_analytics_`

  3. **Provider fills loop #2** (second shadow mode path):
     - Added identical routing logic for symmetry

**Verification**:
- Build successful
- All 38 Engine-related tests pass

---

## Phase 2 – Text Reporting (Non-TUI)

**Goal**: Provide visible dual-portfolio output for users running without the rich TUI.

### Changes

**File: `src/engine/engine.cpp`**
- Added a new section inside `print_summary()` called **"Dual Portfolio Shadow Report (Text)"**:
  - Displays:
    - Sim Equity vs Exchange Equity
    - P&L Delta (absolute + percentage)
    - Sim Cash vs Exchange Cash
  - Only executes when `mode == shadow` and the exchange portfolio exists
  - Uses `last_mid_price_` for equity calculation

**Note**: This report is automatically skipped when the rich ncurses TUI is active (existing behavior in `main.inc`).

**Verification**:
- Build successful
- All Engine tests pass

---

## Current Status (as of latest commit)

| Phase | Status   | Description |
|-------|----------|-----------|
| 0     | Complete | Design decisions finalized |
| 1.1   | Complete | Core data model + initialization |
| 1.2   | Complete | Accessors + real fill routing |
| 2     | Complete | Basic text dual-portfolio report |

**What is working**:
- `exchange_portfolio_` and `exchange_analytics_` are created in shadow mode.
- Real tape fills are being fed into both.
- Accessors are available.
- A text report is printed at the end of non-TUI shadow runs.

**What is not yet done**:
- QuestDB dual-row persistence (Phase 3)
- `dashboard_snapshot` extension (Phase 4)
- Rich TUI panel enhancements (Phase 5)
- Full documentation updates

---

## Files Modified Summary

| File                              | Changes |
|-----------------------------------|--------|
| `src/engine/engine.h`             | Added `exchange_portfolio_`, `exchange_analytics_`, and accessors |
| `src/engine/engine.cpp`           | Constructor initialization, fill routing (3 locations), `print_summary()` report, accessor implementations |

---

## Future Phases (Planned)

- **Phase 3**: QuestDB persistence (second `runs_meta` row)
- **Phase 4**: Extend `dashboard_snapshot` for TUI consumption
- **Phase 5**: Enhance existing TUI panels (Overview, Risk, Positions)
- **Phase 6**: Comprehensive testing + documentation

---

*This document is maintained as part of the implementation of Dual-Portfolio Shadow P&L.*