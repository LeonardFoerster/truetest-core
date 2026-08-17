# Realism Models (Latency / Impact / Queue / Fill / Fee)

**Status**: Thin extraction / planned skeleton (Doc Phase).

**Planned / extracted; see reference/ for current** authoritative details and flag usage. Full material lives in `docs/reference/02-user-manual.md`, `docs/reference/01-instructions.md` (§7), tests (e.g. `test_latency_model.cpp`, `test_impact_model.cpp`, `test_queue_model.cpp`, `test_fill_model.cpp`, `test_fee_model.cpp`, `test_realistic_fills.cpp`), and source (`src/execution/*_model.*`, `src/orderbook/`).

Opt-in latency, impact, queue, probabilistic-fill, and fee models require their respective flags. Passive-side fill pricing is always on, and the synthetic market-maker has default calibration. **Completely bypassed in live** (live venue supplies truth). Realism is for backtest + shadow divergence measurement only.

**Last updated**: 2026-07 (new content impl — synthesized from instructions.md + user-manual.md; pointers only).

---

## Overview (from user-manual + instructions)

Realistic microstructure modeling enables honest slippage / adverse-selection analysis before risking capital:
- Queue position
- Walked-book impact
- Latency stacking
- Trade-tape shadow fills

Used in backtest (local/synthetic) and shadow (real data + simulated fills). L2-dependent models require `--depth-stream depth20@100ms` (or equivalent) + appropriate provider.

**Key flags** (see `docs/reference/04-flags.md` and instructions for full):
- `--order-latency-us N --order-latency-stddev-us M`
- `--wire-latency-us N`
- `--impact-k-bps N --impact-adv N`
- `--queue-model l2-snapshot`
- `--maker-queue-model uniform|front|back`
- `--fee tiered --maker-rate ... --taker-rate ...`
- `--fill-prob ... --fill-fade ... --fill-decay ...`
- `--mm-levels ... --mm-base-depth ... --mm-spread-pct ...`

`--realistic-fills` and `--bar-spread-bps` remain accepted as deprecated warn-noops.

**Bypass note** (repeated): These models are **not active** for live execution. Divergence between shadow (realistic sim) and live (venue truth) is expected and measured via `shadow_tracker`.

---

## Latency Models

Two layers (from instructions):
- `latency_model`: strategy decision → eligible for submission (simulates internal processing / decision delay).
- `wire_latency_model`: order submission → venue receipt (network + exchange processing).

Implemented in `src/execution/latency_model.h` + related. Configurable mean + stddev (microseconds). Stacks with other delays.

Used to model realistic fill timing and opportunity cost in backtest/shadow.

See: `test_latency_model.cpp`.

---

## Impact Models

- `SquareRootImpactModel` (primary): applied before aggression on market orders.
- Walked-book impact: when real L2 depth is available (`--depth-stream`), market orders can walk the book and produce one `fill_event` per crossed level (`--walked-book-impact`).
- In bar mode, the synthetic market-maker ladder is the source of modeled spread cost; calibrate it with `--mm-spread-pct` rather than `--bar-spread-bps`.

Impact is applied in execution adapter / orderbook paths. Measures slippage due to own order moving the market.

See: `test_impact_model.cpp`, `test_walked_book_impact.cpp`, `src/execution/` impact + `src/orderbook/fill_model.h`.

---

## Queue Models (Position + Maker)

Two primary uses:

1. **Shadow queue position** (`--queue-model l2-snapshot`):
   - `L2SnapshotQueueModel` (in `src/execution/queue_position_model.h`).
   - For adverse-selection honesty in shadow runs.
   - Uses depth20 snapshots to estimate position in queue at entry time; tracks how prints consume front.
   - Helps explain why a resting order "should" have filled (or not) vs. real tape.

2. **Maker / paper queue simulation** (backtest + shadow maker orders, `--maker-queue-model`):
   - `HybridPaperAdapter` routes **limits** to `QueueAwareBookAdapter` and **market/stop** to `LocalBookAdapter` (protective SL/TP and aggressive entries never silent-drop).
   - `IQueueModel` (`queue_model.h`): `uniform` (recommended), `front`, `back`.
   - Tracks `size_ahead` for passive orders; trade tape + L2 shrinkage update queue position.
   - **Paper tape:** bar/tick backtests feed synthetic `on_trade` prints into adapters so QueueAware limits can fill (bar close / tick price × qty). Without tape, passive limits would silent-no-op.
   - **DAY/EOS:** cancels route through the full adapter path (Hybrid/QueueAware), not book-only.
   - **Modify:** queue-held limits fail closed (no cancel-and-claim-success). True amend/resubmit is not implemented for QueueAware.
   - Requires `--depth-stream` for meaningful L2; without depth, the default is **conservative** — limits join with `size_ahead=+inf` (no `on_trade` fills at all), not front-of-queue. Bar/tick range sweep still fills passive limits on `[low,high]`. Opt-in `set_join_front_without_l2(true)` restores legacy optimistic join-front, but no CLI/production path currently exposes it.

See: `test_queue_model.cpp`, `test_queue_aware_adapter.cpp`, `test_backtest_defect_closure.cpp` (FR-01, Hybrid*).

---

## Fill Models

- Passive-side fill pricing is **always on** (resting counterparty price). `--realistic-fills` is a **deprecated warn-noop**.
- `--fill-prob` / `--fill-fade` / `--fill-decay`: probabilistic limit acceptance + pre-match qty fade (book and portfolio stay aligned).
- In shadow: `TradeTapeShadowAdapter` replays real trade tape against simulated orders.
- `test_realistic_fills.cpp`, `test_backtest_defect_closure.cpp` (FR-03).

Fill events carry rich metadata (opener, strategy, queue_position, futures flags, signed qty, etc.) for analytics + QuestDB.

---

## Fee Models

- `IFeeModel` (`src/execution/fee_model.h`): Zero, Fixed, Tiered.
- Flags: `--fee tiered --maker-rate 0.0002 --taker-rate 0.0004` (example).
- Applied on fills for realistic net P&L in backtest/shadow reports.
- Tiered uses venue-like maker/taker distinction.
- See: `test_fee_model.cpp`.

Funding is partially wired (events exist; full risk/P&L impact in progress — see todo R-03).

---

## Important Invariants & Limitations

- **Live bypass**: All realism models are compile/runtime bypassed for `engine_live`. Use only to measure *what would have happened* in shadow vs. real.
- L2 models require depth stream + sufficient history (L2 seeding).
- **Monte Carlo**: fee (`--fee`/`--maker-rate`/`--taker-rate`), order latency (`--order-latency-us`), and impact (`--impact-k-bps` + `--impact-adv`) are applied to **every trial**. Impact without ADV refuses to run (no silent ignore). Synthetic/MC bar dates are epoch-ms with 1-minute spacing.
- Never calibrate realism params from testnet (synthetic liquidity, fictional funding, resets, thin books).
- Queue/impact fidelity limited by data quality and model simplicity (no hidden liquidity, etc.).
- Zero default commission/impact when flags are omitted is intentional research default — report/export should show costs applied when set.

---

## Usage Example (shadow with realism)

```bash
./build/engine_shadow --provider binance-futures --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --persist --run-tag ... \
  --queue-model l2-snapshot --maker-queue-model uniform \
  --walked-book-impact --impact-k-bps 5 --impact-adv 1000000 \
  --order-latency-us 2000 --order-latency-stddev-us 500 \
  --fee tiered --maker-rate 0.0002 --taker-rate 0.0004
```

See full flag reference and MC section in `docs/reference/01-instructions.md`.

---

**See for current details**:
- Flag semantics + examples: `docs/reference/01-instructions.md` (§7) + `04-flags.md`
- High-level: `docs/reference/02-user-manual.md` (Realistic microstructure modeling)
- Tests exercising models: `tests/test_*_model.cpp` + `test_realistic_fills.cpp`
- Source: `src/execution/{latency,impact,queue*,fee}_model.*`, `src/execution/queue_aware...`, `src/orderbook/`
- Target arch context: `docs/architecture/01-target-architecture.md`
- MC caveats: `docs/reference/01-instructions.md` + `docs/todos/03-MC-simulation.md`

Thin synthesis only. Prefer source + instructions for implementation/usage. Update pointers on changes to models.
