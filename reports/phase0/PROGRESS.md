# Phase 0 Qualifying Sessions – Master Tracker

**Target**: 15+ qualifying tiny-size mainnet BTCUSDT (and other symbols) futures sessions across ≥3 volatility regimes with **zero unexplained drift** and full artifacts + two-person sign-off.

**Qualifying rules** (from prod.md):
- Full command + conservative caps used (depth20, persist, DMS, reconcile ≤3 bps, risk caps, daily-loss/unwind).
- Post-halt mandatory grep for POSITION-SNAPSHOT / funding / drift clean.
- Artifacts: zstd event log, QuestDB run_tag, signed session note, volatility classification, PROGRESS row.
- Batch review every 5 sessions + two signatures on the batch.

| # | Date (UTC) | Symbol(s) | Regime (H/M/L) | Duration | Max Notional | Drift Result | Artifacts | Signed By | Notes / Volatility Classifier |
|---|------------|-----------|----------------|----------|--------------|--------------|-----------|-----------|-------------------------------|
| 1 |            |           |                |          |              |              |           |           |                               |
| 2 |            |           |                |          |              |              |           |           |                               |
| 3 |            |           |                |          |              |              |           |           |                               |
| 4 |            |           |                |          |              |              |           |           |                               |
| 5 |            |           |                |          |              |              |           |           | (Batch 1 review in ops/batch-001.md) |
| 6 |            |           |                |          |              |              |           |           |                               |
| 7 |            |           |                |          |              |              |           |           |                               |
| 8 |            |           |                |          |              |              |           |           |                               |
| 9 |            |           |                |          |              |              |           |           |                               |
|10 |            |           |                |          |              |              |           |           | (Batch 2) |
|11 |            |           |                |          |              |              |           |           |                               |
|12 |            |           |                |          |              |              |           |           |                               |
|13 |            |           |                |          |              |              |           |           |                               |
|14 |            |           |                |          |              |              |           |           |                               |
|15 |            |           |                |          |              |              |           |           | (Batch 3 – exit review) |

**Current count**: 0 / 15 qualifying (as of 2026; Phase 0 collection was paused during priority work on the monte-carlo branch — MC changes did not alter P0 gates or the live safety surface). Monte Carlo simulation capabilities (integrated from the monte-carlo branch) are available for research and strategy robustness (see root `todo.md` MC-* items and `docs/instructions.md`). The 15 qualifying sessions and Phase 0/1 gates defined in `prod.md` remain the requirements for any increase in live capital.

**Next actions**: See root `todo.md` (P0-01..P0-04 + MC-*; Phase 0 collection notes; MC does not relax P0 gates) and `prod.md` (exit criteria + full ritual). The volatility classifier and session scripts remain available. Phase 0 collection was paused during priority work on the monte-carlo branch (MC changes did not alter P0 gates or the live safety surface); 0/15 qualifying.

When a row is filled and signed, also record the corresponding `run_tag` in QuestDB and the binary log filename for auditability.