# Trading Logic Audit Report

- **Scope**: `full`
- **Fix mode**: on
- **Raw findings**: 12
- **Confirmed**: 5
- **Fixed**: 1
- **Blocked (freeze/CCB)**: 2
- **Blocked (other)**: 2
- **Retest**: PASS

## Executive summary

Confirmed trading-logic issues that can affect money, positions, risk, fills, or safety. Freeze-surface items require human CCB (`LIVE_SAFETY_CCB_APPROVED`) and were not auto-edited.

## Confirmed findings

### pnl-analytics-single-global-position (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/analytics/analytics.cpp`
- **Issue**: Analytics maintains a single global inventory and mark (position_qty_/avg_entry_price_/last_close_). Multi-symbol fills net and close against that book, and every market/tick overwrites the sole mark, so equity, realized PnL, win-rate/Sharpe/drawdown, risk_view().equity, and MC final_equity are wrong whenever more than one symbol trades or prints. Portfolio already has correct per-symbol positions and marks_by_symbol equity; analytics does not.
- **Evidence**: analytics.h: position_qty_/avg_entry_price_/last_close_ are single scalars. analytics.cpp on_fill (~356): closes when position_qty_*side_sign<0 with no symbol match; on_market/on_tick set last_close_ from any event (~193,~272); equity/last_equity_/snapshot final_equity use cash_+position_qty_*last_close_ (~215-217,~290-292,~458-462,~574-576). per_symbol_ only tallies closed PnL after that global close (~408-414). portfolio.cpp apply_netted_fill uses positions_[fill.get_symbol()]; get_equity(marks_by_symbol) is multi-symbol-correct. monte_carlo_controller.cpp: result.final_equity=report.final_equity from analytics.snapshot(). risk_manager uses snap.equity for max_position_pct_of_equity. tests/test_analytics.cpp only uses symbol "X"; no multi-symbol analytics test. analytics.cpp not on live-safety freeze list.
- **Why critical**: Research reports, risk % equity caps, daily-loss inputs (last_trade_pnl), and Monte Carlo ranking all consume Analytics equity/PnL. Cross-symbol books silently invent PnL and can fail-open or over-tighten risk.
- **Suggested fix**: Not a small fix: replace global inventory with per-symbol state (qty, avg_entry, open_commission, entry_time) and per-symbol last marks; close only against same-symbol inventory; MTM as sum over symbols; add multi-symbol analytics regression tests. Prefer reusing portfolio netting semantics rather than inventing a third book.
- **Freeze surface**: no

### pnl-analytics-funding-skips-drawdown-peak (medium)

- **File**: `/home/leonard/work/projects/truetest/core/src/analytics/analytics.cpp`
- **Issue**: on_funding updates cash_/last_equity_/equity_curve_ but never peak_equity_/max_drawdown_ (only on_market does). risk_view/snapshot/max_drawdown_pct therefore lag or permanently understate funding-induced drawdown used by RiskManager halt and reports.
- **Evidence**: analytics.cpp on_funding (171-189): cash_+=delta, total_funding_pnl_+=delta, last_equity_=equity, record_equity_point only. peak_equity_/max_drawdown_ written solely in on_market (259-267) and reset. risk_view() sets r.max_drawdown=max_drawdown_*100; RiskManager::check_order/check_post_fill halt when snap.max_drawdown/100>=limits_.max_drawdown. generate_report/snapshot use max_drawdown_ state, not curve recompute. Tests FundingEvent_UpdatesCashAndEquityAndRiskView and FundingEventUpdatesAnalyticsRiskView assert equity/total_funding_pnl only.
- **Why critical**: Perpetual funding is a real cash P&L stream. Undercounted drawdown fails risk halts (max_drawdown) and misstates research risk metrics after funding-heavy sessions.
- **Suggested fix**: In Analytics::on_funding, after computing equity/last_equity_, mirror on_market peak/DD: if (equity > peak_equity_) peak_equity_ = equity; if (peak_equity_ > 0) max_drawdown_ = max(max_drawdown_, (peak_equity_-equity)/peak_equity_). Optionally set prev_equity_=equity so next bar return does not double-count funding. Add unit test: initial cash, optional market seed, large funding debit, assert max_drawdown_pct() and risk_view().max_drawdown without requiring a later market event.
- **Freeze surface**: no

### pretrade-min-liq-distance-pct-unit-mismatch (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/risk/futures_risk_check.h`
- **Issue**: min_liquidation_distance_pct is a fraction in code/tests/CLI help, but phase0 preset and operator docs supply whole-percent numbers (7, 1.5), making the pre-trade liq-distance gate ~100x stricter than intended and effectively blocking normal non-flat futures orders under the phase0 envelope.
- **Evidence**: futures_risk_check.h: distance = margin_base/post_notional - mm_rate; reject if distance < min_liquidation_distance_pct; comment '0.05 = 5%'; reject snprintf multiplies threshold by 100 (config 7.0 prints as 700%). tests/test_futures_risk_check.cpp uses 0.05 for 5%. CLI help: 'as a fraction (0.05 = 5%)'. main.inc apply_preset_with_precedence sets min_liquidation_distance_pct=7.0 for futures-phase0; pcfg passthrough has no /100. docs/governance/01-prod.md and ops/scripts use --min-liq-distance-pct 7; user-manual uses 1.5. With max_leverage 2.5, distance maxes near ~0.4 << 7.0.
- **Why critical**: Phase-0 live template either rejects all risk-increasing futures orders (operators disable the cap to trade) or trains operators to set nonsensical units, so the liquidation-distance safety net is not what the SOP claims.
- **Suggested fix**: Do not change freeze-file units. Align operator surface to the existing fraction contract: set futures-phase0 preset to 0.07 (not 7.0); change docs/scripts examples from --min-liq-distance-pct 7 / 1.5 to 0.07 / 0.015; optionally harden CLI help/flag name to discourage whole-percent values.
- **Freeze surface**: yes

### pretrade-max-open-orders-orders-minus-fills (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/risk/risk_manager.cpp`
- **Issue**: Pre-trade max_open_orders uses Analytics cumulative (total_orders - total_fills) as a proxy for resting orders. Multi-fill partials understate open count and can size_t-underflow the check so the limit never rejects; cancels/rejects never reduce the proxy (over-reject bias). True active count exists on OrderTracker but is not fed into risk_snapshot.
- **Evidence**: risk_manager.cpp:93 uses static_cast<int>(snap.total_orders - snap.total_fills) >= limits_.max_open_orders. analytics.cpp on_order ++total_orders_; on_fill ++total_fills_; cancel/amend/rejection fall through with break (no decrement). risk_view() copies total_orders_/total_fills_. fill_event::is_partial() and engine handle_engine_fill mark partially_filled while still counting each fill. MaxOpenOrders_Reject only asserts reject for snap(10,5). OrderTracker::active_count() already counts pending|open|partially_filled.
- **Why critical**: Operators believe resting-order inventory is capped; with partial fills (common on limits/live) the gate undercounts or fails open, allowing unbounded open orders and stack-up of working risk.
- **Suggested fix**: Plumb OrderTracker::active_count() (or equivalent open-order cache size) into risk_snapshot and compare that to max_open_orders; stop using total_orders-total_fills. Requires risk_snapshot/API change (risk_manager.h is freeze-listed) plus engine risk_view/check_order wiring and tests for partial fills, cancels, and fills>orders.
- **Freeze surface**: no

### pretrade-venue-caps-skip-when-l2-only-mark (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/engine/engine.cpp`
- **Issue**: Allow-on-mark<=0 is deliberate cold-start policy, but L2 is a first-class order path that never seeds last_mids_by_symbol_. Pure-L2 streams and depth-before-first-trade windows therefore permanently or transiently bypass FuturesRiskCheck notional/leverage/liq caps that mainnet live requires operators to set.
- **Evidence**: futures_risk_check.h: evaluate returns allow immediately if mark_price<=0 (IRiskCheck docs + test MarkZeroSkipsAllChecks). engine.cpp process_order: risk_check_->evaluate(*o, portfolio_, mark_for_symbol(o->get_symbol())) with comment mark==0 → check skips. engine.h mark_for_symbol returns 0.0 when symbol absent from last_mids_by_symbol_; note_mark_price only writes that map. note_mark_price call sites only on bar/tick/trade paths (~2624,2643,2756,3434,3452,3686,3838,3853,3894). apply_l2_snapshot/apply_l2_update never call note_mark_price; apply_l2_update drains pending + strategy on_l2_update → route_order → process_order. Test TickToTradeSafety.L2Dispatch_StampsRecvNsAndRoutesOrder submits on pure L2 without ticks. L2 exit path falls back to L2 price when last_mid==0; pre-trade mark does not. main.inc validate_futures_live_phase0 refuses mainnet live without venue caps.
- **Why critical**: Venue caps are the futures live Phase-0 envelope; any path that submits before a trade mark (or depth-only configs) silently disables max_notional/max_leverage/min_liq.
- **Suggested fix**: Freeze + CCB required. Small wiring: after L2 snapshot/update with valid BBO, note_mark_price(symbol, mid); optionally process_order fallback mark = order.get_price() when mark_for_symbol==0 and price>0. Add regression: pure L2 order with caps set must reject oversized notional. Do not flip allow-on-zero to refuse without design review.
- **Freeze surface**: yes

## Applied fixes

### pnl-analytics-funding-skips-drawdown-peak

on_funding now updates peak_equity_/max_drawdown_ (and prev_equity_) so funding cash P&L is reflected immediately in risk_view/max_drawdown_pct/snapshot without waiting for a market event.

Files: `src/analytics/analytics.cpp` `tests/test_analytics.cpp` 

Residual risk: on_tick still refreshes last_equity_ but does not update peak/max_drawdown (pre-existing, out of scope). Funding credits that raise a new peak are covered by the same peak path as on_market.

## Requires human CCB (freeze / live-safety)

- **pretrade-venue-caps-skip-when-l2-only-mark** (`/home/leonard/work/projects/truetest/core/src/engine/engine.cpp`): Allow-on-mark<=0 is deliberate cold-start policy, but L2 is a first-class order path that never seeds last_mids_by_symbol_. Pure-L2 streams and depth-before-first-trade windows therefore permanently or transiently bypass FuturesRiskCheck notional/leverage/liq caps that mainnet live requires operators to set.
  - Fix sketch: Freeze + CCB required. Small wiring: after L2 snapshot/update with valid BBO, note_mark_price(symbol, mid); optionally process_order fallback mark = order.get_price() when mark_for_symbol==0 and price>0. Add regression: pure L2 order with caps set must reject oversized notional. Do not flip allow-on-zero to refuse without design review.
- **pretrade-min-liq-distance-pct-unit-mismatch** (`/home/leonard/work/projects/truetest/core/src/risk/futures_risk_check.h`): min_liquidation_distance_pct is a fraction in code/tests/CLI help, but phase0 preset and operator docs supply whole-percent numbers (7, 1.5), making the pre-trade liq-distance gate ~100x stricter than intended and effectively blocking normal non-flat futures orders under the phase0 envelope.
  - Fix sketch: Do not change freeze-file units. Align operator surface to the existing fraction contract: set futures-phase0 preset to 0.07 (not 7.0); change docs/scripts examples from --min-liq-distance-pct 7 / 1.5 to 0.07 / 0.015; optionally harden CLI help/flag name to discourage whole-percent values.

Do not auto-merge. Follow `docs/governance/02-prerequisites.md` and T3 multi-agent protocol.

## Deferred (not auto-fixed)

- **pretrade-max-open-orders-orders-minus-fills**: Pre-trade max_open_orders uses Analytics cumulative (total_orders - total_fills) as a proxy for resting orders. Multi-fill partials understate open count and can size_t-underflow the check so the limit never rejects; cancels/rejects never reduce the proxy (over-reject bias). True active count exists on OrderTracker but is not fed into risk_snapshot.
- **pnl-analytics-single-global-position**: Analytics maintains a single global inventory and mark (position_qty_/avg_entry_price_/last_close_). Multi-symbol fills net and close against that book, and every market/tick overwrites the sole mark, so equity, realized PnL, win-rate/Sharpe/drawdown, risk_view().equity, and MC final_equity are wrong whenever more than one symbol trades or prints. Portfolio already has correct per-symbol positions and marks_by_symbol equity; analytics does not.

## Rejected after verification

- `pnl-portfolio-lot-flip-no-split`: apply_lot_fill is intentionally binary per opener_order_id for multi-lot/hedge attribution; apply_netted_fill alone owns flip split for cash/net position. Risk and equity use positions_, not lots_. Mirroring netted flip-split into lots would break concurrent opposite legs (hedge-demo). Claimed high position_accounting impact is not supported.
- `pnl-equity-missing-mark-zero-mtm`: Documented intentional incomplete-mark policy, not a ledger/risk/fill bug. get_equity(map) correctly implements cash + sum(qty*mark) and skips MTM when the caller omits a symbol, as portfolio.h states. Spot double-entry + complete marks preserves equity (tested for short open and multi-symbol). Engine last_mids_by_symbol_ feeds this path for reporting only; FuturesRiskCheck does not use get_equity(map). Short-inflation/long-deflation requires caller-supplied incomplete marks (GIGO), not wrong cash/position state. High severity overstated.
- `pnl-can-afford-sell-inventory-spot-only`: can_afford sell is inventory-only (symbol-less even cross-symbol), while on_fill intentionally opens shorts — but can_afford has zero production callers (only buy-side unit tests). Engine pre-trade uses RiskManager/FuturesRiskCheck, not can_afford. Dead helper asymmetry cannot affect money, positions, risk, fills, or safety today; medium reduce_only claim is overstated.
- `pnl-restore-state-drops-lots-and-funding`: restore_state/checkpoint omit lots_ and total_funding_pnl_ as claimed, but money/risk/fills use cash_ and positions_ which ARE restored. lots_ are dashboard/attribution only (open_lots_by_* unused; get_lots only UI); portfolio total_funding_pnl_ is unread externally and funding is already embedded in restored cash; analytics has a separate funding counter. Incomplete resume is a known H-03 design gap, not a netted-position or risk accounting error.
- `pretrade-leverage-liq-skip-nonpositive-margin`: Behavior is intentional and unit-tested: when mark equity margin_base=(cash+qty*mark)<=0, leverage/liq are skipped to avoid div-by-zero/undefined ratios; notional still enforces when enabled. LeverageCapZeroCashSkipsCheck documents refuse was declined. Treating this as a medium fail-open bug confuses deliberate design with a defect. A fail-closed margin_base<=0 reject would be a freeze-surface policy change, not a correctness fix. Same class already rejected as pretrade-futures-cash-le-zero-skip in the trading-logic audit.
- `pretrade-portfolio-exposure-entry-cost-not-mark`: RiskManager portfolio exposure is intentionally entry/cost-basis based and internally consistent with projected_notional (order price or avg entry). Using abs(pos.cost_basis) for other symbols is not a broken check or fail-open; it is the only multi-symbol notional available without a mark map, which risk_snapshot deliberately does not carry. No tests or docs define max_portfolio_exposure as mark gross; multi-symbol mark exposure is backlog (R-04). Dashboard mark display vs the same limit is a presentation inconsistency, not a pre-trade limit bypass. Default 5e9 further limits practical impact.
- `pretrade-max-position-pct-reused-as-portfolio-cap`: Confirmed same-field dual gate and unread VaR/vol scaffolding, but this is incomplete/conservative Phase 2.3 design, not a fail-open or mis-fill risk bug. Dual use is strictly more restrictive than per-name-only; default max_position_pct_of_equity=0 disables both gates; absolute max_portfolio_exposure already caps multi-name notional. Unused max_portfolio_var_estimate/realized_vol_1h are unfinished features (VaR≠notional%), not evidence of an active wrong check. Single-symbol test matches intended per-name behavior when equity is populated.

## Retest

- Verdict: **PASS**
- Summary: on_funding peak/max_drawdown/prev_equity fix verified; all three gates OK; build OK; 24/24 Analytics ctests passed including FundingDebit_UpdatesMaxDrawdownWithoutMarket.
- Commands:
  - `./scripts/check-hotpath-json.sh`
  - `./scripts/check-layer-deps.sh`
  - `./scripts/check-live-safety-freeze.sh`
  - `git diff -- src/analytics/analytics.cpp tests/test_analytics.cpp`
  - `cmake --build --preset linux-tests -j`
  - `ctest --test-dir out/build/linux-tests -R 'Analytics' --output-on-failure`

## Scan notes

- portfolio-pnl: Audited portfolio netted fills (long/short/flip/commission), lot bookkeeping, multi-symbol equity APIs, funding cash application, and Analytics trade/equity PnL. Single-symbol cash+MTM and commission-prorated flips in portfolio are sound; critical gap is Analytics’ single global position vs multi-symbol portfolio, plus lot flip desync and missing-mark short equity inflation.
- pretrade-risk: Pre-trade path: engine runs FuturesRiskCheck then RiskManager. Venue notional/leverage/liq math is coherent when mark>0 and margin_base>0, and mark equity for shorts is fixed. Remaining serious issues: min_liquidation_distance_pct unit mismatch (fraction in code/tests vs whole-percent phase0/docs so official live template always fails liq or trains wrong units), max_open_orders via total_orders-total_fills (partial fills undercount/underflow fail-open), L2 paths never note_mark_price (mark==0 skips all venue caps), leverage/liq skipped when margin_base<=0, multi-symbol portfolio exposure uses entry cost_basis not mark, and max_position_pct_of_equity is applied identically to single-name and portfolio totals.
- order-lifecycle: Order-lifecycle audit of tracker/bridge/adapters/router/engine drain and ExitManager opener fills. Live defects: REST cancel ACK and submit-fail untrack before WS lifecycle (race fills dropped / orphaned venue orders), WS terminal states silent to engine, unknown-fill remaining formula wrong. Research/live partial accounting: ExitManager binds only first opener slice into bracket qty; sync cancel marks cancelled before latency window; QueueAware also omits remaining_qty (related adapter/tracker divergence, not listed separately to stay within cap).
- fills-realism: Audited BACKTEST/SHADOW fill honesty across LocalBookAdapter, HybridExecutor (Binance/Bitget), QueueAwareBookAdapter, TradeTapeShadowAdapter, orderbook L2 apply, ExitManager on_bar, and fee/latency/impact models plus matching tests. Live paths still use venue bridges; sim fill models are not gated into live. Strongest issues: Hybrid mid reseeds wipe resting limits, L2 apply wipes user orders, bar-exit fire prices ignored by market fill path, fill-model probability drops limits instead of resting them, trailing on_bar looks ahead within the bar, and shadow tape always fees as taker.
- backtest-lookahead: Audited bar-path look-ahead and fill fidelity across engine run/process_single_bar, route_order (execution_bar_delay + latency), ExitManager on_bar/on_price, LocalBookAdapter market pricing, QueueAwareBookAdapter trade matching, and strategy on_market indicator updates. Default entry delay (fill next open) is implemented and pinned by test_engine_lookahead; remaining material bugs are exit/stop fill prices discarding trigger extremes, trailing-stop same-bar high-before-low, exit closes inheriting bar-delay, pending fills against a non-open-aligned book, and queue adapter missing submit_ts gating that TradeTapeShadowAdapter has.
- backtest-determinism: Audited MC campaign isolation (MonteCarloController path gen + per-trial engine/strategy/data_handler), synthetic provider seeding vs CLI --seed, engine::reset_for_next_trial completeness, strategy reset coverage under --mc-reuse-objects (defaulted by mc-robustness preset), MC realism knobs, and golden/MC tests. Canonical derive_mc_trial_seed + per-trial seed_used reporting are correct; fresh-engine serial MC with mean-reversion is deterministic. Live bugs: CLI seed never reaches SyntheticProvider path generation; strategy reuse leaves indicator/position state for strategies without IStrategy::reset; MC latency/impact flags are config/report-only no-ops; reset_for_next_trial leaves adapters/pending/MM seed incomplete for engine reuse.
- safety-paths: Safety-path audit of live_safety, tt_target, worker_watchdog, Binance/Bitget kill/DMS/reconciler, engine live shutdown, and related tests. Compile-time live gate, halt write-once exchange, watchdog single-shot, and futures HTTP fail-closed look correct. Six logical issues: spot kill parse fail-open, DMS disarmed before kill-switch, spot reconciler ignores exchange inventory when local flat, Binance DMS close_fn re-fire, Bitget empty size treated as flat, and live NoopKillSwitch + post-fail WARNING with no re-arm.
- provider-contracts: Audited provider event contracts (provider_event, data_bridge, Binance/Bitget market+user-data parsers, depth, synthetic). Side/ms mapping OK; Bitget closed-bar gate OK. Found: Binance kline ignores closed flag; shadow on_trade uses 1e8-scaled tape vs coin orders; depthUpdate applied as full snapshot (clears book) without U/u gap handling; synthetic CSV default precision breaks seed reproducibility; engine drops L2 venue timestamps.
- exits-brackets: Exit/bracket audit of ExitManager, venue adapters, engine net-flat sweep, and position_sizing. Trailing direction, bar SL-precedence, and basic long/short SL comparisons look correct. Material bugs are partial-opener arming, scale-out lifecycle (net-flat sweep + venue cancel siblings), fire-before-ack disarm, zero-qty rehydrate, and equity base for sizing.
- engine-wiring: Engine-wiring audit of composition root: risk_check_ is wired from provider and runs before RiskManager; shadow dual-track (LocalBook primary + TradeTape exchange portfolio) is intentional; live gate is TT_TARGET + CLI. Real issues: MC strategy reuse without reset() on several strategies, batch run()/run_tick_data missing advance_all (cancel latency dead), ctor discarding seeded orderbook, incomplete reset_for_next_trial, and on_fill dispatch requiring strategy_name.

## How to re-run

```text
/workflow trading-logic-audit
/workflow trading-logic-audit {"scope":"risk","fix":false}
/workflow trading-logic-audit {"scope":"full","fix":true,"max_fix":3}
```
