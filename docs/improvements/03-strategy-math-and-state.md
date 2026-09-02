# Plan 03 — Strategy mathematics and state machine

## Current contract

Long:

```text
allow_long AND close[t] > EMA[t]
AND RSI[t-1] <= 40 AND RSI[t] > 40
AND state == flat
```

Short:

```text
allow_short AND close[t] < EMA[t]
AND RSI[t-1] >= 60 AND RSI[t] < 60
AND state == flat
```

The local strategy uses EMA(150), RSI(14), ATR(14), Wilder RSI/ATR smoothing,
and a 2×ATR stop. “Pullback” is the RSI recross plus the trend filter; there is
no independent EMA-touch state.

## Codex steps

1. Maintain a reference evaluator sharing no production code.
2. Test warmup, exact thresholds 40/60, EMA equality, invalid bars, reset,
   rejected entries, partial fills, opposite signals, and multi-symbol state.
3. Compare reference signals to engine signals before comparing trades.
4. Verify `prev_rsi`, indicators, and position state are per-symbol and reset.
5. Make signal consumption after rejection an explicit tested policy.

## Invariants

- Invalid bars do not mutate indicators.
- No signal before EMA, RSI, ATR, and previous RSI are ready.
- Strategy and portfolio state converge after every fill.
- A pending/open state cannot emit an unintended second entry.

## Acceptance criteria

Every signal has independent values, `t`/`t-1` provenance, condition results,
state-before, decision, and state-after evidence.

## Evidence status

The boolean entry contract and indicator parameters are `CONFIRMED` from local
code. The independent, test-only evaluator in
`tests/reference/ema_rsi_atr_pullback_reference.h` shares no production
strategy or indicator code and is exercised bar-by-bar by
`EmaRsiAtrPullbackContractTest.IndependentReferenceMatchesFlatStrategySignals`.
For every candidate it records independent EMA/RSI/ATR values, `RSI[t-1]` and
`RSI[t]`, every boolean condition, state-before, returned decision, and
state-after in the failure trace.

The edge policy is covered by the contract tests: exact 40/60 semantics, EMA
equality, invalid OHLC, rejected entries (including the delayed engine route),
partial closes, opposite signals, multi-symbol isolation, and reset. The
explicit rejection policy is: a qualifying recross is consumed when observed;
a subsequent sizing/route rejection unlocks the pending state but cannot replay
that recross, so only a later fresh recross can enter.

## References

- `src/strategy/ema_rsi_atr_pullback/ema_rsi_atr_pullback_strategy.cpp`
- `tests/test_ema_rsi_atr_pullback_strategy.cpp`
- `tests/test_ema_rsi_atr_pullback_strategy_contract.cpp`
- `tests/reference/ema_rsi_atr_pullback_reference.h`
- `docs/reference/07-strategy-development.md`
