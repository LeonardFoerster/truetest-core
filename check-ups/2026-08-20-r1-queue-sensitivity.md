# R1 queue-sensitivity and markout report

Date: 2026-08-20
Harness: `tests/test_mm_strategy_queue_sensitivity.cpp`
Strategy: `truetest::mm::InventoryAwareMarketMakingStrategy`

## What this is — and what it is not

This report answers one question: **do the strategy's safety invariants and
its fill/inventory behaviour survive every cancel-queue assumption the
repository models?**

It is **not** evidence of an edge. The tape is synthetic, its aggressor flow
is uncorrelated with its price path, and its parameters were chosen so the
strategy's quotes can reach the touch at all. Read the caveats at the end
before quoting any number here as a result.

## Setup

| Item | Value |
|------|-------|
| Steps | 20,000 (1 ms per step ≈ 20 s of tape) |
| Seed | `0xC0FFEE2026`, explicit, no `random_device`, no system clock |
| Instrument | 0.10 tick, 0.0001 lot, 1.0 bp maker fee |
| Book spread | 110–200 ticks (1.8–3.3 bps on a 60,000 price) |
| Price path | Gaussian tick walk (σ = 3 ticks) plus a drift re-drawn every 500 steps |
| Aggressor flow | 30% of steps carry a taker print of 0–1.2 base units per side |
| Cancel flow | 0–0.9 base units per side per step |
| Ladder | 2 levels, base size 0.05, spacing 0.5 bps |
| Half spread | floor 0.3 bps, cap 6.0 bps, 1 bp maker fee + 0.2× vol + 0.2× toxicity + 0.5× latency |
| Inventory | hard limit 0.60 base, reservation skew 1.2 bps at the limit |
| Churn guard | refresh after ≥ 8 ticks of movement and ≥ 25 ms of quote life |
| Queue models | `FrontCancelModel`, `UniformCancelModel`, `BackCancelModel` — used unmodified from `src/execution/queue_model.h` |

The same generated tape drives all three runs, so the only difference between
them is the cancel assumption.

## Results

| Queue | Net P&L | Fees | Fills / quotes | Fill ratio | Mean time to fill | Median \|u\| | Max \|u\| |
|-------|---------|------|----------------|-----------|-------------------|-------------|----------|
| Front (optimistic) | 117.51 | 442.76 | 1517 / 3196 | 0.4747 | 7.22 ms | 0.157 | 0.581 |
| Uniform (baseline) | 116.35 | 443.41 | 1520 / 3196 | 0.4756 | 7.25 ms | 0.154 | 0.581 |
| Back (pessimistic) | 115.33 | 440.86 | 1508 / 3196 | 0.4718 | 7.37 ms | 0.144 | 0.580 |

Markouts from the maker's perspective, in bps
(`side_sign * (mid_at_horizon - fill_price) / fill_price * 10000`, `side_sign`
+1 for a buy fill and −1 for a sell fill — defined once in the harness and
covered by `MarkoutSignConventionIsMirroredForBuysAndSells`):

| Queue | Spread capture (h=0) | 10 ms | 100 ms | 1 s | 5 s |
|-------|---------------------|-------|--------|-----|-----|
| Front | 1.1574 | 1.2871 | 1.2808 | 1.3279 | 1.6323 |
| Uniform | 1.1578 | 1.2876 | 1.2783 | 1.3251 | 1.5998 |
| Back | 1.1591 | 1.2855 | 1.2767 | 1.2989 | 1.6085 |

## Invariants (asserted, all three models)

| Invariant | Front | Uniform | Back |
|-----------|-------|---------|------|
| Hard-inventory breaches | 0 | 0 | 0 |
| Quotes off the tick or lot grid | 0 | 0 | 0 |
| Max \|u\| ≤ 1 | 0.581 | 0.581 | 0.580 |
| Markout samples at every horizon | > 0 | > 0 | > 0 |
| Rerun with same seed reproduces fills and net P&L exactly | ✅ | — | — |

`Front` fills at least as often as `Back` on the same tape, which is what the
cancel-model ordering requires.

## Caveats — read before quoting any number above

1. **The queue assumption barely moves the result** (117.5 / 116.4 / 115.3).
   That is *not* a robustness finding. It is a property of this harness: most
   fills come from quotes that improved on the touch, where `queue_ahead` is 0
   by construction and the cancel model never applies. A tape where the
   strategy mostly *joins* an existing level would separate the three models
   far more.
2. **Markouts are positive at every horizon, which the generator guarantees.**
   Aggressor arrivals are drawn independently of the price path, so there is
   no informed flow and therefore no adverse selection to be measured. Real
   maker markouts at 1 s and 5 s are routinely negative. Do not read
   `+1.6 bps @ 5 s` as anything but "the synthetic flow is uninformed".
3. **Fees dominate.** Gross P&L is roughly 560 against 443 of maker fees on a
   1 bp fee schedule. Net profitability here is a function of the fee
   assumption, not of the model.
4. **The configuration was tuned to the tape, not to P&L.** The half-spread
   floor was lowered from the reference config's 6 bps to 0.3 bps because a
   6 bps quote never reaches the touch of a 1.8–3.3 bps book and every model
   reported zero fills. That is a fixture-realism fix, not parameter fitting —
   no parameter was chosen by comparing in-sample P&L.
5. **No real market data was replayed.** Running this against recorded L2 is
   the follow-up, and needs the engine wiring listed as gap 1 in
   `docs/internal/r1-inventory-aware-market-making.md`.

**Conclusion: R1 must not be described as robust alpha evidence.** What this
report supports is narrower and still worth having: the inventory, tick, lot
and hard-limit invariants hold identically under the optimistic, realistic and
pessimistic cancel assumptions, and the run is bit-reproducible from its seed.

## Reproduce

```bash
cmake --preset linux-tests
cmake --build --preset linux-tests --target truetest_tests
./out/build/linux-tests/truetest_tests \
    --gtest_filter='MMStrategyQueueSensitivity.*'
```
