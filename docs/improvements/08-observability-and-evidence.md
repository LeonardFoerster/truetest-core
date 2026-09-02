# Plan 08 — Observability and audit evidence

## Trace requirements

Use disabled-by-default, structured, test-only or explicitly enabled tracing. It
must perform no RNG calls, state mutation, ordering change, or blocking hot-path
I/O. Remove unconditional per-bar `std::cout` instrumentation before trusted
benchmarks.

Each record must include:

```text
bar: provenance, index, timestamp, O/H/L/C
indicator: EMA/RSI/ATR before and after
strategy: state and every condition
order: IDs, intended price, quantity, decision timestamp
risk: every check and result
execution: model, book reference, intended/fill price, depth, slippage
fill: ID, quantity, fee, timestamp, remaining quantity
position: cash, quantity, cost basis before/after
exit: intent, arm, trigger, reason, close order
report: source records and reconciliation result
```

## Delivered test-component trace

`ObservabilityEvidence.OneTradeLinksPhysicalInputToReconciledReport` is a
deterministic, test-only integration sequence over the real `CsvBarParser`,
EMA/RSI/ATR strategy, `RiskManager`, `LocalBookAdapter`, `ExitManager`,
`Portfolio`, and `Analytics`. It follows one controlled round trip from the
CSV parser's physical row through the final accounting reconciliation.

The trace is buffered in test memory and serialized only after the report is
complete. No production engine, strategy, risk, execution, or live-safety
source contains a trace write. The older `tests/reference/forensic_trace.h`
uses mutex-protected `fprintf` and remains an unwired historical reproduction
tool; it is deliberately not the Plan 08 implementation.

Every evidence row names its origin:

- `PRODUCTION_RETURN_VALUE` — returned by a production component;
- `PRODUCTION_STATE_SNAPSHOT` — read through a production accessor;
- `INDEPENDENT_ORACLE` — independently recomputed and asserted against the
  production result;
- `HARNESS_ASSIGNED` — identity or timing supplied by the deterministic test
  sequence because the component API does not assign it;
- `HARNESS_JOIN` — correlated by explicit identifiers inside the test
  component sequence.
- `HARNESS_DERIVED` — deterministic arithmetic performed by the harness over
  explicitly provenance-labelled source fields.

Mixed records use per-field provenance columns. For example, `orders.csv`
separates harness-assigned identities/timestamps from the strategy-returned
side, price, and quantity; `signals.csv` separates production state, oracle
conditions, and the production decision.

The harness constructs the aggregate `risk_snapshot` passed to `RiskManager`,
so risk inputs are labelled `HARNESS_ASSIGNED` even when some values were read
from production accessors; only the returned action/rule is
`PRODUCTION_RETURN_VALUE`. Controlled depth is injected by the harness, read
back through the production orderbook snapshot API, and kept distinct from the
production fill result.

This distinction is load-bearing. Parser physical-row metadata is not carried
through the production data bridge, strategy predicate values are supplied by
the independent reference evaluator, `RiskManager` exposes only its aggregate
decision/first failing rule, and the normal engine audit sink discards most
records. Those production links remain `UNVERIFIED` in `completeness.csv`.

## Evidence bundle

```text
manifest.md
effective-config.json
input.sha256
binary.sha256
cmake-cache.sha256
signals.csv
orders.csv
risk-decisions.csv
fills.csv
trade-reconciliation.csv
metrics.json
semantic-result.csv
test-results.txt
bars.csv
indicators.csv
execution.csv
positions.csv
exits.csv
completeness.csv
compiler.sha256
toolchain.txt
environment.txt
git-status.txt
working-tree.patch
untracked.sha256
capture.argv.sh
artifacts.sha256
```

Generate the bundle from an existing `linux-tests` tree:

```bash
./scripts/capture-observability-evidence.sh \
  --output-dir /tmp/truetest-observability-evidence \
  --test-binary "$(realpath out/build/linux-tests/truetest_tests)" \
  --input "$(realpath tests/fixtures/observability_one_trade.csv)"
```

The runner executes the selected integration test in two fresh processes with
collection disabled and enabled. `metrics.json`, `effective-config.json`, and
the canonical `semantic-result.csv` (every `AnalyticsReport` field plus the
selected final harness state used by reconciliation) must be byte-identical. It then
records input, binary, compiler, CMake cache, commit, dirty
patch, exact capture argv, test output, and SHA-256 digests in an atomically
published directory. `working-tree.patch` records tracked modifications, while
`untracked.sha256` fingerprints untracked inputs without silently treating them
as part of the commit. Selected environment settings and toolchain versions are
captured separately.

Input, binary, compiler, CMake cache, tracked diff, git status, commit, and
untracked-file fingerprints are captured before execution and rechecked after
both processes. Endpoint drift fails the bundle. These before/after checks do
not claim to detect a transient change that is restored before the final
snapshot.

`artifacts.sha256` covers the manifest and every captured artifact except the
checksum file itself. It detects edits when checked against an externally
trusted digest; the bundle is not cryptographically signed and does not prove
authenticity on its own.

The output directory must be outside the source tree so capture staging cannot
contaminate the working-tree snapshot.

## Acceptance criteria

One trade can be followed from physical input row to final report value. Missing
links are `UNVERIFIED`, never silently inferred. Trace-enabled and disabled runs
are identical in results.

The acceptance applies to the declared `TEST_COMPONENT_HARNESS` scope. It does
not certify that the frozen engine performed the joins, nor does it certify
shadow/live/MC/streaming/checkpoint/persistence paths. Closing those gaps needs
a separately approved T3 design for preallocated POD observation through the
frozen engine/risk surface; synchronous per-event file tracing is not an
acceptable shortcut.

## Evidence status

The deterministic component-harness lifecycle and its fresh-process
trace-on/off result invariance are `CONFIRMED`. Complete production-engine
signal-to-report linkage, physical-row carriage into engine events, individual
strategy-predicate emission, ordered per-rule risk results, and trace effects
on latency/allocation remain `UNVERIFIED`. The bundle is audit scaffolding and
software-regression evidence, not live or performance evidence.

## References

- `src/strategy/ema_rsi_atr_pullback/ema_rsi_atr_pullback_strategy.cpp`
- `src/analytics/analytics.cpp`
- `docs/todos/11-F-forensic-lifecycle-audit.md`
