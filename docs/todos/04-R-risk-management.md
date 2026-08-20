# R: Risk Management (Highest remaining technical risk per gaps.md)

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. See prod.md Go-Live + docs/reference/01-instructions.md .

- **R-01** Implement proper position-based risk limits (max % of equity, volatility-adjusted) in `RiskManager` / `FuturesRiskCheck`.
- **R-02** Complete accurate per-position mark-price liquidation calculator using real tiered Binance maintenance margins + funding. (Current: `MaintenanceMarginTable` + `/fapi/v1/leverageBracket` landed for rates; liq distance in `FuturesRiskCheck` remains approximation `cash / notional − maintenance_margin`; "Approx" comments present.)
- **R-03** Wire funding rate events from user-data stream into `Portfolio`, `shadow_tracker`, P&L, analytics, risk checks, and QuestDB. (Fees/FUNDING_FEE events fully wired end-to-end: portfolio `on_funding`, analytics, QuestDB `record_funding`, engine publish. **R3 closed the rate gap**: `Analytics::on_funding` now derives the realized 8h rate from the settlement itself (`rate = -cash_delta / signed_position_notional`) and flags it via `risk_snapshot::funding_rate_known`, so `--max-funding-8h-rate` actually binds. A dedicated venue funding-rate feed can still override it through `set_current_funding_rate_8h`; that feed is the remaining open item.)
- **R-04** Add cross-margin / multi-symbol / account-level margin ratio monitoring and exposure limits. (**R3 landed the multi-symbol exposure half**: `portfolio_risk_view` aggregates mark-to-market gross/net/worst-case notional across every instrument and `max_portfolio_exposure` enforces against it. Account-level cross-margin *ratio* remains deliberately unimplemented — R3 introduced no universal margin formula and makes no cross-margin safety claim; see the R3 note §8.)
- **R-05** Add configurable extreme-event circuit breakers (spread widening, funding spikes, exchange anomalies). (Phase 2.4 partial: spread/funding rate breakers in risk_manager via Analytics snapshot; "can be fed" comments.)

Additional (Go-Live Gate row 3): Funding + tiered MMR exercised for ≥30 days. Position-based pre-trade risk must land before futures live path vs. real money (existing RiskManager is balance-based/cash; venue notional/leverage/liq caps are backlog).

**R3 — Authoritative risk accounting** (`docs/internal/r3-authoritative-risk-accounting.md`): open orders, remaining quantity and per-symbol pending exposure now come from the `OrderTracker` ledger, never from analytics counters; position exposure is mark-to-market with mark-age classification; candidate orders are checked against current + pending worst case; inventory-increasing vs risk-reducing is explicit; `max_portfolio_var_estimate` was removed as a false safety claim.

**Last updated**: 2026-08-20 (R-03/R-04 status refreshed by R3; original split from governance/03-todo.md per TODOS-SPLIT-SPEC — see 00-OVERVIEW.md).
