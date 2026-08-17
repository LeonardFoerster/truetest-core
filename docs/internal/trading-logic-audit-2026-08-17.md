# Live-safety remediation decision record — 2026-08-17

## Scope

This T3 remediation is reconstructed from clean baseline
`276a83916d6f0d602f517bedf247864a9d345136`.  The separately located dirty
worktree is evidence only and is not part of this change series.

The work is intentionally split into these rollback boundaries:

1. atomic economic fill preflight;
2. tri-state private-message parsing;
3. ordered private order lifecycle;
4. bounded engine-side private-ingress servicing and shutdown drain;
5. Binance diff-depth refusal;
6. authoritative ledger-file reservation and finalization;
7. cross-slice audit and verification.

## Architecture registration

### Private execution

Private WebSocket execution data has exactly one producer and one consumer.
It is transported in source order through a fixed-capacity SPSC ingress.  A
full ingress, malformed known private message, identity contradiction, or
terminal transport ambiguity closes admission and is observed by the engine as
a terminal failure.  REST submit/cancel outcomes remain a separate producer
channel and never form a multi-producer private ring.

The lifecycle is monotonic:

```text
active -> cancel_requested -> rest_cancel_acked -> private_terminal
```

A REST cancel acknowledgement is advisory.  Identity is retained until an
accepted private terminal event.  Private `fill`, `canceled`, `rejected`, and
`expired` records are resolved by the engine; `unknown_lifecycle` and `fatal`
are terminal control records.  Only a fill can enter fill or bracket-fill
accounting.  A non-fill loss of an active protective leg is a terminal
protection failure, except for the expected OCO sibling cancellation after the
opposite sibling's recorded fill.  After a successful REST cancel response,
private confirmation has a fixed 30-second `steady_clock` deadline; expiry
does not erase identity and instead requests terminal halt/reconcile.  The
deadline is a named, deterministic configuration-independent constant and is
covered by a clock-driven regression.

### Fill atomicity

Every fill must pass finite and checked-arithmetic preflight before tracker,
event-log, portfolio, analytics, risk, dashboard, or strategy mutation.  The
portfolio and analytics independently preview their numerical transitions;
commit happens only after every preview succeeds.  A rejected fill leaves all
accounting state unchanged and latches terminal halt in a live path.

### Service and shutdown ordering

The engine drains private ingress before every strategy-capable bar, tick, and
L2 action; after each public frame (including ignored frames); at bounded idle
intervals; and, after producers join, before final funding/accounting drain and
ledger finalization.  A final drain records already-produced venue truth only;
it cannot initiate a venue mutation or automatic unwind.

### Market depth

Raw Binance `depthUpdate` is not a snapshot.  Until a separately reviewed
sequence synchronizer exists, live configuration accepts only explicit partial
book streams and rejects all diff-depth spellings before provider open.

### Authoritative ledger

Every resolved Live provider reserves a new regular POSIX file before provider
open.  It uses a descriptor-based, no-follow exclusive `.partial` file and
publishes an authoritative final filename only after file data sync, atomic
rename, and parent-directory sync.  Any failure poisons admission.  A leftover
`.partial` is forensic evidence, never an authoritative replay ledger.

## Non-goals

- No in-process Binance diff-depth resynchronisation.
- No retry, backoff, auto-resume, or synthetic terminal success.
- No change to compile-time target gates, kill switch, DMS, or reconciler
  refusal semantics.
- No invented crash-RPO claim for power loss/SIGKILL.

## Evidence status

Implementation and verification evidence is appended slice by slice.  External
requirements, including attended Shadow soak and two-person CCB review, remain
separate from code-level evidence.
