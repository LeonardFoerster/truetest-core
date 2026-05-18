# Deepdive Refactor — Migration & File History

**Purpose**: Chronological record of every significant file touched, moved, refactored, or deleted during the "deepdive" architectural work. This document was created as part of Phase 1 of `prod.md`.

## How to Maintain This Document

- Every PR that touches core, risk, safety, threading, or live-provider code must add an entry here.
- Format: `YYYY-MM-DD | Short description | Files affected | PR/commit`
- Group by major phase when possible.

## Known Major Phases (High-Level)

### Pre-Deepdive Baseline (before 2026-05)
- Original engine with SQLite/PostgreSQL persistence, Boost.Beast WebSocket dashboard + React SPA (later removed).
- Single `engine` binary with runtime mode switching.
- Per-lot bookkeeping and ExitManager introduced during early deepdive.

### 2026-05 Phase 0–1 Window (current)
- Creation of Phase 0 Operator SOP and artifact tracking (`docs/futures-phase0-operator-sop.md`, `reports/phase0/`)
- Phase 1 freeze activation:
  - `prerequisites.md`, `todo.md` (root)
  - `docs/target-architecture.md`, `docs/migration.md`
  - `scripts/check-live-safety-freeze.sh`
  - LIVE-SAFETY SURFACE comment blocks applied to the 9 protected files
  - Updates to `CLAUDE.md` and `prod.md`

## Detailed Change Log

| Date       | Description                                      | Key Files / Areas Affected                          | Reference |
|------------|--------------------------------------------------|-----------------------------------------------------|-----------|
| 2026-05    | Phase 0 tiny-size mainnet SOP and tracking       | New: `docs/futures-phase0-operator-sop.md`, `reports/phase0/` | Phase 0 execution |
| 2026-05    | Phase 1 planning artifacts created               | `prerequisites.md`, `todo.md`, `docs/target-architecture.md`, `docs/migration.md` | This document |
| 2026-05    | Live-safety freeze markers applied               | 9 files listed in `prod.md` Phase 1 (tt_target.h, engine.cpp, binance futures safety headers, risk/*, etc.) | Phase 1 |
| 2026-05    | Phase 2.2 tiered MMR wiring completed + build fix | `src/risk/maintenance_margin_table.{h,cpp}`, `futures_risk_check.h`, `binance_futures_provider.h`, `CMakeLists.txt`, `event_log.h` (serialise + funding cases), analytics.cpp handling | Post-Phase1 deepdive stabilization |
| TBD        | (Add future entries here)                        |                                                     |           |

**Note**: This document is intentionally started minimal. It will be expanded as the deepdive continues and as people back-fill older changes from git history.

**Last updated**: 2026-05 (Phase 1 baseline)
