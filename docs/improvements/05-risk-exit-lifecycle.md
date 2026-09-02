# Plan 05 — Risk, exit, and position lifecycle

## Required invariants

| Invariant | Test |
|---|---|
| No exit before an entry fill | delayed-entry integration |
| Protection never exceeds filled quantity | partial-entry |
| Rejected protective close leaves protection or terminal emergency state | forced-reject |
| Flat position has no stale executable close | delayed-close/bracket race |
| No accidental reversal | latency greater than bar |
| Flatten never uses future information | same-bar timestamp |
| Reductions remain possible under risk pressure | limit-breach close |

## Paths to cover

- Slippage disarm and flatten queue.
- Min-notional, max-open-order, and venue-risk rejection of protective closes.
- `fill-fade` partial or zero protective fills.
- `execution_bar_delay > 1` with an in-flight strategy close.
- Opposite signal while exit is pending.
- Multiple intents for one opener.

## Required lifecycle record

```text
signal_id, order_id, opener_order_id, fill_id,
decision_ts, submit_ts, eligible_ts, fill_ts,
requested_qty, filled_qty, remaining_qty,
risk checks, exit reason, state before, state after
```

## Acceptance criteria

Every fired exit ends in filled, explicitly rejected, or explicitly terminal
state. No stale close may open exposure after its opener is flat.

## Evidence status

Implemented 2026-08-23: fired exits retain a ticketed protective-close record
until fill or an explicit terminal outcome; terminal/rejected protection halts
without retry; duplicate closes are reserved; and slippage flattening keeps its
own symbol, price, and timestamp. The durable audit record joins submitted,
filled, and terminal states by signal/order/ticket.

Deterministic focused tests cover ticket retention, partial fills, flatten
observation capture, lifecycle-record fields, event-log compatibility, and the
existing bracket/forensic paths. The full test gate remains `UNVERIFIED`:
`FR02_McFeeModelProducesCommissionDelta` and the native WebSocket shutdown
ordering test fail in the current worktree. A clean four-hour attended shadow
soak is also still required before any live-safety sign-off.

## References

- `src/exits/exit_manager.cpp`
- `src/engine/engine.cpp`
- `src/engine/order_intent_processor.cpp`
- `src/engine/fill_processor.cpp`
- `docs/todos/11-F-forensic-lifecycle-audit.md`
