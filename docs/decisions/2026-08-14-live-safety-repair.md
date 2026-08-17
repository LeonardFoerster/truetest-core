# ADR: Terminal live-safety repair series

- **Date:** 2026-08-14
- **Tier:** T3
- **Status:** Implementation authorized; acceptance pending
- **Authorization:** The operator supplied `LIVE_SAFETY_CCB_APPROVED` for this
  work. Human CCB review, the clean-path exercise, and shadow-soak evidence
  remain acceptance gates; this ADR is not evidence that they passed.

## Decision

Implement the safety repairs as serialized vertical slices on one clean baseline.

1. A cold `LiveSafetySession` is created before a live provider is opened and is
   transferred into the engine. Every failure after a successful live open uses
   the same exact-once kill-before-close sequence, including reconciliation and
   engine-construction failures.
2. Operator kill, engine halt, strict-persistence failure, teardown, and the
   destructor share one cached kill result. Halt is terminal and is set before
   the remote kill operation begins.
3. Provider shutdown is staged: stop local heartbeats/transports without
   disarming the venue countdown, perform the kill once, then disarm only after
   confirmed success. Failed, timed-out, or ambiguous kill leaves the venue
   countdown armed.
4. Kill-switch, DMS, and reconciler HTTP requests use a dedicated bounded,
   single-attempt path. It performs no clock resync/replay, reconnect replay,
   weight/rate-limit sleep, or HTTP/business retry.
5. The first failed DMS heartbeat latches terminal failure and immediately
   signals halt/kill. Failure before callback registration is delivered when
   registration occurs. DMS-local flatten functions are removed.
6. Streaming and strict persistence propagate explicit results to the CLI.
   Safety halt is never reported as clean operator completion.
7. Private venue threads never allocate from engine-owned pools, mutate the
   portfolio/audit/dashboard, or publish to engine worker rings. Futures
   funding settlements cross a fixed-capacity provider-owned SPSC ingress;
   only the engine event-loop consumes it and publishes the pooled event.
   Ingress overflow is terminal, loud, and never retried or overwritten.
8. A funding-capable live stream provides an engine-thread idle-drain seam so
   settlements are consumed while public market data is quiet and before the
   next strategy/risk decision. Public WebSocket reads use one persistent
   asynchronous frame operation across bounded idle ticks, so partial TLS or
   WebSocket frames cannot turn the idle deadline into a synchronous block.
   Its frame storage and composed-operation storage are preallocated, and late
   cancellation completion owns its state instead of referring to a destroyed
   transport. Bitget text heartbeats require a pong by a fixed deadline.
   Provider shutdown joins the producer before a final engine-thread drain and
   persistence flush.
9. Strict startup/recovery probes require authoritative evidence. Binance
   strict margin mode refuses HTTP/schema/identity ambiguity, and a
   TrueTest-branded Bitget bracket for a symbol outside the configured engine
   scope refuses restart instead of being silently ignored.

## Alternatives rejected

- Calling the provider kill switch directly from each UI or teardown path:
  permits duplicate kill attempts and inconsistent results.
- Closing the provider before kill: disarms DMS before account state is known
  safe.
- Wrapping existing REST calls with a shorter timeout: hidden retries, sleeps,
  clock resync, and pre-write replay remain active.
- Restoring checkpoint v1 or patching Adaptive Hybrid in place: both surfaces
  lack complete state contracts and are withdrawn until rebuilt.

## Residual risks

- Remote exchanges can return ambiguous outcomes after request bytes are sent.
  The process remains halted and DMS remains armed; no automatic retry occurs.
- Live construction failure can flatten pre-existing account exposure. This is
  the approved fail-closed policy for a provider that has already opened live.
- DNS/TLS cancellation and callback lifetime require sanitizer and timeout
  tests in addition to unit call-count tests.
- Funding accounting accepts only an authoritative venue funding envelope with
  exactly one valid settlement-currency delta. Missing, zero, duplicate,
  trailing, mixed-reason, or prefix-parsed evidence is terminal.

## Rollback

Rollback is by complete slice/binary only and requires the same CCB process.
Never restore halt clearing, safety retries, direct UI kill calls, partial
checkpoint restore, or a live launcher.

## Acceptance criteria

- Terminal halt cannot be cleared inside a process.
- Live kill is logically and remotely attempted at most once.
- Kill failure never disarms DMS and returns nonzero.
- Every kill/DMS/reconciler request is bounded and non-retrying.
- Strict persistence failures are latched and observable.
- Streaming distinguishes clean EOF, operator stop, safety halt, and faults.
- The private fill thread is the sole funding-ingress producer; the engine loop
  is its sole consumer and remains the sole producer of engine worker rings.
- Funding parse and handoff are bounded and allocation-free; overflow drains
  already-admitted records, then causes terminal halt without overwrite,
  growth, or retry.
- Strict margin and branded bracket recovery fail closed when venue evidence is
  missing, malformed, duplicated, or outside the configured symbol scope.
- Full tests, hot-path/layer/freeze gates, sanitizer runs, independent review,
  and the required clean shadow soak are recorded for the final fingerprint.
