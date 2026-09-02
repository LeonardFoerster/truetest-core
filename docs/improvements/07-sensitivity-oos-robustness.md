# Plan 07 — Sensitivity, metamorphic tests, and OOS validity

## Controlled variants

Run one factor at a time and compare both the serialized report and the binary
event ledger. The supported backtest controls are:

- `periods_per_year=525600` versus 252 — the fill/trade ledger must remain
  identical; only annualized metrics may differ;
- documented maker/taker fees versus zero or a fixed fee;
- synthetic-book spread/impact through `--mm-spread-pct`, related MM depth
  controls, and `--impact-k-bps`/`--impact-adv`;
- `--exec-bar-delay` 0, 1, and 2 (or the explicitly grouped latency controls);
- production risk limits versus a diagnostic soft/uncapped risk configuration.

`--realistic-fills` is a deprecated warn-noop. `--bar-spread-bps` no longer
changes recorded fill prices, but an explicitly larger value can still win as
a legacy strategy-sizing estimate. The campaign runner rejects both controls
from baseline and variant arguments so neither ambiguous legacy behavior can
enter the study. There is no selectable
bar-constrained-versus-synthetic fill model or alternate same-bar ambiguity
policy in the current CLI: synthetic-book bar traversal and conservative
SL-first ambiguity are fixed behaviours. Those two axes remain `UNVERIFIED`,
not simulated as sensitivity variants.

Use the runner with immutable, externally sliced local CSV inputs. It accepts
only `engine_backtest`, owns output/event/config paths, requires a non-zero
seed plus inline/no-pin execution, freezes its parameters before any child
process, and delegates every cell to the two-process reproducibility capture:

```bash
./scripts/capture-sensitivity-oos.sh \
  --output-dir /tmp/ema-sensitivity-oos \
  --study-status exploratory \
  --is-window train=/absolute/path/btcusdt-2020-2022.csv \
  --oos-window future=/absolute/path/btcusdt-2023.csv \
  --variant baseline --factor baseline \
  --variant ppy-252 --factor periods-per-year \
    --arg --periods-per-year --arg 252 \
  --variant fee-zero --factor fees --arg --fee --arg zero \
  --variant tight-spread --factor spread-impact \
    --arg --mm-spread-pct --arg 0.0001 --arg --mm-vol-mult --arg 0 \
  --variant delay-0 --factor execution-delay \
    --arg --exec-bar-delay --arg 0 \
  --variant delay-2 --factor execution-delay \
    --arg --exec-bar-delay --arg 2 \
  --variant risk-soft-uncapped --factor risk-limits \
    --arg --risk-soft-limits --arg --max-daily-loss --arg 0 \
    --arg --max-gross-leverage --arg 0 \
  -- ./out/build/linux-tests/engine_backtest \
    --provider local --strategy ema-rsi-atr-pullback --symbol BTCUSDT \
    --balance 10000 --seed 424242 --thread-preset inline --no-pin \
    --no-risk-soft-limits --max-daily-loss 80 --max-gross-leverage 1 \
    --status-format off --no-tui
```

Every `is-*`/`oos-*` cell gets two result JSON files, two event ledgers, the
engine's current `--dump-config` snapshot, exact argv, input/binary/compiler
hashes, environment, and dirty-tree state. The dump-config schema is not
exhaustive (for example, it currently omits `exec_bar_delay`), so exact argv is
the authoritative binding for CLI controls absent from that snapshot.
`parameters.freeze` binds the normalized absolute inputs,
their hashes, and the binary hash before execution; the runner rechecks those
bindings before and after every cell. `campaign.manifest` hashes the cell
config, argv, manifest, environment, dirty patch, and untracked-file manifest,
records baseline-relative result/event equality, labels every risk-limit
variant `DIAGNOSTIC_ONLY`, and leaves conclusion stability explicitly
`NOT_ASSESSED`. It is evidence capture, never a performance-promotion gate.

The 20,000-bar table in the ignored
`out/forensic-audit-20260822/sensitivity.csv` contains the formerly quoted
observations: baseline `14 trades / 8668.72`, tight spread `9 / 9267.39`,
delay 0 `15 / 8469.25`, delay 2 `12 / 9088.40`, and fixed fee `14 / 8655.81`.
Its dataset/binary/dirty-patch hashes are recorded in the adjacent manifest,
but it lacks per-variant argv, dump-config snapshots, and event-ledger artifacts.
Treat it as `DIRTY-EXPLORATORY`, not as a reproducible or promotable claim.

## Metamorphic tests

`tests/test_sensitivity_oos.cpp` covers these contracts:

1. The same seeded fixture produces an equivalent economic trade sequence in
   the same process; the campaign wrapper performs the stronger fresh-process
   byte comparison of report and event ledger.
2. Changing `periods_per_year` preserves the economic trade ledger and
   non-annualized accounting while changing annualized metrics; the campaign
   fails if this factor changes the binary event ledger.
3. Scaling all OHLC prices and starting balance by two preserves trade order,
   side, timestamp, and relative metrics while PnL/equity doubles within the
   deterministic book-quantization bound.
4. Whitespace-only CSV formatting does not change trade artifacts.
5. A repeated header creates exactly one provenance rejection while later
   parser accepted indexes and trade artifacts remain contiguous/equivalent.
6. An upward one-bar SMA feature shift causes no prior signal, produces the
   expected buy/reversal within its three-bar causal window, and then ages out.

## OOS controls

- Freeze parameters before reading OOS results (`parameters.freeze`).
- Supply chronological IS/OOS files explicitly; the runner records their
  hashes but does not pretend to infer chronology from arbitrary CSV dialects.
- Record every trial, factor, argv, effective configuration, and event artifact.
- For the `validated` label, declare `--oos-regime NAME` for an OOS window
  before execution. The runner rejects byte-identical IS/OOS content, but this
  is only a guard against trivial reuse and unlabeled promotion, not proof that
  the input is a distinct chronology, regime, or symbol.
- Test unseen time ranges and at least one independently reviewed additional
  regime/symbol before claiming portability.

## Acceptance criteria

No single supported spread/impact, delay, fee, or risk choice may reverse a
conclusion without disclosure. Label every campaign `exploratory`,
`validated`, or `regression-only`; diagnostic soft/uncapped-risk results are
never performance evidence. A `validated` label still requires human review of
chronology, frozen parameters, regime/symbol independence, and all disclosed
variant outcomes.

## Evidence status

The campaign wrapper and its CTest contract are `CONFIRMED`; the CTest runs both
failure-injected fake-engine cases and a checked-in-fixture campaign against the
real `engine_backtest`. Fresh-process report/event determinism,
periods-per-year ledger invariance, price scaling, CSV formatting/repeated
header, and one-bar causal SMA contracts are regression-tested. The historical
20,000-bar table is `DIRTY-EXPLORATORY`. OOS robustness, regime portability, a
selectable bar-fill alternative, and an alternate same-bar ambiguity policy
remain `UNVERIFIED`.

## References

- `docs/architecture/03-realism.md`
- `docs/reference/01-instructions.md`
- `docs/todos/11-F-forensic-lifecycle-audit.md`
