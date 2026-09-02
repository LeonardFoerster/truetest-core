# Plan 01 — Baseline and deterministic evidence

## Goal

Freeze a complete reproducible baseline before changing validity assumptions.

## Command

```bash
./out/build/linux-tests/engine_backtest \
  --provider local --path /absolute/path/data.csv \
  --strategy ema-rsi-atr-pullback --symbol BTCUSDT \
  --balance 10000 --seed 424242 --thread-preset inline \
  --no-pin --status-format off --no-tui \
  --output /tmp/ema_baseline.json --output-format json
```

## Codex steps

1. Record commit, dirty state, diff hash, compiler, preset, binary hash, input
   hash, exact argv, dump-config, and relevant environment.
2. Run twice in independent processes.
3. Compare serialized output and structured signals, orders, fills, timestamps,
   quantities, PnL, and final state.
4. Store a manifest next to every result.
5. Mark dirty-tree runs `DIRTY-EXPLORATORY`; do not promote them.

The first four steps can be captured with the repository wrapper. It expects
the executable invocation after `--` and writes two JSON results plus the
manifest and hashes into the selected directory:

```bash
./scripts/capture-repro-baseline.sh \
  --input /absolute/path/data.csv \
  --output-dir /tmp/ema-baseline \
  --preset linux-tests \
  -- ./out/build/linux-tests/engine_backtest \
     --provider local --path /absolute/path/data.csv \
     --strategy ema-rsi-atr-pullback --symbol BTCUSDT \
     --balance 10000 --seed 424242 --thread-preset inline \
     --no-pin --status-format off --no-tui
```

The wrapper compares both the JSON report and the binary event ledger. It
refuses an existing output directory and fails if either pair differs. Add
`--trace-env TT_FORENSIC` only when the executable is built with the explicit
trace sink; that performs a third run and requires both a non-empty trace
artifact and byte-identical report/event output. A dirty working tree is
recorded as `DIRTY-EXPLORATORY`; it is evidence for debugging only.

## Acceptance criteria

- Event-level output is byte-identical or every difference is explained.
- Effective defaults not printed by CLI are recorded.
- Trace-enabled and trace-disabled runs are identical.
- The same clean-tree commit reproduces the baseline.

## Evidence status

The wrapper contract and a fixture-level two-run result/event comparison are
`CONFIRMED`. The current working tree remains dirty, and the production trace
sink is not wired into the binary, so clean-tree promotion and trace invariance
remain `UNVERIFIED`.

## References

- `src/bin/main.inc`
- `docs/reference/01-instructions.md`
- `docs/governance/02-prerequisites.md`
