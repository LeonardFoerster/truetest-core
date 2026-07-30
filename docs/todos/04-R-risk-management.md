# R: Risk Management (Highest remaining technical risk per gaps.md)

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. See prod.md Go-Live + docs/reference/01-instructions.md .

- **R-01** Implement proper position-based risk limits (max % of equity, volatility-adjusted) in `RiskManager` / `FuturesRiskCheck`.
- **R-02** Complete accurate per-position mark-price liquidation calculator using real tiered Binance maintenance margins + funding. (Current: `MaintenanceMarginTable` + `/fapi/v1/leverageBracket` landed for rates; liq distance in `FuturesRiskCheck` remains approximation `cash / notional − maintenance_margin`; "Approx" comments present.)
- **R-03** Wire funding rate events from user-data stream into `Portfolio`, `shadow_tracker`, P&L, analytics, risk checks, and QuestDB. (Fees/FUNDING_FEE events fully wired end-to-end: portfolio `on_funding`, analytics, QuestDB `record_funding`, engine publish; 8h rate setter + circuit breaker fields exist in risk_manager/analytics but **zero callsites** — always 0.)
- **R-04** Add cross-margin / multi-symbol / account-level margin ratio monitoring and exposure limits. (Engine largely single-symbol; cross-margin coarse proxy noted as out-of-scope.)
- **R-05** Add configurable extreme-event circuit breakers (spread widening, funding spikes, exchange anomalies). (Phase 2.4 partial: spread/funding rate breakers in risk_manager via Analytics snapshot; "can be fed" comments.)

Additional (Go-Live Gate row 3): Funding + tiered MMR exercised for ≥30 days. Position-based pre-trade risk must land before futures live path vs. real money (existing RiskManager is balance-based/cash; venue notional/leverage/liq caps are backlog).

**Last updated**: 2026-07-03 (split from governance/03-todo.md per TODOS-SPLIT-SPEC; verbatim extraction; see 00-OVERVIEW.md).
