# Trading-logic audit decision record — 2026-08-17

Tier: T3 (frozen engine/provider/risk surfaces). Human approval token supplied:
`LIVE_SAFETY_CCB_APPROVED`.

## Scope and acceptance

This change set is limited to reproducible trading semantics:

- source-aware market-quantity units at execution boundaries;
- same-symbol delayed execution with no synthetic end-of-stream fill;
- authoritative ledger replay (recorded economic events applied once);
- exchange timestamps for L2 events;
- closed-candle gating for Binance klines;
- direction-symmetric futures margin checks;
- current-equity strategy sizing and fee-complete open PnL.
- prior-bar-only breakout baselines and real swing-range regime input;
- shared-book ownership during Bitget synthetic quote refresh;
- defensive split close/open lot accounting when a fill carries an explicit
  old-opener attribution and overshoots that lot.

Acceptance requires focused regressions for every corrected defect, the full
test suite, the three repository gates, no new hot-path allocation, and fresh
safety/performance/correctness review. Passing code tests is not sufficient for
live release: the human CCB, clean freeze gate and attended shadow exercise are
separate mandatory gates.

## Decisions

1. Raw integer market quantities carry an explicit scale. Execution adapters
   receive base units; the order book receives values converted to its configured
   quantity scale. A blanket `/ 1e8` is rejected because local/synthetic integer
   data are unscaled while venue and fractional CSV data are fixed-point.
2. `execution_bar_delay=N` counts future price-bearing events for the same
   symbol. Timestamp latency remains a separate queue. Orders with no future
   observation expire at EOS and never reach an adapter.
3. Replay is ledger/audit replay. Market data update marks; recorded orders,
   fills and funding rebuild accounting exactly once. Strategies, exits, risk
   decisions and simulated execution are not regenerated because the event-log
   preamble contains no strategy/config manifest. When a binary ledger is
   configured, a logging-ring drop is terminal even under an otherwise
   permissive drop policy; a silently incomplete ledger is not authoritative.
4. L2 event timestamps originate at the provider. Host wall time is only a
   compatibility fallback for direct callers that omit a timestamp.
5. Futures leverage/liquidation use engine-supplied per-symbol marked account
   equity rather than spot-style cash; a missing mark on another open symbol
   refuses risk increases, while strictly exposure-reducing orders remain
   permitted. L2-only paths update the current mark from the book before risk.
6. Breakout boundaries, ATR minima and volume baselines are frozen before the
   candidate bar advances their histories. EMA regime classification receives
   the detector's measured swing range rather than a boolean sentinel.
7. Synthetic quote refresh may cancel only quote IDs owned by that executor;
   it may not clear venue depth or locally resting strategy orders.

## Rejected alternatives

- Filling a delayed A order on a B event using A's cached price: correct price,
  wrong causal opportunity.
- Force-filling at EOS: invents both liquidity and a future timestamp.
- Regenerating replay orders/fills: duplicates recorded derived events and
  cannot reproduce arbitrary strategies/configuration.
- A per-symbol map of dynamically-created queues: first-touch allocations on
  the event path.

## Residual risks and rollback

Bar OHLC has no intra-bar path or aggressor-volume split; its range-fill policy
remains a low-fidelity approximation. Authoritative venue balance/margin
reconciliation remains outside this patch. Multi-lot close attribution is still
ambiguous when a strategy supplies no opener ID; the engine only infers the
single unambiguous opposing lot. Partial IOC and unfilled IOC/FOK submissions
still lack an adapter-to-engine terminal outcome, so tracker state and provider
latency metadata can outlive the immediate order.

The final adversarial live review identified release blockers that are not
greenwashed by the passing backtest/unit tests:

- private execution fills and fatal ingress are not drained at a bounded cadence
  during L2-only or forming-kline traffic;
- a successful REST cancel can discard bridge identity before a legitimate late
  private fill arrives;
- venue-originated cancel/reject/expire does not reach the engine order lifecycle;
- unknown non-fill bracket lifecycle messages can enter the unknown-fill path;
- Binance diff-depth is interpreted as a replacing snapshot without U/u
  sequencing;
- individually finite fill inputs can still overflow economic products before
  portfolio/analytics mutation;
- malformed known private envelopes and invalid Bitget side/lifecycle identity
  can be silently treated as unrelated or accepted;
- the mainnet durable-log gate does not yet require a regular recoverable file
  and therefore accepts sinks such as `/dev/null`.

Bounded replay is refused until prefix/checkpoint reconstruction and an
append-sequence contract exist; exchange timestamps need not be monotonic in the
record stream. Authoritative replay accepts only finalized, non-segmented current
v2 logs because legacy/rotated logs lack the required complete prefix and
unambiguous quantity scale/opener/strategy attribution. Replay also depends on
the caller supplying the recorded initial balance because the log has no config
manifest.

Verification on this worktree: 1,327/1,327 sandbox-compatible CTest cases pass;
the one excluded real-socket shutdown case passes independently outside the
sandbox. Hot-path focus is 55/55, the final focused ASAN set is 209/209, layer
and JSON gates pass, and two seeded synthetic backtests are byte-identical. The
dirty freeze gate correctly refuses the modified protected files. No comparative
p99 benchmark was produced, so performance is not release-cleared.

Rollback is by semantic boundary: quantity metadata/conversion, scheduler/EOS,
ledger replay, and accounting/risk changes are independent. No persisted
portfolio or live-order state migration is introduced.
