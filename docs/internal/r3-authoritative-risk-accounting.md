# R3 — Authoritative Risk Accounting

**Risk register**: R3 ("Risk-Accounting nicht autoritativ genug").
**Status**: implemented (Phase 1 + Phase 2).
**Related**: `docs/internal/r1-inventory-aware-market-making.md` §7.2 (the R1
strategy's `inventory_snapshot.worst_case_position_if_all_*_fill` fields are
exactly what Phase 2 of this change makes derivable), `docs/todos/04-R-risk-management.md`.

---

## 1. What was wrong

| # | Defect | Where it lived |
|---|--------|----------------|
| 1 | Open-order state derivable from performance counters | `risk_snapshot::total_orders` / `::total_fills` were carried into the risk struct; `dashboard_snapshot_builder` derived `perf.total_orders` as `open_orders_cache_.size() + get_total_fills()` |
| 2 | Order lifecycle had no quantity dimension | `OrderTracker` was `id -> order_status` only. No original/filled/remaining qty, no symbol, no side, no `expired` state |
| 3 | Position exposure approximated by cost basis | `RiskManager::position_price()` derived a price as `cost_basis / qty`; the portfolio-exposure loop summed `abs(pos.cost_basis)` |
| 4 | Risk was not mark-to-market | no mark timestamps existed anywhere; `last_mark_prices_` was `symbol -> double` |
| 5 | Pending orders carried no risk | a candidate order was checked against the current position only |
| 6 | `max_portfolio_var_estimate` was a dead safety claim | declared in `risk_limits`, **one** occurrence in the whole repository, never read |
| 7 | Funding limit had no producer | `max_funding_8h_rate` is enforced, but `current_funding_8h_rate` had zero call sites (see R-03) |

## 2. Authoritative components after this change

| Concern | Authoritative owner |
|---------|--------------------|
| order lifecycle state, open orders, remaining qty, per-symbol pending exposure | `OrderTracker` (`src/execution/order_tracker.h`) — extended in place, **not** replaced |
| signed position quantity, cost basis, cash, lots | `portfolio` (`src/execution/portfolio.{h,cpp}`) — already fill-driven; unchanged semantics |
| current mark + mark age | `mark_point` values in the engine's `last_mark_prices_` (`src/execution/mark_point.h`) |
| derived risk view | `risk_snapshot::instrument` / `::portfolio` built by `truetest::risk::build_risk_view` (`src/risk/risk_accounting.h`) |
| enforcement | `RiskManager` (`src/risk/risk_manager.{h,cpp}`) |
| reporting only (never risk) | `Analytics` counters, `dashboard_snapshot` |

## 3. Removed derivations

* `risk_snapshot::total_orders` / `::total_fills` — deleted from the struct.
* `RiskManager::position_price()` (cost-basis-derived price) — deleted.
* `abs(pos.cost_basis)` as portfolio exposure — replaced by `|qty| * mark`.
* `perf.total_orders = open_orders_cache_.size() + get_total_fills()` — replaced
  by the ledger's lifetime order count.
* `risk_limits::max_portfolio_var_estimate` — deleted (see §6).

## 4. Public interface changes

* `order_status` gains `expired` and `unknown` (appended; existing values keep
  their ordinals).
* `OrderTracker` gains `register_order`, `on_fill`, `amend`, `find`,
  `open_exposure`, `pending_qty`, `filled_qty`, `orders_seen`, `symbol_of`,
  `tracks_symbol`, `reserve`, and the `for_each_order` /
  `for_each_symbol_exposure` iterations. `get_order_status` returns `unknown`
  (was `pending`) for an id the ledger has never seen.
  Note the two "remaining" quantities: `order_ledger_entry::remaining_qty()` is
  the raw `original - filled` invariant and keeps its value on a cancelled or
  expired order, while `OrderTracker::pending_qty(id)` is what risk means —
  the quantity that can still fill, and therefore exactly 0 once terminal.
* `risk_snapshot` gains `instrument`, `portfolio`, `ledger_authoritative`,
  `funding_rate_known`; loses `total_orders`, `total_fills`.
  `portfolio_risk_view::daily_realized_loss` mirrors
  `RiskManager::daily_realized_loss()`, the accumulator `max_daily_loss` is
  actually enforced from (the dashboards used to show a hardcoded 0).
* `risk_limits` gains `max_symbol_inventory_qty`, `max_mark_age_ms`,
  `require_fresh_mark`; loses `max_portfolio_var_estimate`.
* `RiskManager::check_order` / `check_post_fill` take an optional
  `risk_rule*` out-parameter (defaulted, so every existing call site compiles
  unchanged).
* `engine`'s `last_mark_prices_` is `symbol -> mark_point` (price + sim
  timestamp) instead of `symbol -> double`; `portfolio::get_equity` gains a
  matching overload.

## 5. Authoritative data flow

```
order intent ──route()──► OrderTracker::register_order   (symbol, side, qty, price)
                          OrderTracker::set_status(pending|open|rejected|cancelled|expired)
venue/paper fill ────────► OrderTracker::on_fill          (filled/remaining, dedupe by fill_id)
                          portfolio::on_fill              (signed position qty, cost basis, cash)
market/tick/L2 ──────────► last_mark_prices_[symbol] = {price, sim_ts}

                 ┌──────────────────────────────────────────────┐
                 │ truetest::risk::build_risk_view(...)         │
                 │   portfolio  (position qty, cost basis, cash)│
                 │   OrderTracker (open buy/sell remaining qty) │
                 │   mark_point  (price + age)                  │
                 └──────────────────┬───────────────────────────┘
                                    ▼
                          risk_snapshot {instrument, portfolio}
                                    ▼
                     RiskManager::check_order(candidate, ...)
                                    ▼
                   risk_action + risk_rule (machine-readable code)
                                    ▼
                 reject / halt / unwind + audit_sink reason code
```

## 6. VaR decision — **removed** (case B)

`grep -rn "max_portfolio_var_estimate" src/ tests/` returned exactly one hit
before this change: its own declaration in `risk_limits`. There is no VaR
estimator, no covariance/vol model feeding one, no config key, no CLI flag and
no enforcement branch anywhere in the repository. Keeping a configurable
`max_portfolio_var_estimate` therefore advertised a portfolio-VaR safety bound
that could never bind.

Per the task's case B the field is **deleted** rather than backed by an invented
formula. `risk_snapshot::realized_vol_1h` (which does have a defined meaning and
a producer path) is retained. If a real VaR estimator is ever added, it gets a
new field together with the estimator and its tests.

## 7. Funding

* **Settlements** (`funding_event` → cash / P&L / QuestDB) were already wired
  end-to-end and are untouched.
* **The 8h rate** had no producer. It is now derived at settlement time from the
  identity `funding_fee = -position_notional * funding_rate`, i.e.
  `rate = -cash_delta / position_notional`, and marked with
  `risk_snapshot::funding_rate_known`. `max_funding_8h_rate` is only enforced
  when a rate is actually known — an unknown rate can no longer look like
  "0.0, therefore inside the limit".
* Spot instruments never receive a `funding_event`, so they never acquire
  perpetual-only semantics.

## 8. Cross-margin / margin — deliberate capability boundary

The repository models per-symbol futures margin (`FuturesRiskCheck`,
`MaintenanceMarginTable`, `/fapi/v1/leverageBracket`). It does **not** model a
venue account-level cross-margin ratio, and R-04 records that as open. This
change therefore adds portfolio-level *exposure* aggregation (mark-to-market
gross/net notional across all instruments) but introduces **no** universal
margin formula and makes **no** cross-margin safety claim.

## 9. Hot-path notes

`build_risk_view` runs once per candidate order inside
`OrderIntentProcessor::process`, not per market event. It is O(#distinct
symbols) — bounded by `SymbolTable::kMaxSymbols` and typically 1-5 — and
allocation-free; the ledger's per-symbol aggregates are maintained
incrementally on each transition so the pre-trade query itself is O(1). The
same pass now also produces the account equity, which removed a second walk
over the position map and one `last_mark_prices_mu_` acquisition that
`marked_account_equity()` used to cost on this path. Measured budget and the
engine-throughput A/B: `check-ups/2026-08-20-r3-verification.md` §5.

## 10. Not in scope / unchanged

No new live-trading or network-write capability. `target_allows_live_orders()`
is untouched; every new code path is evaluation + rejection only.
