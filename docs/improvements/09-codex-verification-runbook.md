# Plan 09 — Codex implementation and final verification

## Before editing

1. Read the relevant plan and source files.
2. Record and preserve unrelated dirty changes.
3. Classify the change as docs/test-only, cold-path, hot-path, or live-safety.
4. For frozen files, stop and use the governance/CCB process.

## Required gates

```bash
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
cmake --build --preset linux-tests --target engine_backtest truetest_tests
ctest --preset linux-tests --output-on-failure
```

## Evidence status

This runbook is a process control document. It does not certify the current
baseline; certification is `UNVERIFIED` until every promotion-gate item passes.

## References

- `AGENTS.md`
- `docs/governance/02-prerequisites.md`
- `scripts/check-hotpath-json.sh`
- `scripts/check-layer-deps.sh`
- `scripts/check-live-safety-freeze.sh`

Use ASAN/TSAN and focused hot-path tests for lifetime, timing, pool, ring, or
execution changes. Rerun the baseline twice after every execution or metric change.

## Promotion gate

Codex may call a result trustworthy only when:

- P0 execution and lifecycle tests pass;
- provenance is complete;
- hashes and effective config are recorded;
- fees and time basis are realistic and disclosed;
- accounting and metrics reconcile independently;
- OOS/walk-forward evidence exists;
- sensitivity does not reverse the conclusion;
- working tree and freeze gate are clean;
- no affecting `UNVERIFIED` item remains.

Otherwise report:

```text
USEFUL ONLY AS A SOFTWARE REGRESSION SIGNAL
```

## Final report fields

```text
Reproducibility: HIGH/MEDIUM/LOW/UNKNOWN
Implementation consistency: HIGH/MEDIUM/LOW/UNKNOWN
Data trust: HIGH/MEDIUM/LOW/UNKNOWN
Execution trust: HIGH/MEDIUM/LOW/UNKNOWN
Accounting trust: HIGH/MEDIUM/LOW/UNKNOWN
Metric trust: HIGH/MEDIUM/LOW/UNKNOWN
Overall trust: HIGH/MEDIUM/LOW/UNKNOWN
First non-strategy-determinable transition: ...
Decision-making status: ...
```
