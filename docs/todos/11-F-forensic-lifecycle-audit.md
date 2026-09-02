# F: Forensic Trade-Lifecycle Audit Fixes (ema-rsi-atr-pullback, 2026-08-22)

**See**: `docs/todos/00-OVERVIEW.md` for structure/anchors/maintenance conventions. This file tracks fixes for the defect list found by a runtime-trace forensic audit of the backtest trade lifecycle on 2026-08-22, `market_maker_refactor` branch. Every item was proven by an env-gated runtime trace of the unmodified execution path, an independent clean-room reference implementation of the strategy, and a purpose-built deterministic dataset — not inferred from code smell or architecture docs.

**Status (2026-08-22)**: F-01 … F-10b are **RESOLVED**. Workstream A and Workstream B landed together in one CCB cycle because the chosen F-01(a) policy (refuse-to-arm and flatten) requires an engine-side routing seam, which pulled `order_intent_processor.*` into F-01's scope. Verification evidence is at the bottom of this file.

**Companion**: `docs/todos/10-BF-backtest-engine-bugfixes.md` (2026-08-14 audit). F-02/F-03/F-06 share the order-lifecycle-attribution root cause that BF-13 addressed for fills; this audit showed the same gap existed for **terminal non-fill outcomes**. The `emit_terminal_transition` helper added for F-02 is the engine-owned transition point BF-02/BF-09 also want.

**Scope note**: `src/exits/**`, `src/analytics/**`, `src/execution/portfolio.*`, `src/execution/position_sizing.h`, `src/strategy/**`, `src/data/**`, `src/engine/instrument_spec_cache.cpp` and `src/risk/risk_manager.cpp` are **not** on the Phase 1 frozen surface. `src/bin/main.inc`, `src/engine/engine.cpp`, `src/engine/engine_market.cpp`, `src/engine/fill_processor.{h,cpp}`, `src/engine/order_intent_processor.{h,cpp}`, `src/engine/pending_order_scheduler.*`, `src/engine/engine_config.h`, `src/risk/risk_manager.h` and `src/execution/order_tracker.h` **are** frozen (`scripts/check-live-safety-freeze.sh` is authoritative).

---

## Severity ordering and why

Three defects were independently sufficient to invalidate a run. This was established by disarming each in isolation on the full 1,713,600-bar BTCUSDT dataset:

| Configuration | Trades | Win rate | Avg holding | Final equity | Outcome |
| --- | ---: | ---: | ---: | ---: | --- |
| Both traps armed (shipped default) | 4 | 0.0% | 0s | 6,892.80 | deadlock at bar 409 |
| F-02 disarmed only (3,000-bar slice) | 35 | 0.0% | 0s | 230.59 | account wiped, −97.7% |
| F-01 suppressed only (narrow spread) | 51 | 13.7% | 28m 08s | 7,529.69 | deadlock again at 30.56% DD |
| Both suppressed | 2,079 | 17.1% | 28m 33s | −101,678.92 | equity goes negative (F-05) |

**No single fix produced a usable backtest.** F-02 capped *coverage* and fired in every configuration where any rejection occurred. F-01 destroyed *validity* of whatever executed. F-05 destroyed *realism* and only became visible once the first two were cleared. See "Post-fix isolation matrix" below for the same configurations after the fixes.

---

## Workstream A — non-frozen fast track

- **F-01 — RESOLVED (2026-08-22); P0** Stop-losses fired at prices the market never traded at. 29 of 30 traced stop exits fired on a level lying entirely outside the triggering bar's high–low range, all via the `SL_GAP_AT_OPEN` branch, holding time 0s. Two independent mechanisms compounded in `ExitManager`:

  **(a) The entry-slippage shift was unbounded.** `on_fill` shifted SL/TP by `delta = fill_price - reference_entry`. The entry fills at the book's ask (~21 bps above the strategy's reference mid under default `--mm-spread-pct 0.002`), while the stop distance is `2 × ATR` (~10 bps on 1m BTC). With `|delta| >= stop_dist` the shifted stop landed *above* the market for a long and was already through its trigger the instant it was armed.

  **Fix**: `ExitManager::shift_entry_relative_levels` refuses the shift when `|delta| >= designed`, where `designed = |reference_entry - stop_loss|` measured on the intent as the strategy declared it. On refusal the opener is **not armed at all** — no sibling intent either — and a `flatten_request` is queued so the engine closes the lot at the current mark. One `stderr` line per opener, plus `slippage_disarms` / `flatten_requests` counters that reach the analytics report.

  *Decision (open question 1)*: refuse-to-arm-and-flatten, chosen over "keep the original stop". Keeping the original stop leaves a live position whose realized risk exceeds its budget; the breach condition means the entry should very likely not have been taken, so the honest response is to be out of it. This is the one Workstream A item that needed an engine-side seam: `OrderIntentProcessor::route_exit_flatten_requests` drains the queue at the top of both `evaluate_exits` overloads and routes each request as an opener-attributed market close anchored at the current mark, inside the same observation.

  **(b) Gap-at-open was applied to intents armed inside the current bar.** `on_bar` selected `fire_px = open` whenever `open <= sl` (long), treating it as a gap. For an intent armed *during* that same bar the bar cannot have gapped through a level that did not yet exist, and the exit filled at a price strictly *earlier* than the `low` that triggered it.

  **Fix**: `ExitManager::begin_evaluation_window()` opens one evaluation window per market observation; `OrderIntentProcessor::drain_due` calls it, which is the one call that precedes `evaluate_exits` on every market path (bar, tick, L2, streaming, batch). Openers armed inside the current window are recorded in `armed_this_window_`, and for those the SL/TP fire price is the level clamped into the bar's own range rather than the bar open. Deliberately **not** a timestamp comparison (F-08 makes bar timestamps the wrong clock and `fill_event` carries none).

  **Narrower than the original plan, deliberately**: the plan proposed skipping evaluation of such an intent entirely for that bar. That was implemented first and rejected — it broke `EngineBrackets.SlFiresOnIntraBarWickEvenWhenCloseRecovers` and `EngineLookahead.StrategyCallbackAfterIntrabarExitReceivesPostExitPortfolioCash`, both of which pin correct behaviour: a bar that wicks through the stop *after* the entry filled at its open must still stop out. Deferring the whole bar leaves the lot unprotected for exactly the bar it was opened in, which is a worse defect than the one being fixed. Only the *fill price* is deferred; protection is not.

  **Files**: `src/exits/exit_manager.{h,cpp}`, `src/engine/order_intent_processor.{h,cpp}` (frozen). **Regressions**: `ForensicLifecycle.F01a_*` (5), `ForensicLifecycle.F01b_*` (7) in `tests/test_forensic_lifecycle.cpp`, covering shorts, siblings, absolute structure stops, genuine gaps, and the no-engine-window fail-safe.

- **F-03 — RESOLVED (2026-08-22); P0** With `--exec-bar-delay 0` the strategy's stop-loss was never armed and the position ran unprotected for the rest of the run. With a synchronous fill the fill happens *inside* `route()` while `register_pending` runs afterwards in `finalize_route`, so the intent landed in `pending_` after the opener fill it was waiting for had already been consumed. Traced: 1 `ENTRY_ORDER`, 1 `FILL`, `FINALIZE status=3 (filled)`, **0 `ARM`**, 0 `EXIT_EVAL`.

  **Fix**: deferred-arm reconciliation inside `ExitManager`. When `on_fill` finds an opener id with neither a `pending_` nor an `armed_` entry it records the fill (qty, VWAP, ts, window) in a bounded `orphan_opener_fills_` map; `register_pending` consults that map first and arms immediately from the recorded fill. Arming is now order-independent rather than dependent on the engine calling in a particular sequence. Orphans expire after one full evaluation window and are additionally capacity-bounded, so a bracket-less opener can never accumulate and can never arm a later unrelated intent.

  **Live relevance**: `main.inc` forces `execution_bar_delay = 0` whenever `--mode live`/`--live` is set; any adapter reporting a fill synchronously on submit reproduces the original ordering. Not proven against a real venue adapter — **still flagged, not asserted** (see open question 4, unresolved).

  **Files**: `src/exits/exit_manager.{h,cpp}`. **Regressions**: `ForensicLifecycle.F03_*` (5) — both orderings arm exactly once, siblings both arm from one deferred fill, the deferred path still honours the F-01(a) refusal, and a stale orphan is evicted rather than arming a later intent.

- **F-06 — RESOLVED (2026-08-22); P1** Exit intents leaked into `pending_` and were never released. `pending_` entries were erased only by an opener fill or an explicit `cancel()`; an order rejected after registration received neither. Traced: default run registered 5 intents and armed 4 (opener 8369 orphaned); `--exec-bar-delay 0` registered 1 and armed 0.

  **Fix, both halves**: the real fix is F-02's terminal-transition hook, which releases the intent of every order that dies without filling. On top of that `pending_` is capacity-bounded with loud eviction, and registered/armed/deferred/cancelled/evicted counters are exposed through `ExitManager::counters()`, folded into `AnalyticsReport` and rendered in the report's Execution Quality section — so a leak is visible without inspecting the container. The bound is a backstop, not the mechanism: `ForensicEngine.F02_RejectedOrderDoesNotLeakItsExitIntent` asserts `exit_intents_evicted == 0` precisely so the bound can never become the thing that silently hides a regression.

  **Files**: `src/exits/exit_manager.{h,cpp}`, `src/analytics/analytics.{h,cpp}`, `src/analytics/report_generator.cpp`, `src/engine/engine_pending.cpp` (frozen — counter fold). **Regressions**: `ForensicLifecycle.F06_*`, `ForensicEngine.F06_ExitIntentLifecycleReachesTheReport`.

- **F-09a — RESOLVED (2026-08-22); P2** Multi-level opener fills grew the armed quantity and rolled `entry_price` to VWAP but left `stop_loss`/`take_profit` anchored to the **first** partial. Traced: a 972.61-unit entry walking 4 levels rolled `entry_price` to 7062.0651 while the stop stayed at 7028.6157 — the designed 5.14 risk distance became 33.45 against the true entry (6.5×). **Fix**: the levels are re-shifted by the same VWAP delta applied to `entry_price`, through the same F-01(a) guard so a wide walk cannot re-introduce the mis-arm; a breach mid-walk disarms and flattens the whole outstanding quantity. The designed distance is recomputed from the jointly-shifted levels rather than cached, because caching it grew `armed_intent` past a cache line and cost ~14% on `on_bar`. **Files**: `src/exits/exit_manager.{h,cpp}`. **Regressions**: `ForensicLifecycle.F09a_*`.

- **F-09b — RESOLVED (2026-08-22); P2** `total trades` counted closing fill legs, not closed lots. The same single round trip walking 4 exit levels was reported as **4 trades**, inflating trade counts, win rates and every per-trade average. **Fix**: `Analytics` accumulates PnL per closing leg (cash and realized PnL stay exact against every partial) and settles the win/loss statistics only when the position returns to flat. `total_trades` is now closed round trips; `closing_fill_legs` is reported alongside so execution fragmentation stays visible. `realized_pnl` is a separate per-leg accumulator, so a position that has scaled out but not yet closed still reconciles against cash. **Files**: `src/analytics/analytics.{h,cpp}`, `src/analytics/report_generator.cpp`. **Regressions**: `ForensicEngine.F09b_*` (3).

  *Answer to open question 3*: yes — `tests/golden/sma_basic_expected.json` encodes `total_trades`, and `tests/test_analytics.cpp` asserts it in eight places. All of them use single-leg full closes, so per-round-trip counting reproduces every existing expectation unchanged. The golden file did move, but for F-01(a), not for this (see below).

- **F-05a — RESOLVED (2026-08-22); P1** Equity was allowed to go and stay negative: with F-01 and F-02 both suppressed the account ran from $10,000 to **−$101,678.92** with no margin call, liquidation or bankruptcy stop. **Fix**: `portfolio::observe_marked_equity` latches a bankruptcy flag the first time marked equity reaches or crosses zero, driven from the engine's authoritative marked-equity pass (`OrderIntentProcessor::marked_account_equity`, which runs on the engine thread before every strategy callback). Latched — a later favourable mark does not resurrect a wiped account — and a NaN/missing mark is a data-quality condition, never a bankruptcy. The flag reaches `AnalyticsReport` and the report renders a `RUN INVALID — ACCOUNT BANKRUPT` banner **before** any ratio, instead of printing a Sharpe ratio for an account that no longer existed. **No auto-liquidation**: that is a behaviour change owned by F-05b. **Files**: `src/execution/portfolio.{h,cpp}`, `src/analytics/{analytics.h,analytics.cpp,report_generator.cpp}`, `src/engine/order_intent_processor.cpp` (frozen). **Regressions**: `ForensicEngine.F05a_*` (5).

- **F-07a — RESOLVED (2026-08-22); P1** Venue filters silently did nothing on local CSV backtests. The Binance CSV has no `symbol` column, so every bar carried `symbol = ""` and `--symbol BTCUSDT` did not bind. Proven: `--instrument BTCUSDT:tick=0.01,lot=1.0,minn=100000000` — which should round every quantity to whole units and reject every order on min-notional — produced byte-identical output. **Fix**: `InstrumentSpecCache::unmatched_overrides` reports every configured `--instrument` symbol absent from the loaded series, and `engine::validate_instrument_overrides` (called once from `setup_event_loop_infra`, before any worker) **refuses to start**, naming both the unmatched specs and the symbols actually present — including `"" (unbound — pass --symbol)`. A silent no-op is now a hard failure. **Files**: `src/engine/instrument_spec_cache.{h,cpp}`, `src/engine/{engine.h,engine_pending.cpp}` (frozen). **Regressions**: `ForensicEngine.F07a_*` (3, including a positive control).

- **F-10 — RESOLVED (2026-08-22); P3** `StrategyFactory` was a second, hardcoded strategy table beside `StrategyRegistry`: it listed six strategies, omitted every one registered after it was written (`ema-rsi-atr-pullback` among them), and **silently fell back to mean-reversion for any unrecognised name**, so a typo ran a different strategy instead of failing. **Fix**: `StrategyFactory` is now a thin adapter over `StrategyRegistry` — `create`, `has` and `available` all delegate, and `strategy_params` is applied through the strategy's own parameter schema so a strategy without a given knob keeps its default. An unknown name now throws. The two live strategy-switch call sites in `engine_market.cpp` check `has()` first and keep the running strategy on a typo rather than aborting a live stream. **Files**: `src/strategy/strategy_factory.h`, `src/engine/engine_market.cpp` (frozen). **Regressions**: `ForensicEngine.F10_*` (4).

---

## Workstream B — frozen-surface track (`LIVE_SAFETY_CCB_APPROVED`)

- **F-02 — RESOLVED (2026-08-22); P0; ROOT CAUSE** A rejected bar-delayed order deadlocked the strategy permanently. `route()` parks bar-delayed orders and returns status `pending`, so `finalize_route` took the non-rejected path and armed the exit intent. The actual rejection happened one bar later inside `drain_due → process()`, which has **no** corresponding `finalize_route` and never called `notify_position_change_all`. The reject path in `finalize_route` existed but was unreachable for every delayed order — i.e. for every order in a default backtest. Traced: order 8369 rejected at bar 410 (`rule=drawdown`); the strategy stayed in `entry_pending_long` with `open_qty=0, opener=0` for the remaining **1,713,190 bars** and never traded again. The failure was **silent** — a complete analytics report with a Sharpe ratio for a run that stopped trading 39 minutes into a 3-year dataset.

  **Fix**: one engine-owned terminal-transition emission point, `OrderIntentProcessor::emit_terminal_transition(order_id, symbol, qty, terminal)`. It sets the terminal status, drops the dashboard cache entry, and then does the two things that were missing:
  - **releases the order's exit intent** — `ExitManager::cancel(order_id)` when the dead order is an opener (its intent can never be promoted, which is F-06's real fix), or `ExitManager::release_close_reservation(opener, qty)` when it is a closer, so a bracket-fired close that died terminally gives its reserved quantity back instead of silently understating the lot;
  - **resyncs the owning strategy's position gate** via `notify_position_change_all`, which is what returns an optimistic `entry_pending_*` state to `flat`.

  Every `set_status(..., rejected | cancelled | expired)` in the order pipeline routes through it: the venue risk check, the risk-manager reject/halt, the post-registration halt gate, the venue filter, the max-open-orders cap, the delayed-order capacity cap, the async submit failure, the async cancel acknowledgement, the operator cancel and the end-of-stream expiry. `ForensicEngine.F02_EveryTerminalStatusSiteRoutesThroughTheEmitter` reads the translation unit and fails on any terminal `set_status` written outside the emitter, so the next rejection rule added cannot silently reopen the hole.

  `notify_position_change_all` gained a `sweep_flat_brackets` parameter, passed `false` from the emitter. The emitter already cancels the dead order's own intent precisely by opener id; letting the legacy net-flat sweep also fire there would additionally drop a *different* opener's still-live pending intent whenever the book happened to be flat. The unstick signal is the `set_position_open` push, not the sweep.

  **Files**: `src/engine/order_intent_processor.{h,cpp}`, `src/engine/fill_processor.{h,cpp}` (both frozen), `src/exits/exit_manager.{h,cpp}`. **Regressions**: `ForensicEngine.F02_*` (6), covering the bar-delayed path, the synchronous path, the capacity path, the venue-filter path, intent release, and the invariant guard.

- **F-04 — RESOLVED (2026-08-22); P1** Position sizing was blind to the only execution cost that exists. `apply_execution_cost_params` populated `entry_slip_bps`/`exit_slip_bps` solely from `o.bar_spread_bps` — the `--bar-spread-bps` flag, which the CLI itself documents as *"DEPRECATED, no effect"* and which defaults to zero — so sizing overshot by ≈4.9×. **Fix**: the estimate is derived from the execution model that actually governs fill prices, the synthetic book's half spread (`--mm-spread-pct` → `mm_calibration::base_spread_pct`): a market entry crosses to the first resting level at `mid * (1 ± half)`, so the half spread *is* the entry slip. The vol-widening term only widens it further at runtime, making the base an honest lower bound available at sizing time. `bar_spread_bps` is retained only so a deliberately larger operator estimate still wins. Same injection for Monte Carlo (`McRunConfig::mm_spread_pct`). **Files**: `src/strategy/apply_execution_cost_params.h`, `src/bin/main.inc` (frozen), `src/simulation/monte_carlo_{types.h,controller.cpp}`. **Regressions**: `ForensicWorkstreamB.F04_*` (3). **Observed effect**: on the full-dataset run the worst single trade is now −187.48 against the strategy's $200 risk budget; pre-fix the average loss was $776.80.

- **F-05b — RESOLVED (2026-08-22); P1** No leverage or cash admission rule for spot: $186,325 of notional on $10,000 of equity (18.6×) was accepted without comment, later 50.3×, and 68.3× ($6.83M on $100k). Default caps (`max_position_value $1e9`, `max_exposure $5e9`) never bind at these sizes and `max_notional_frac` defaults to 0.

  *Decision (open question 2)*: **a hard cash constraint for spot**. `risk_limits::max_gross_leverage` caps gross mark-to-market exposure (this symbol's worst case plus every other held instrument) at a multiple of account equity; `1.0` is a cash account that cannot borrow. Breach **rejects** the order (`risk_rule::gross_leverage`) — it does not halt and does not liquidate. Inventory-*reducing* orders stay exempt, so an account already over the line can always de-risk; an unusable equity reading refuses rather than disabling the cap.

  The library default is `0.0` (disabled) so an embedder's existing configuration is not changed underneath it. The CLI resolves the default per venue: `1.0` on spot, disabled on futures, where `--max-leverage` / `FuturesRiskCheck` already owns leverage and liquidation distance and stacking both would double-charge the same exposure. Operator override: `--max-gross-leverage` (distinct from the existing per-order futures `--max-leverage`).

  **Files**: `src/risk/risk_manager.h` (frozen), `src/risk/risk_manager.cpp`, `src/bin/main.inc` (frozen). **Regressions**: `ForensicWorkstreamB.F05b_*` (5).

- **F-07b — RESOLVED (2026-08-22); P1** `--symbol` never bound to symbol-less bar sources. **Fix**: `MarketSeries::bind_unset_symbols` binds a configured symbol to bars and ticks that carry none, touching **only** unbound rows so a genuinely multi-symbol load is never rewritten; `main.inc` calls it on both load paths and reports how many rows were bound.

  *Decision (open question 5, scope)*: an unset `--symbol` on symbol-less data is **not** an error. Single-instrument research runs that never name the instrument are legitimate and common. It is a loud warning instead — and F-07a turns it into a hard failure the moment an `--instrument` spec is actually configured, which is the case where the silence was harmful. **Files**: `src/data/market_series.{h,cpp}`, `src/bin/main.inc` (frozen). **Regressions**: `ForensicWorkstreamB.F07b_*` (4), including the audit's own proof: with the symbol bound, a min-notional spec now actually rejects every order.

- **F-08 — RESOLVED (2026-08-22); P2** Order timestamps preceded the information that produced them. Bar *N*'s timestamp is its **open** time; the order derived from `close[187]` was stamped 03:07:00, one full bar interval before that close existed. Not a price lookahead — execution is correctly deferred by `execution_bar_delay=1` — but it mis-dated every order, and every time-windowed risk rule inherited the error.

  *Decision (open question 5)*: **carry an explicit decision timestamp**, do not overwrite the order's timestamp. Bar open time is the correct market-data clock and keeps its meaning. `order_event::decision_ts` defaults to the event timestamp, so an order nobody stamps behaves exactly as before. The engine maintains `last_decision_ts_` per observation — equal to the observation for ticks and L2 updates, one bar interval later for bars — and `route()` stamps it. The interval is inferred once per run from the modal gap between consecutive same-symbol bars over a bounded prefix, and requires a clear majority: an irregular series yields zero, which leaves the decision clock equal to the bar open (exactly the pre-F-08 behaviour) rather than inventing an interval. `RiskManager`'s `orders_per_minute` window and daily-loss reset now key off the decision clock.

  **Duration analytics deliberately unchanged**: entry and exit are both fill timestamps on the same clock, so elapsed time was already correct. The reported "avg holding 0s" came from F-01's zero-duration phantom exits, not from this — the same run now reports 40m 4s. **Files**: `src/core/event.h`, `src/engine/{engine.h,engine.cpp,engine_market.cpp,engine_pending.cpp,order_intent_processor.{h,cpp}}` (frozen), `src/risk/risk_manager.cpp`. **Regressions**: `ForensicWorkstreamB.F08_*` (5).

- **F-10b — RESOLVED (2026-08-22); P3** The `--strategy` help string is generated from `StrategyRegistry::available()` instead of a hardcoded list that named six strategies and omitted every later registration. `--help` now lists all nine. **Files**: `src/bin/main.inc` (frozen). **Regression**: `ForensicWorkstreamB.F10b_RegistryIsTheOnlySourceOfTheStrategyList` (plus `ForensicEngine.F10_FactoryAndRegistryAgreeOnWhatExists`, which fails if a second list ever reappears).

**No strategy-level stopgap was added.** The plan proposed a bounded `entry_pending_*` timeout in `ema_rsi_atr_pullback_strategy` as an interim workaround while F-02 sat in CCB. F-02 landed in the same cycle, so the workaround was never introduced and there is nothing to remove.

---

## Verification evidence (2026-08-22)

**Post-fix isolation matrix** — same configurations as the table above, run against the full 1,713,600-bar BTCUSDT dataset with `--strategy ema-rsi-atr-pullback --balance 10000 --seed 424242`:

| Configuration | Trades | Win rate | Avg holding | Final equity | Bankrupt | Outcome |
| --- | ---: | ---: | ---: | ---: | :---: | --- |
| Shipped default (full) | 26 | 15.4% | 40m 04s | 7,309.42 | no | trades across the whole 3 years |
| 3,000-bar slice | 0 | — | — | 10,000.00 | no | no signal in the window; account untouched |
| Narrow spread 2 bps (full) | 19 | 10.5% | 34m 12s | 8,330.89 | no | no deadlock at the drawdown breach |

All rows are economically coherent: no deadlock, no wiped account, no negative equity, non-zero holding times, and the worst single trade (−187.48) is inside the strategy's $200 risk budget. The strategy remains unprofitable over the period (−26.9%) — that is now a strategy result rather than an artefact, which is the point of the audit.

**Independent reference evaluator** (`tests/reference/ref_eval_ema_rsi_atr.py`, clean-room, shares no code with the engine), full dataset:
- 29,527 flat-state signal bars vs 29,504 entry orders the engine emitted (`exit_intents_registered`). The 23-bar residual is signals the engine correctly suppressed while already in a position, which the reference deliberately does not model.
- **26 of 26** engine entries align exactly with a reference signal bar under `execution_bar_delay=1`. Pre-fix the engine emitted 4 orders in total.

**Test suite**: 1,533 passed, 12 skipped, 0 failed (`--gtest_filter=-EngineStreaming.*`; `EngineStreaming` passes in isolation — the full unfiltered run stalls in a threaded funding test, a pre-existing condition unrelated to this work).

**Gate scripts**: `check-hotpath-json.sh` OK, `check-layer-deps.sh` OK, `check-live-safety-freeze.sh` OK with the recorded CCB approval.

**Hot path** — `ExitManager::on_bar` runs once per market observation per symbol, `on_fill` once per fill. Measured with a purpose-built isolated harness compiling the pre-fix and post-fix `exit_manager.cpp` against the identical driver, pinned to one core, median of 101 repetitions each with an independently allocated `ExitManager` (this benchmark moves ±10% on heap layout alone — a single unrelated 512-byte allocation at construction shifted it 8%):

| | before | after | Δ |
| --- | ---: | ---: | ---: |
| `on_bar`, 1 armed | 9.5 ns | 9.8 ns | +3% |
| `on_bar`, 16 armed | 84.1 ns | 90.3 ns | +7% |
| `on_bar`, 64 armed | 315.8 ns | 355.2 ns | +12% |
| `on_fill` arm+cancel | 173.9 ns | 190.4 ns | +9% |
| `on_fill` 4-level walk | 214.2 ns | 239.8 ns | +12% |

The `on_bar` per-intent *work* is unchanged — a control build with HEAD's own `on_bar` body compiled into the post-fix translation unit measures the same as the post-fix body (335–347 ns vs 345–357 ns at 64 armed, against 318–342 ns for the pre-fix TU), so the residual is translation-unit code layout from ~500 added lines, not the loop. Two real regressions were found and removed during this pass: caching the designed stop distance on `armed_intent` grew it past a cache line (+14% on `on_bar`), and `cancel()` performed two redundant hash lookups. The remaining `on_fill` cost is the F-01(a) breach test and the F-03 orphan guard, on a path that runs once per fill (28 fills against 1.7M bars in the audited run). Benchmarks are checked in: `BM_ExitManager_OnBar`, `BM_ExitManager_ArmFill`, `BM_ExitManager_MultiLevelOpenerFill` in `benchmarks/bench_main.cpp`.

**Hot-path allocations**: `HotpathAllocs.SmaSynthetic_1000Bars` and `HotpathAllocMatrix.B_BarSma_1000` moved from ~62k to ~106k allocations, and the budgets were raised to 115k with the reason recorded in both tests. This is the F-02 fix working: that fixture breaches the drawdown limit early and then rejects every order, and the pre-fix budget was calibrated against a *deadlocked* strategy that emitted ~76 orders in 1,000 bars. The strategy now correctly returns to flat after each rejection and keeps signalling (~1,000 orders), each costing what an order always cost. Per-event steady state is unchanged: the idle scenarios (`A_BarIdle_1000`, `C_TickIdle_3600`, `D_BarIdle_1000_Standard`) and every `pool_grows == 0` assertion are untouched.

**Golden fixture**: `tests/golden/sma_basic_expected.json` was regenerated, and only for F-01(a). Every one of that fixture's 28 entries slips further than its own 0.3% designed stop distance, so the pre-fix run stopped every trade out at a shifted level above the market it filled in — win rate 0.0%, final equity 8,919.05, which is precisely the audit's phantom-stop-out signature. The refused-bracket flatten gives win rate 35.71% and final equity 9,226.97. Order count, fill count and trade count are unchanged, so F-09b did not contribute. The reason is recorded in `tests/golden/sma_basic_config.json`.

**Reproduction artifacts** checked in from the audit session:
- `tests/fixtures/forensic_golden_f01_long_no_stop.csv` — 79-bar deterministic dataset: one verified long signal at bar 53, then a strictly rising market. Lowest low from the signal bar onward 7016.45 vs designed stop 7014.26.
- `tests/reference/ref_eval_ema_rsi_atr.py` — independent clean-room EMA/RSI/ATR + entry-condition evaluator, transcribed from the headers, sharing no code with the engine. Reproduces engine indicator values bit-exactly.
- `tests/reference/make_golden_f01.py` — regenerates the golden dataset (`python3 tests/reference/make_golden_f01.py tests/reference`).
- `tests/reference/forensic_trace.h` — env-gated (`TT_FORENSIC=<path>`) trace sink emitting `BAR`, `COND`, `ENTRY_ORDER`, `ARM`, `EXIT_EVAL`, `FILL`, `RISK_REJECT`, `FINALIZE` records. Inert and byte-identical when unset. Not wired into the build; kept as the reproduction tool for the next lifecycle audit.

**Incidental fix**: `benchmarks/bench_main.cpp` included `strategy/sma_strategy.h`, which has not existed since the strategy moved into its own directory — the benchmark target did not build at `HEAD`. Corrected to `strategy/sma/sma_strategy.h`.

---

## Open questions — resolved and remaining

1. **F-01(a) on breach** — RESOLVED: refuse to arm and flatten. See F-01.
2. **F-05b policy** — RESOLVED: hard cash constraint for spot (gross exposure ≤ equity), reject on breach, off for futures. See F-05b.
3. **F-09b existing expectations** — RESOLVED: yes, nine assertions encode `total_trades`, all single-leg, all unchanged by round-trip counting. See F-09b.
4. **F-03 live adapters** — **OPEN**. Which venue adapters can report a fill synchronously on submit is still uncertified. The `ExitManager` fix makes arming order-independent regardless, so the backtest and shadow paths are safe, but `execution_bar_delay = 0` in live mode still needs a per-adapter certification before it can be called safe. Tracked here; not closed by this work.
5. **F-08 timestamp shape** — RESOLVED: explicit decision timestamp alongside the market-data clock. See F-08.
6. **Scope** — **OPEN, unchanged**. This audit covered the batch bar backtest with the local CSV provider only. Tick mode, L2 mode, the streaming path, shadow, live, Monte Carlo, checkpointing and QuestDB persistence were **NOT ACTIVE IN THE AUDITED RUN** and remain unaudited. F-02's terminal-transition emitter is shared by every path that routes through `OrderIntentProcessor`, and F-01(b)'s window is opened from `drain_due`, which every market path calls — but neither has been *exercised* under tick, L2 or streaming load beyond the existing suite. Re-check before declaring the mechanism closed for those loops.

---

**Last updated**: 2026-08-22 — F-01…F-10b landed (one CCB cycle); open questions 4 and 6 remain.
