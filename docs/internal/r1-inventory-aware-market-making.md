# R1 — Inventory-aware market-making strategy

Status: implemented (strategy + verification), not wired into the engine event
loop. Date: 2026-08-20.

Risk register entry R1 read *"no full inventory-aware market-making
algorithm"*. Before this change the only thing in the repository with
"market maker" in its name was `src/market_maker/`, which seeds synthetic
counterparty liquidity into a backtest orderbook. That is simulation
furniture, not a strategy: it has no inventory, no fair value, no risk
inputs, and it never produces an order intent that reaches pre-trade risk.

This document is the specification the implementation and its independent
reference both follow.

---

## 1. Scope boundary (A02)

| Component | Location | Responsibility |
|-----------|----------|----------------|
| `MarketMaker` | `src/market_maker/` | Synthetic counterparty liquidity for bar-mode simulation |
| `InventoryAwareMarketMakingStrategy` | `src/strategy/market_making/` | Quote decisions from market state + authoritative inventory |

Neither includes the other. `scripts/check-layer-deps.sh` (check C) fails the
build on any include edge in either direction.

Intended pipeline position (not a current engine path):

```
market data
  -> canonical market state           (market_snapshot)
  -> IMarketMakingStrategy::evaluate  (this module)
  -> quote_decision / quote_intent
  -> pre-trade risk (requires a frozen-surface adapter)
  -> backtest matcher | shadow | later sandbox
  -> order lifecycle / fills
  -> portfolio / authoritative inventory
  -> back into the strategy input (requires an authoritative quote ledger)
```

Separately, and never inside the strategy:

- `SyntheticLiquidityModel` (`src/market_maker/`) — counterparty liquidity
- matching / fill model (`src/execution/queue_model.h`, `src/orderbook/`) —
  queue position and fills

---

## 2. Public API

`src/strategy/market_making/mm_strategy.h`

```cpp
class IMarketMakingStrategy
{
public:
    virtual ~IMarketMakingStrategy() = default;

    [[nodiscard]] virtual strategy_result evaluate(
        const market_snapshot& market,
        const inventory_snapshot& inventory,
        const strategy_context& context) noexcept = 0;

    [[nodiscard]] virtual std::string_view strategy_id() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t strategy_version() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t strategy_config_hash() const noexcept = 0;
};
```

`strategy_result` is a plain `status + value` struct, matching the existing
`<x>_status` idiom in the repository (`resolve_footprint_tick_size`,
`risk_action`). The repository uses neither `std::expected` nor a shared
`Result<T>`, so no new one was introduced. A non-ok status always carries a
PAUSED decision with no intents, so a caller that only inspects the decision
still fails closed.

### Types

| Concept | Type | Note |
|---------|------|------|
| Price | `Price` (`src/types/price.h`) | existing int64 fixed point, 1e-4 |
| Quantity | `qty_atoms` = `std::int64_t` | canonical atoms (1e-8), signed |
| Basis points | `basis_points` = `double` | risk inputs arrive as doubles |
| Time | `timestamp_ns` = `std::int64_t` | supplied by the caller, never read from a clock |
| Side | `order_side` (`src/core/event.h`) | existing enum |
| Instrument | `mm_instrument` | validated fixed-point projection of `instrument_spec` |

`qty_atoms` is signed because inventory is signed; the orderbook's `quantity`
alias is `std::uint64_t` and cannot express a short. It is a signed view of
the repository's existing atom scale, not a second quantity system.

`mm_instrument` is likewise a projection, not a replacement:
`make_mm_instrument()` converts a venue `instrument_spec` and **refuses** a
tick or lot size that is not exactly representable on the Price/atom grid
rather than approximating it.

---

## 3. Mathematical semantics

All prices below are in `Price::raw()` units.

### Fair value

```
mid        = (best_bid + best_ask) / 2
microprice = (best_ask * best_bid_qty + best_bid * best_ask_qty)
             / (best_bid_qty + best_ask_qty)
imbalance  = (best_bid_qty - best_ask_qty) / (best_bid_qty + best_ask_qty)
book_half  = (best_ask - best_bid) / 2

fair = mid
     + microprice_weight  * (microprice - mid)
     + imbalance_weight   * imbalance * book_half
     + short_flow_weight  * short_flow_signal * book_half
```

`imbalance_weight` and `short_flow_weight` default to **0**. The imbalance
term is computable from the canonical snapshot; the short-flow term is a hook
for a signed order-flow signal the market-state layer does not publish yet.
Neither is enabled by default, so v1 fair value is exactly
`mid + w * (microprice - mid)`. No new feed was invented for R1.

**Zero-denominator policy.** `best_bid_qty + best_ask_qty == 0` means neither
quoted price is backed by displayed size. The canonical market layer has no
"phantom book" state, so this fails closed: PAUSED with
`invalid_market_state`, no fallback to a mid derived from prices nothing
backs. (The specification allowed either a mid fallback or PAUSED; the
repository's fail-closed rule decided it.) A single side at zero keeps the
formula well defined and is quoted normally.

### Inventory and reservation price

```
u        = clamp(signed_base_position / hard_limit, -1, +1)
boost    = 1 + soft_limit_skew_boost * (|u| - soft_ratio) / (1 - soft_ratio)   for |u| > soft_ratio
           1                                                                    otherwise
skew_bps = reservation_skew_bps_at_hard_limit * u * boost
reservation = fair * (1 - skew_bps / 10000)
```

`soft_limit_skew_boost` defaults to 0, which reduces the formula to exactly
`reservation = fair * (1 - skew_at_hard_limit * u / 10000)`.

**Monotonicity is a hard invariant.** `u * boost(|u|)` is strictly increasing
in `u` for any non-negative boost, and `fair > 0` is enforced, so
`reservation` is strictly decreasing in `u`:

- `u > 0` (long): reservation `<` fair — bids get less attractive, asks more
- `u < 0` (short): reservation `>` fair — asks get less attractive, bids more
- `u == 0`: reservation `==` fair

Config validation rejects
`reservation_skew_bps_at_hard_limit * (1 + soft_limit_skew_boost) >= 10000`,
which is what keeps the reservation price positive at the limit.

### Spread controller

```
fee_component = fee_buffer_bps + maker_fee_multiplier * instrument.maker_fee_bps

raw_half_spread = fee_component
                + volatility_multiplier * short_horizon_volatility_bps
                + toxicity_multiplier   * toxicity_risk_bps
                + latency_buffer_bps
                + latency_multiplier    * latency_risk_bps

half_spread = clamp(max(min_half_spread_bps, raw_half_spread),
                    min_half_spread_bps, max_half_spread_bps)
```

`maker_fee_multiplier` defaults to 1.0: a round trip earns `2 * half_spread`
and pays the maker fee on both legs, so covering one maker fee per side is
the break-even requirement.

If the configured cap clips `half_spread` below `fee_component`, quoting is a
structural loss — no intents are emitted and `insufficient_edge` is recorded.

### Quote ladder

```
offset(level) = half_spread + level * level_spacing_bps
bid_target    = reservation * (1 - offset / 10000)   -> floor to tick
ask_target    = reservation * (1 + offset / 10000)   -> ceil  to tick
```

Floor for bids and ceil for asks: rounding can only move a quote away from
the touch, never through it. Adjacent levels are then forced at least one
tick apart, so tick rounding can never collapse two levels onto one price.

Hard invariants on every emitted set:

- `bid[level+1] < bid[level]`, `ask[level+1] > ask[level]`
- highest bid `<` lowest ask
- every price is tick-valid and `> 0`
- every quantity is lot-valid and `> 0`

### Size controller

```
bid_multiplier = clamp(1 - size_skew_strength * u, min_size_mult, max_size_mult)
ask_multiplier = clamp(1 + size_skew_strength * u, min_size_mult, max_size_mult)

bid_size = lot_floor(round_to_atom(base_size * bid_multiplier))
ask_size = lot_floor(round_to_atom(base_size * ask_multiplier))
```

Rounding is to the *nearest* atom before the lot floor. Flooring the raw
product would drop a whole lot whenever a multiplier such as 0.85 lands one
ulp low in binary; the maker-safe granularity is enforced by the subsequent
`lot_floor`, not by the atom conversion.

Because `clamp` is monotone, the invariants follow directly:

- long inventory: `bid_size <= neutral_bid_size`, `ask_size >= neutral_ask_size`
- short inventory: `ask_size <= neutral_ask_size`, `bid_size >= neutral_bid_size`

In the reducing band (`|u| >= reducing_bias_ratio`) the inventory-increasing
side is additionally multiplied by `reducing_size_factor` (default 0.25). The
cut also applies at and beyond the hard limit so the reported side size stays
monotone in `|u|` even where the side is suppressed outright.

---

## 4. Safety semantics

### States

| State | Meaning | Intents | Resting quotes |
|-------|---------|---------|----------------|
| `ACTIVE` | Normal two-sided quoting, inventory-skewed | both sides | replaced |
| `REDUCING_ONLY` | Hard limit reached on at least one side | inventory-reducing side only | replaced |
| `PAUSED` | Fail closed | none | **must be cancelled** (`cancel_resting_quotes == true`) |

### Band policy

| `abs(u)` | State | Reason | Behaviour |
|----------|-------|--------|-----------|
| `< soft_limit_ratio` | ACTIVE | `normal` | plain skew |
| `[soft, reducing)` | ACTIVE | `inventory_soft_limit` | skew, optionally boosted |
| `[reducing, 1)` | ACTIVE | `inventory_reducing_bias` | inventory-increasing side cut to `reducing_size_factor` |
| `>= 1` | REDUCING_ONLY | `inventory_hard_limit` | inventory-increasing side suppressed |

The `[reducing, 1)` band deliberately stays ACTIVE rather than flipping to
REDUCING_ONLY: keeping a heavily reduced two-sided presence is what lets the
position work itself off without the strategy becoming a one-way taker
magnet. REDUCING_ONLY is reserved for the state where a side is genuinely
closed.

### Hard-limit enforcement is over the worst case, not the position

```
worst_long   = max(position, worst_case_position_if_all_buys_fill)
worst_short  = min(position, worst_case_position_if_all_sells_fill)
buy_headroom  = lot_floor(max(hard_limit - worst_long,  0))
sell_headroom = lot_floor(max(worst_short + hard_limit, 0))
```

A side with zero headroom emits nothing. Per level, the emitted quantity is
capped by the remaining headroom, so for every decision:

```
worst_long  + sum(bid quantities) <= hard_limit
worst_short - sum(ask quantities) >= -hard_limit
```

That is what makes pending orders count against the limit *before* they fill
(I05, P03), not only afterwards.

The effective limit is the tighter of the configured
`inventory.hard_limit_base` and a non-zero `inventory_snapshot::hard_limit`
from the ledger.

### Fail-closed gates

| Condition | Result |
|-----------|--------|
| Not configured | `not_configured` status, PAUSED, no intents |
| tick/lot/fee metadata invalid | `invalid_instrument` status, PAUSED, `invalid_instrument_metadata` |
| Crossed/locked book, non-positive price, no displayed size, non-finite or negative risk input, out-of-range flow signal | PAUSED, `invalid_market_state` |
| `event_time > decision_time` or `receive_time > decision_time` | PAUSED, `invalid_market_state` (look-ahead) |
| `sequence_valid == false` and `pause_on_sequence_gap` | PAUSED, `sequence_gap` |
| `decision_time - receive_time > max_market_data_age_ms` | PAUSED, `stale_market_data` |
| `!authoritative` and `require_authoritative_inventory` | PAUSED, `unknown_inventory` |
| Implausible ledger magnitudes or negative ledger hard limit | PAUSED, `unknown_inventory` |
| `half_spread < fee_component` | no intents, `insufficient_edge` |
| Post-only quote would cross after tick rounding | that level suppressed, `post_only_cross_prevented` |
| Quantity 0 after lot rounding, or below venue `min_qty` | that quote not emitted |

All applicable gate reasons are recorded, not only the first, so telemetry
shows every reason a quote was withheld.

**Post-only policy is suppress, not clamp.** The order-intent system has no
venue-independent guarantee that a post-only order will be rejected rather
than crossed, so a quote that would take liquidity after tick rounding is
simply not emitted. With `post_only == false` the crossing quote is emitted
and no suppression reason is recorded — the difference is tested both ways.

### Quote churn guard

`quotes.minimum_refresh_ticks` and `quotes.minimum_quote_lifetime_ms` are
evaluated against `strategy_context::resting`, supplied by the execution
layer, so the strategy holds no mutable state. When the guard trips the
decision carries no intents and `requote == false`, meaning *leave the
resting quotes alone*. The guard applies only to a plain two-sided ACTIVE
refresh: a decision that pauses, trips the hard limit, or removes a side is
never throttled.

---

## 5. Determinism and hot-path properties

`evaluate()` is a pure function of `(config, market, inventory, context)`:

- no clock read — every time value arrives in the snapshots or the context
- no network, file, database, or secret access
- no global mutable state; the telemetry sink is injected and nullable
- no RNG
- no unordered-container iteration influencing output order
- `noexcept`; configuration errors fail fast at `configure()` time instead

Hot path (verified by `MMStrategyIntegration.EvaluateAllocatesNothingAfterWarmup`,
which counts global `operator new` calls through `tests/helpers/alloc_counter.h`):

- **0 heap allocations** per `evaluate()` after warmup, across ACTIVE,
  REDUCING_ONLY and PAUSED paths at the maximum ladder depth
- fixed-capacity output (`fixed_vector`, capacity `max_quote_levels * 2`) — no
  unbounded growth; a full container refuses rather than grows
- no locks, no string formatting, no JSON

The telemetry record is only built when a sink is attached, and the sink
contract is explicitly non-blocking: an SPSC ring push that may drop, never a
mutex or a socket write. A telemetry outage cannot move quotes.

`decision_hash()` folds every actionable decision field in a fixed order,
skipping padding, and is what the determinism and replay tests compare.

---

## 6. Verification

| Suite | File | Covers |
|-------|------|--------|
| Unit | `tests/test_mm_strategy_unit.cpp` | T01–T25 plus instrument/market/churn edge cases |
| Property + scenario | `tests/test_mm_strategy_property.cpp` | P01–P10 over 400 generated cases each, S01–S15 |
| Integration | `tests/test_mm_strategy_integration.cpp` | I01–I08, look-ahead, zero-alloc, telemetry |
| Golden / differential | `tests/test_mm_strategy_golden.cpp` | 13 fixtures against the Python reference |
| Queue sensitivity | `tests/test_mm_strategy_queue_sensitivity.cpp` | Front/Uniform/Back, markouts, sign convention |

### Differential reference

`tests/reference/mm_strategy_reference.py` re-derives this specification in
exact rational arithmetic (`fractions.Fraction`). It shares no code, header,
or build with the C++ implementation.

The two agree exactly on the fixed-point grid. That is not assumed: at
generation time the reference verifies that every floor/ceil-to-tick and
every nearest-atom rounding sits at least `1e-6` grid units away from its
boundary, and refuses to write a fixture that does not. Several fixtures were
adjusted during authoring because of exactly this check. Pure-double
aggregates (`target_half_spread_bps`, `inventory_utilization`) are compared
with a relative tolerance of `1e-9` / `1e-12`.

```bash
python3 tests/reference/mm_strategy_reference.py --check   # local verification
python3 tests/reference/mm_strategy_reference.py --write   # after an intentional change
```

The repository does not currently invoke this script from a tracked local CI
configuration. Treat the command as an explicit verification step until a
separate CI change wires it in.

### Reproducibility anchors

`MMStrategyGolden.ConfigAndResultHashesAreStable` pins two values that must
not drift silently:

| Anchor | Value |
|--------|-------|
| Reference `config_hash` | `0x834DD815BAEB86F4` |
| Folded golden `result_hash` (13 cases) | `0x73FA545867D95FFE` |

| Fixture | SHA-256 |
|---------|---------|
| `tests/golden/mm/cases.json` | `35c47a70689ab5c24da56a5c45d8a099700b1b1740ac7bbe1a7c26b7c91fa10d` |
| `tests/golden/mm/reference_config.json` | `01e07644ed2a56fdccf1b071dcb8ac4b480a23880c19e1ab27b901814f002a2a` |
| `tests/golden/mm/expected.json` | `6793ed1c091191b46c26fa6aae88a3a1b3a4b3f944bd99e7f0e16751d71b7610` |
| `tests/reference/mm_strategy_reference.py` | `5cbe4e9f3c8df00b48f4bcd62350bab44025ed2a7fae401e7c36d36424fc0d0c` |

### Markout sign convention

Defined once, in `tests/test_mm_strategy_queue_sensitivity.cpp`, and tested
in `MarkoutSignConventionIsMirroredForBuysAndSells`:

```
markout_bps = side_sign * (mid_at_horizon - fill_price) / fill_price * 10000
side_sign   = +1 for a buy fill, -1 for a sell fill
```

Positive means the fill was on the right side of where the mid subsequently
went. Horizons: 10 ms, 100 ms, 1 s, 5 s.

---

## 7. Known gaps

1. **Not wired into the engine event loop.** `evaluate()` is exercised by the
   test harness, not by `engine.cpp`. Wiring it requires a market-state
   publisher that emits `market_snapshot` and an inventory publisher that
   emits `inventory_snapshot` from the order/portfolio subsystems — both of
   which sit on the Phase 1 live-safety freeze surface. That is a separate,
   CCB-scoped change, and R1 deliberately does not bundle it. Nothing in R1
   introduces a dependency on `engine_live`.

2. **Dependency on R3 (authoritative order ledger).** The hard-limit
   invariant is only as good as
   `worst_case_position_if_all_buys_fill` / `..._sells_fill`.
   **R3 has since landed** (`docs/internal/r3-authoritative-risk-accounting.md`):
   `OrderTracker` is the authoritative ledger and
   `truetest::risk::build_risk_view` produces exactly those two quantities as
   `instrument_risk_view::worst_case_long_qty` /
   `worst_case_short_qty`. What is still missing is the *publisher* that
   converts them into an `inventory_snapshot` for this strategy — the same
   engine-wiring gap as item 1, on the same frozen surface. Until that lands,
   `safety.require_authoritative_inventory` stays `true`, which makes the
   strategy PAUSE rather than guess, and the test suites keep using the
   `sim_ledger` double.

3. **`engine_live` is part of the standard build.** `CMakeLists.txt` builds
   all three binaries; there is no research-only build flag that omits
   `engine_live`. Live orders remain impossible in `engine_backtest` and
   `engine_shadow` through the constexpr `target_allows_live_orders()` gate,
   so this is a pre-existing packaging risk, not one R1 introduces. It was
   deliberately not refactored here.

4. **Short-flow signal has no producer.** `market_snapshot::short_flow_signal`
   exists and is validated, but nothing publishes it;
   `fair_value.short_flow_weight` therefore defaults to 0.

5. **No parameter tuning.** The reference configuration is chosen for test
   determinism and invariant coverage, not for fill rate or P&L. Fitting
   parameters to in-sample P&L is an explicit non-goal.

6. **Shadow validation not run.** Running the strategy against live public
   market data through `engine_shadow` needs the engine wiring from gap 1.

---

## 8. Commands

```bash
# Build + run the R1 suites
cmake --preset linux-tests
cmake --build --preset linux-tests --target truetest_tests
ctest --preset linux-tests -R 'MMStrategy' --output-on-failure

# Or directly
./out/build/linux-tests/truetest_tests --gtest_filter='MMStrategy*'

# Differential reference
python3 tests/reference/mm_strategy_reference.py --check

# Gate scripts (mandatory after any src/ edit)
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh

# Benchmarks
cmake --preset linux-release-low-memory -DENABLE_BENCHMARKS=ON
cmake --build --preset linux-release-low-memory --target truetest_benchmarks
./out/build/linux-release-low-memory/truetest_benchmarks \
    --benchmark_filter='MMStrategy'
```
