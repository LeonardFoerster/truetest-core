# Trading Logic Audit Report

- **Scope**: `full`
- **Fix mode**: on
- **Raw findings**: 12
- **Confirmed**: 8
- **Fixed**: 0
- **Blocked (freeze/CCB)**: 5
- **Blocked (other)**: 3
- **Retest**: N/A

## Executive summary

Confirmed trading-logic issues that can affect money, positions, risk, fills, or safety. Freeze-surface items require human CCB (`LIVE_SAFETY_CCB_APPROVED`) and were not auto-edited.

## Confirmed findings

### portfolio-pnl-multisymbol-single-mark (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/execution/portfolio.cpp`
- **Issue**: portfolio::get_equity(double) marks all open symbols with one price; engine maintains only last_mid_price_/last_mark_symbol_, so multi-symbol open books report wrong equity, final PnL, dashboard equity, and shadow equity delta. Does not corrupt cash, fills, or netted positions.
- **Evidence**: portfolio.cpp:217-226 loops positions_ and does equity += pos.qty * last_price with one price. engine.cpp:1132 final_equity=get_equity(last_mid_price_); engine.cpp:1281-1282 shadow sim/exch equity same; dashboard_snapshot_builder.cpp:197 out.equity=get_equity(last_mid_price_) while row.mark is gated by last_mark_symbol_ (211). test_portfolio MultiSymbol_* only cash/position_open; sole get_equity assert is single-symbol short at matching price. Example: AAPL 10@100 + GOOG 5@200, last_mid=200 → cash+3000 vs cash+2000.
- **Why critical**: Final equity, dashboard equity, shadow sim-vs-exchange equity, and any research ranking on multi-symbol runs are wrong money numbers.
- **Suggested fix**: Add get_equity(marks_by_symbol) (or mark lookup callback); maintain engine last_mids_[symbol] on market/tick; update QuestDB final_equity, shadow report, and dashboard equity to pass the map. Cover MultiSymbol equity in test_portfolio. Engine wiring is freeze-adjacent (engine.cpp).
- **Freeze surface**: no

### portfolio-pnl-analytics-single-position-book (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/analytics/analytics.cpp`
- **Issue**: Analytics inventory is a single net book: multi-symbol fills corrupt realized PnL, trade stats, equity MTM, and risk_snapshot fields derived from them; per_symbol_ does not hold inventory.
- **Evidence**: analytics.h: position_qty_/avg_entry_price_/cash_/last_close_ are scalars. analytics.cpp on_fill L345: close if position_qty_*side_sign<0 (no symbol); open path L424-426 blends avg_entry across any symbol; on_market L213-215 equity=cash_+position_qty_*last_close_; per_symbol_ L397-403 only attributes already-computed close pnl. portfolio.cpp L42 uses positions_[fill.get_symbol()]. test_analytics.cpp only symbol "X"; test_portfolio has MultiSymbol_* tests. RiskManager uses AnalyticsReport max_drawdown/last_trade_pnl/final_equity for halt/reject.
- **Why critical**: Research reports, win-rate, profit factor, max drawdown, and risk_view equity diverge from the true multi-symbol portfolio; strategies/risk decisions based on those metrics are misled.
- **Suggested fix**: Not a small fix: introduce per-symbol inventory (qty, avg_entry, open commission, entry_time) and per-symbol last marks; close only against same-symbol book; MTM sum over symbols. Mirror portfolio apply_netted_fill semantics; add multi-symbol analytics regression tests.
- **Freeze surface**: no

### portfolio-pnl-funding-not-on-engine-analytics (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/engine/engine.cpp`
- **Issue**: Funding events update portfolio_ cash (and audit) in publish_event but never call engine.analytics_.on_event; engine.analytics_ (risk_view, inline reports, QuestDB metric snapshot, dashboard analytics tails) omits funding PnL while portfolio equity includes it, desyncing risk limits and reports.
- **Evidence**: engine.cpp publish_event: if event_type::funding → portfolio_.on_funding(*fe); audit_sink_->record_funding(...); then if thread_preset::inline_mode return; // no analytics_.on_event. Comment: 'advisory for now; later will also feed risk_snapshot / RiskManager'. Contrast fill path: publish_event(fill_ptr); analytics_.on_event(fill_ptr); then risk_manager_.check_post_fill(..., analytics_.risk_view()). Pre-trade: auto snap = analytics_.risk_view(); risk_manager_.check_order(*o, portfolio_, snap) uses snap.equity for max_position_pct_of_equity. Analytics::on_event(event_type::funding) → on_funding updates cash_/total_funding_pnl_/equity_curve_/last_equity_ (comment: 'visible in reports, TUI sparkline, and risk_view()'); only unit test is Analytics isolation (test_analytics.cpp FundingEvent_UpdatesCashAndEquityAndRiskView), no engine integration test. Provider funding publishes via set_event_publisher → publish_event only. questdb_end: final_equity from portfolio_.get_equity (includes funding) but report metrics from analytics_.snapshot() (excludes funding). freeze: src/engine/engine.cpp on live-safety freeze list.
- **Why critical**: Live/shadow futures funding changes true cash but analytics equity, total_funding_pnl(), equity curve, and risk % equity limits stay pre-funding; operators and risk see wrong account value.
- **Suggested fix**: In engine::publish_event funding branch, after portfolio_.on_funding, also analytics_.on_event(ev) (and decide shadow exchange_analytics_ policy). Requires LIVE_SAFETY_CCB_APPROVED + freeze protocol because engine.cpp is freeze-listed; add engine-level test that funding changes analytics_.risk_view().equity / total_funding_pnl.
- **Freeze surface**: yes

### portfolio-pnl-short-cash-inflates-leverage (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/execution/portfolio.cpp`
- **Issue**: After short fills, portfolio cash embeds short proceeds (spot double-entry). FuturesRiskCheck leverage/liquidation-distance treat that cash as available margin, so operator max_leverage / min_liq_distance become optimistic post-short. Spot equity identity remains correct; notional caps unaffected; compute_quantity cash-sizing is not a live strategy path.
- **Evidence**: portfolio.cpp apply_netted_fill short open: cash_ += fill_qty*price - commission (lines 105-111); test SellWithNoPosition_OpensShort expects cash+=1000 and equity@mark unchanged. futures_risk_check.h: implied_leverage = post_notional / port.get_cash() and margin_ratio = cash/post_notional (lines 146-181). engine.cpp evaluates risk_check_->evaluate(*o, portfolio_, mark) before submit after fills update portfolio cash. Example: cash=10k, max_lev=5, short 40k then add 40k → check sees 80k/50k=1.6x allow while equity leverage is 8x. portfolio::compute_quantity only referenced from tests/test_portfolio.cpp.
- **Why critical**: After shorts, leverage caps fail-open and notional sizers oversize; common futures path can exceed intended risk.
- **Suggested fix**: Not a small non-design fix: either (1) freeze-surface change FuturesRiskCheck to margin base on equity (e.g. get_equity(mark) single-symbol) not get_cash(), with tests for post-short leverage, or (2) introduce futures wallet/margin accounting separate from spot cash proceeds. Do not delete short cash credit without rewriting get_equity—would break tested spot identity.
- **Freeze surface**: no

### pretrade-equity-pct-never-fires (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/risk/risk_manager.cpp`
- **Issue**: max_position_pct_of_equity pre-trade checks fail open on the engine hot path because risk_view().equity stays 0 (last_equity_ only written in on_funding/reset), so both single-symbol and portfolio equity-% gates are skipped for all non-funding-updated sessions; after funding they use stale equity.
- **Evidence**: risk_manager.cpp:107-111 and 129-134 gate on limits_.max_position_pct_of_equity > 0 && snap.equity > 0. analytics.h:176 sets r.equity = last_equity_. Grep of last_equity_ assignments: analytics.cpp:69 reset→0, analytics.cpp:182 on_funding only. on_market (analytics.cpp:213-246) computes equity and updates prev_equity_ but not last_equity_. Ctor seeds prev_equity_ with initial_cash but leaves last_equity_ at 0. engine.cpp:1539-1540 calls analytics_.risk_view() then risk_manager_.check_order. No tests exercise max_position_pct_of_equity; only FundingEvent_UpdatesCashAndEquityAndRiskView covers risk_view equity via funding.
- **Why critical**: Operators enabling max_position_pct_of_equity believe position size is equity-scaled; instead only absolute max_position_value (default 1e9) applies, allowing oversized entries relative to account equity in live and research.
- **Suggested fix**: In Analytics: set last_equity_ = initial_cash in ctor and reset; after computing equity in on_market (same formula as on_funding), set last_equity_ = equity. Optionally recompute last_equity_ on_tick when last_close_ updates if tick-only sessions need live MTM. Add a unit test: market bar → risk_view().equity > 0 and RiskManager rejects when projected notional exceeds equity * pct. Do not touch frozen risk_manager.h unless changing the limit struct.
- **Freeze surface**: yes

### pretrade-mark-price-symbol-mismatch (medium)

- **File**: `/home/leonard/work/projects/truetest/core/src/engine/engine.cpp`
- **Issue**: Venue pre-trade mark is a single process-wide last_mid_price_ not bound to the order symbol; multi-symbol interleaving can under/over-state FuturesRiskCheck notional/leverage/liq. Live v1 is single-symbol so impact is latent/medium, not broadly high.
- **Evidence**: engine.cpp ~1513: risk_check_->evaluate(*o, portfolio_, last_mid_price_.load(...)) with no symbol guard. engine.cpp ~2610-2611/2744-2745 overwrite last_mid_price_ and last_mark_symbol_ from the latest bar/tick. futures_risk_check.h ~43-49 contracts mark as 'for the symbol'; ~129-133 post_notional = |post_qty|*mark_price. dashboard_snapshot_builder.cpp ~211 uses mark only when sym == last_mark_symbol_. No test asserts engine mark/symbol coherence; venue risk tests are single-symbol.
- **Why critical**: If last mid is ETH~3k while ordering BTC, notional is understated by ~orders of magnitude → leverage and notional caps fail open and oversized futures risk can pass.
- **Suggested fix**: Freeze+CCB: at process_order, pass mark only if o->get_symbol()==last_mark_symbol_ else 0.0 (skip checks). Also set last_mark_symbol_ on every last_mid_price_ store site (~3423, ~3675, ~3827, ~3883). Proper fix is per-symbol last-mark map + IRiskCheck lookup (design, still freeze).
- **Freeze surface**: yes

### pretrade-futures-short-cash-inflates-leverage (high)

- **File**: `/home/leonard/work/projects/truetest/core/src/risk/futures_risk_check.h`
- **Issue**: FuturesRiskCheck treats portfolio.get_cash() as futures margin base, but portfolio cash uses spot settlement (short open credits full notional). That systematically understates implied leverage and overstates liquidation distance on subsequent risk-increasing shorts; mark equity (get_equity(mark) or cash+qty*mark) is the consistent single-symbol proxy.
- **Evidence**: futures_risk_check.h:146-181 uses cash=port.get_cash(); leverage=post_notional/cash; distance=cash/post_notional-mm. portfolio.cpp:105-111 short open does cash_ += fill_qty*fill_price - open_commission. After cash=10k short 1@30k: cash=40k, equity via get_equity(30k)=10k; next short to 2 BTC: code lev 60k/40k=1.5x vs equity lev 6x. Tests in test_futures_risk_check.cpp only with_state(cash,qty), no post-short-fill leverage case. File is on live-safety freeze list.
- **Why critical**: After any short, leverage and min-liquidation-distance caps become far too lenient, allowing stacked short risk that would exceed configured max leverage on a real futures wallet.
- **Suggested fix**: In FuturesRiskCheck::evaluate, replace cash margin base for leverage/liq with mark equity for the checked book (e.g. port.get_equity(mark_price), or cash + existing_qty*mark_price). Keep notional cap unchanged. Add regression: open short via on_fill then assert max_leverage rejects add-on short at true equity leverage. Freeze CCB + protocol required — not fixable_now.
- **Freeze surface**: yes

### pretrade-zero-price-notional-failopen (medium)

- **File**: `/home/leonard/work/projects/truetest/core/src/risk/risk_manager.cpp`
- **Issue**: RiskManager notional caps fail open on non-reducing orders when order price<=0 and there is no position avg entry to fall back on (common for market opens), because projected notional becomes 0 and never exceeds max_position_value / exposure / equity-% limits.
- **Evidence**: risk_manager.cpp valuation_price returns order.get_price() if >0 else position_price or 0; projected_position_notional multiplies |projected_qty|*price; check_order rejects only when projected_notional > caps (and equity-% analog). order_event defaults price=0.0. Engine runs risk_check_ only if non-null then always RiskManager. FuturesRiskCheck::evaluate skips when mark_price<=0; binance futures builds FuturesRiskCheck only if a cap >0. test_risk_manager.cpp only exercises priced limit orders; no RiskManager zero-price/market notional test.
- **Why critical**: Absolute position and portfolio exposure limits can be bypassed by market/stop-style orders that omit a positive limit price, especially without a venue FuturesRiskCheck.
- **Suggested fix**: In risk_manager.cpp check_order, after computing valuation price / projected qty: if !reducing_exposure and order qty material and valuation_price<=0, return risk_action::reject (fail closed when notional cannot be valued). Optional follow-up (freeze-touching): pass last_mid into RiskManager or risk_snapshot for market valuation instead of hard reject.
- **Freeze surface**: yes

## Requires human CCB (freeze / live-safety)

- **pretrade-futures-short-cash-inflates-leverage** (`/home/leonard/work/projects/truetest/core/src/risk/futures_risk_check.h`): FuturesRiskCheck treats portfolio.get_cash() as futures margin base, but portfolio cash uses spot settlement (short open credits full notional). That systematically understates implied leverage and overstates liquidation distance on subsequent risk-increasing shorts; mark equity (get_equity(mark) or cash+qty*mark) is the consistent single-symbol proxy.
  - Fix sketch: In FuturesRiskCheck::evaluate, replace cash margin base for leverage/liq with mark equity for the checked book (e.g. port.get_equity(mark_price), or cash + existing_qty*mark_price). Keep notional cap unchanged. Add regression: open short via on_fill then assert max_leverage rejects add-on short at true equity leverage. Freeze CCB + protocol required — not fixable_now.
- **portfolio-pnl-funding-not-on-engine-analytics** (`/home/leonard/work/projects/truetest/core/src/engine/engine.cpp`): Funding events update portfolio_ cash (and audit) in publish_event but never call engine.analytics_.on_event; engine.analytics_ (risk_view, inline reports, QuestDB metric snapshot, dashboard analytics tails) omits funding PnL while portfolio equity includes it, desyncing risk limits and reports.
  - Fix sketch: In engine::publish_event funding branch, after portfolio_.on_funding, also analytics_.on_event(ev) (and decide shadow exchange_analytics_ policy). Requires LIVE_SAFETY_CCB_APPROVED + freeze protocol because engine.cpp is freeze-listed; add engine-level test that funding changes analytics_.risk_view().equity / total_funding_pnl.
- **pretrade-equity-pct-never-fires** (`/home/leonard/work/projects/truetest/core/src/risk/risk_manager.cpp`): max_position_pct_of_equity pre-trade checks fail open on the engine hot path because risk_view().equity stays 0 (last_equity_ only written in on_funding/reset), so both single-symbol and portfolio equity-% gates are skipped for all non-funding-updated sessions; after funding they use stale equity.
  - Fix sketch: In Analytics: set last_equity_ = initial_cash in ctor and reset; after computing equity in on_market (same formula as on_funding), set last_equity_ = equity. Optionally recompute last_equity_ on_tick when last_close_ updates if tick-only sessions need live MTM. Add a unit test: market bar → risk_view().equity > 0 and RiskManager rejects when projected notional exceeds equity * pct. Do not touch frozen risk_manager.h unless changing the limit struct.
- **pretrade-zero-price-notional-failopen** (`/home/leonard/work/projects/truetest/core/src/risk/risk_manager.cpp`): RiskManager notional caps fail open on non-reducing orders when order price<=0 and there is no position avg entry to fall back on (common for market opens), because projected notional becomes 0 and never exceeds max_position_value / exposure / equity-% limits.
  - Fix sketch: In risk_manager.cpp check_order, after computing valuation price / projected qty: if !reducing_exposure and order qty material and valuation_price<=0, return risk_action::reject (fail closed when notional cannot be valued). Optional follow-up (freeze-touching): pass last_mid into RiskManager or risk_snapshot for market valuation instead of hard reject.
- **pretrade-mark-price-symbol-mismatch** (`/home/leonard/work/projects/truetest/core/src/engine/engine.cpp`): Venue pre-trade mark is a single process-wide last_mid_price_ not bound to the order symbol; multi-symbol interleaving can under/over-state FuturesRiskCheck notional/leverage/liq. Live v1 is single-symbol so impact is latent/medium, not broadly high.
  - Fix sketch: Freeze+CCB: at process_order, pass mark only if o->get_symbol()==last_mark_symbol_ else 0.0 (skip checks). Also set last_mark_symbol_ on every last_mid_price_ store site (~3423, ~3675, ~3827, ~3883). Proper fix is per-symbol last-mark map + IRiskCheck lookup (design, still freeze).

Do not auto-merge. Follow `docs/governance/02-prerequisites.md` and T3 multi-agent protocol.

## Deferred (not auto-fixed)

- **portfolio-pnl-short-cash-inflates-leverage**: After short fills, portfolio cash embeds short proceeds (spot double-entry). FuturesRiskCheck leverage/liquidation-distance treat that cash as available margin, so operator max_leverage / min_liq_distance become optimistic post-short. Spot equity identity remains correct; notional caps unaffected; compute_quantity cash-sizing is not a live strategy path.
- **portfolio-pnl-analytics-single-position-book**: Analytics inventory is a single net book: multi-symbol fills corrupt realized PnL, trade stats, equity MTM, and risk_snapshot fields derived from them; per_symbol_ does not hold inventory.
- **portfolio-pnl-multisymbol-single-mark**: portfolio::get_equity(double) marks all open symbols with one price; engine maintains only last_mid_price_/last_mark_symbol_, so multi-symbol open books report wrong equity, final PnL, dashboard equity, and shadow equity delta. Does not corrupt cash, fills, or netted positions.

## Rejected after verification

- `portfolio-pnl-short-cost-basis-commission-upnl`: Short cost_basis = -(notional+commission) is deliberate (inline comment) and test-locked; cash, get_equity, close cash, and analytics open-commission accounting are correct. qty*mark-cost_basis yielding +fee uPnL on short entry is a display/consumer formula mismatch, not a ledger, fill, risk-decision, or safety error. High severity is overstated.
- `portfolio-pnl-stale-risk-equity-after-fill`: Post-fill risk does not use snap.equity; it halts on last_trade_pnl (updated in on_fill before risk_view) and on max_drawdown. Claim wrongly asserts last_equity_ is set in on_market (only on_funding sets it). A fill near last_close does not invent new MTM drawdown beyond the last bar; large realized losses are covered by max_loss_per_trade/daily_loss. Residual one-bar DD lag on gap/slip or tick-only sampling is low-severity design lag, not a high-severity post-fill equity bug.
- `pretrade-futures-cash-le-zero-skip`: Behavior is intentional and unit-tested: leverage/liquidation require cash>0 as a margin base (avoid div-by-zero); notional still enforces; refuse was explicitly declined in the test comment. Framing this as a high-severity fail-open bug confuses deliberate design with a logical defect. A fail-closed cash<=0 reject would be a freeze-surface policy change, not a small bugfix.
- `pretrade-funding-breaker-never-fed`: Technical observation is correct (set_current_funding_rate_8h has zero callers; current_funding_8h_rate_ stays 0), but this is known incomplete Phase 2.4 scaffolding (R-03/R-05), not a high-severity live bug. max_funding_8h_rate defaults to 0 (disabled) and is never assigned by CLI/JSON/API, so the breaker branch never evaluates in product configs and cannot affect orders, fills, or money today. Settlement funding cash path is separate and works; only the rate circuit-breaker feed was never finished.

## Retest

- Verdict: **N/A**
- Summary: No fixes applied; retest skipped

## Scan notes

- portfolio-pnl: Audited portfolio netting (long/short/flip/commission), lot openers, funding cash, analytics realized PnL, equity/uPnL, and risk consumers. Cash round-trips for one symbol are coherent; critical gaps are multi-symbol mark-to-market, analytics single global position book, funding not wired into engine analytics, short cost-basis commission sign for uPnL, stale post-fill risk equity, and short proceeds inflating cash used as futures margin.
- pretrade-risk: Pre-trade risk path: engine correctly runs FuturesRiskCheck then RiskManager. Critical fail-opens: max_position_pct_of_equity never fires because Analytics::last_equity_ is only updated on funding (not market/tick/fill); FuturesRiskCheck skips leverage and liquidation when cash<=0; engine feeds a single global last_mid_price_ as mark without symbol match; funding circuit breaker is never populated by any provider; spot-style portfolio cash inflates after shorts so leverage/liq distance is optimistic; RiskManager treats zero-price opens as zero notional so position caps fail open.
- order-lifecycle: Order-lifecycle audit of tracker/bridge/adapters/engine drain paths. Strongest live defects: REST cancel success untracks before WS fills (drops concurrent fills), submit-fail untracks can orphan venue orders, and cumulative fills are last_qty-additive with no trade-id/venue-cum reconciliation. Research paths: QueueAware omits remaining_qty so partials look full; sync cancel marks cancelled before cancel-latency elapses; ExitManager only binds first opener partial into remaining/bracket qty.
- fills-realism: Audited fills-realism across LocalBookAdapter, QueueAwareBookAdapter, TradeTapeShadowAdapter, fee/impact/latency/queue models, engine bar/tick/stop wiring, and matching tests. Live path uses venue bridges (sim models bypassed). Largest issues: maker-queue paper path never fed trade tape and drops markets; shadow passive fills charged as taker; QueueAware exact-price matching misses trade-through; bar stops fill at close after high/low trigger; LocalBook fill_model discards resting limits.
- safety-paths: Audited live-safety freeze surfaces and tests: tt_target live-order gate, live_safety Noop defaults, WorkerWatchdog halt semantics, Binance/Bitget futures kill-switch/DMS/reconciler, spot kill-switch, engine live wiring (reconcile/kill/halt), and futures user-data liquidation handling. Compile-time live gate, futures reconciler fail-closed HTTP/parse, deadline-bounded kill switches, DMS arm-refuse, and write-once halt look correct. Findings center on DMS emergency-close without halt (ghost positions on HB recovery), spot kill parse fail-open, liquidation log-only, Binance DMS close re-fire, Noop live defaults, and loose -2011 cancel success matching.
- provider-contracts: Audited provider event contracts (provider_event, parsers, data_bridge, Binance/Bitget market+user-data, synthetic) against engine/adapter consumers. Core defect: venue market qty is 1e8-scaled int64 in parsers/orderbook, but shadow trade-tape and L2 queue adapters receive those values as if they were human coin units (dashboard unscales correctly). Also: depthUpdate wrongly applied as full snapshot; hybrid executors never forward on_trade; synthetic CSV uses default float precision.
- exits-brackets: Exit/bracket audit of ExitManager lifecycle, venue dual-path closes, openers_for net-flat sweep, and risk sizing inputs. Trailing ratchet and long/short SL/TP comparisons are directionally correct; critical money bugs are partial-opener under-arming, fire-before-submit naked positions, non-reduceOnly exit flips on duplicate venue+engine fills, openers_for miscount breaking phantom-bracket cleanup for scale-outs, and hardcoded/static equity for position sizing.
- engine-wiring: Engine-wiring audit of composition root: risk_check is wired from provider after open() and runs before RiskManager; live CLI gate (TT_TARGET + --live + credentials) holds; MC trials use fresh engines so shared-state isolation is OK for the current controller. Critical wiring failures found in hot-path risk snapshots under threaded presets (drawdown/equity), risk-worker portfolio cash default, shadow cancel not dual-pathed, and live Noop reconciler/kill-switch fail-open.

## How to re-run

```text
/workflow trading-logic-audit
/workflow trading-logic-audit {"scope":"risk","fix":false}
/workflow trading-logic-audit {"scope":"full","fix":true,"max_fix":3}
```
