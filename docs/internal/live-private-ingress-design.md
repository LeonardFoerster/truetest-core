# Ordered private-account ingress design

**Status:** implementation design — 2026-08-18

**Addresses:** `docs/todos/02-P1-freeze.md` P1-02 and the approved
live-safety repair.

## Phase-0 baseline

`src/engine/engine.cpp` is currently 4,618 lines and remains the composition
root.  The pre-repair path has two independent private channels: a funding
SPSC ring and an `ExecutionBridge` reader-thread path that mutates order maps
and produces fills directly.  A successful REST cancellation currently
retires local state before its authoritative private terminal arrives.  That
cannot prove source order, exact-once fills, or final shutdown truth.

The repair adds one narrowly scoped application use case:

> When a provider private reader receives account truth, the engine applies
> each admitted record in source order before strategy-capable work, because
> private venue lifecycle and cash truth are authoritative.

## Boundary and ownership

```text
private WS reader (one producer)
  parse + fixed POD projection only
        |
        v
ProviderExecutionIngress (fixed 256-slot SPSC)
        |
        v
engine event loop (sole consumer)
  resolve identity/cumulative/tombstone -> account/audit -> terminal ack
        |
        +--> ExitManager only for explicitly registered native bracket legs
```

`private_execution_record` is a trivially-copyable, bounded record.  It
contains fills, ACK/terminal lifecycle records, funding, explicit group
lifecycle, and an explicit fatal/unknown control outcome.  Its producer never
allocates engine events, consults strategy state, generates IDs, invokes
ExitManager, or mutates bridge tracking maps.  A malformed projection or full
ring latches failure and closes order admission; it never overwrites, retries,
or grows.

The ingress is provider-owned because only the provider knows the sole private
reader lifetime.  The engine caches its pointer once; it does not make a
virtual call on the market-event hot path.  `IAsyncSubmitSupport` is retained
as the existing narrow adapter port solely for engine-thread lifecycle
resolution and terminal acknowledgement.

## Lifecycle proof

For engine-submitted orders, bridge-owned engine-thread state is:

```text
active -> cancel_requested -> rest_cancel_acked
       -> private_terminal_enqueued -> retired
```

A REST cancel acknowledgement is advisory only.  It retains both client and
exchange identity, starts a fixed 30-second private-confirmation deadline, and
does not retire tracker/dashboard/audit state.  A private terminal reserves a
non-cyclic fixed tombstone *before* the engine accounts it.  The engine then
acknowledges that sequence only after canonical accounting/audit succeeds;
only this acknowledgement retires the active identity and commits the
tombstone.  Tombstone exhaustion is a terminal admission failure rather than
a circular-cache eviction.

Economic fills require an immutable execution ID, explicitly reported
cumulative quantity, exact `next_cumulative == prior + last` proof, and a
fixed per-order fingerprint history.  A repeated execution ID is a no-op only
when every economic field matches exactly; any mismatch is fatal.  A
lifecycle-only full confirmation can retire an order only after previously
accounted economic cumulative quantity already equals the ordered total; it
never books a second fill.

## Engine application modes

The engine owns `drain_provider_execution_updates` and calls it before every
bar, tick, L2, adapter, risk, exit, strategy, route, and post-public-frame
boundary.  It drains all already-admitted records before acting on a failed
ingress or terminal fault.

Normal mode may run the canonical post-fill pipeline after a validated record.
Final accounting mode, used only after a provider proves its private reader
joined, is stricter:

- it may preflight, account, audit, publish canonical fill/funding truth, and
  acknowledge terminal tombstones;
- it must not route, cancel, place/modify brackets, dispatch strategy fills,
  notify position gates, run post-fill risk, unwind, or trigger a retry;
- it drains every admitted record even after a semantic fault, then latches a
  reconciliation-required terminal halt.

After the final drain, any bridge cancel-awaiting state or ExitManager native
leg/group proof is reconciliation-required; no clean shutdown claim is made.

## Native brackets and provider order

The engine does not infer venue semantics.  An `untracked` private record is
accepted only after an engine-thread `ExitManager` lookup proves an exact
registered native leg/group identity, expected symbol/close side, cumulative
proof, and execution fingerprint.  Everything else is fatal.  Until a venue
has that complete adapter contract, its native bracket feature remains
refused rather than downgraded to an untyped best effort path.

All funding shares this ingress so a fill cannot overtake a settlement from
the same private reader.  Public DataBridge processing gains a post-frame
service hook; its existing idle hook alone cannot cover ignored public frames.

## Delivery DAG

1. Fixed record + ingress + bridge two-phase terminal proof, with unit tests.
2. Engine consumer, private-before-public service points, REST-cancel advisory
   semantics, and final accounting-only drain.
3. Binance parser provenance and provider wiring; validate consumer before
   enabling the provider.  Keep Bitget disabled until its separate
   fill/order-channel cumulative proof is implemented.
4. Typed native bracket/group lifecycle, recovery attribution, and
   same-symbol admission protection.
5. Transport ownership/close-proof hardening, DataBridge post-frame service,
   authoritative ledger, and live depth refusal.
6. Full automated gates, two fresh independent safety reviews, and the
   required feature-enabled shadow soak.  No step is merge-ready before that
   evidence exists.

## Verification matrix

- FIFO sequence, overflow-without-overwrite, and malformed projection latch.
- partial/full cumulative proof, changed replay conflict, terminal tombstone
  capacity reservation, and late REST/private identity comparison.
- REST cancel ACK retains tracker state; private terminal is the only
  cancellation authority; 30-second deadline is deterministic.
- private-before-bar/tick/L2, idle, and ignored-public-frame servicing.
- final joined-producer drain accounts all admitted truth with zero outbound or
  strategy side effects and ends reconciliation-required for any economic
  fill/fault.
- provider parser/transport races, native-leg OCO/group order permutations,
  recovery identity, ledger durability, and live depth refusal.
