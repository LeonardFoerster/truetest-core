# Realism Models (Latency / Impact / Queue / Fill / Fee)

**Status**: Thin extraction / planned skeleton (Doc Phase).

**Planned / extracted; see reference/ for current** authoritative details and flag usage. Full material lives in `docs/reference/02-user-manual.md`, `docs/reference/01-instructions.md` (§7), tests (e.g. `test_latency_model.cpp`, `test_impact_model.cpp`, `test_queue_model.cpp`, `test_fill_model.cpp`, `test_fee_model.cpp`, `test_realistic_fills.cpp`), and source (`src/execution/*_model.*`, `src/orderbook/`).

**All models default off**; require explicit flags. **Completely bypassed in live** (live venue supplies truth). Realism is for backtest + shadow divergence measurement only.

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
- `--realistic-fills`
- `--order-latency-us N --order-latency-stddev-us M`
- `--impact-k-bps`
- `--bar-spread-bps`
- `--queue-model l2-snapshot`
- `--maker-queue-model uniform|front|back`
- `--fee tiered --maker-rate ... --taker-rate ...`
- `--fill-model ...`

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
- Walked-book impact: when real L2 depth available (`--depth-stream`), market orders "walk the book" producing one `fill_event` per level crossed (with `--realistic-fills`).
- Bar-spread: for bar-mode (OHLCV) data, simulates realistic bid-ask spread on market orders (`--bar-spread-bps`); suppressed on L2 symbols.

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
   - `QueueAwareBookAdapter` + `IQueueModel` (`src/execution/queue_aware_book_adapter.h`, `queue_model.h`).
   - Models: `uniform` (recommended default), `front`, `back` (or `BackCancelModel`).
   - Tracks `size_ahead` for passive orders.
   - Real prints consume front of queue; L2 shrinkage triggers modeled cancels per chosen policy.
   - Enables realistic passive fill probability without over-optimism.

See: `test_queue_model.cpp`, `test_queue_position_model.cpp`, `test_queue_aware_adapter.cpp`.

---

## Fill Models

- `--realistic-fills`: enables passive/resting price logic + multi-level walked fills (one event per price level).
- `FillModel` (`src/orderbook/fill_model.h`): partial-fill probability modeling on the synthetic orderbook.
- In shadow: `TradeTapeShadowAdapter` replays real trade tape against simulated orders for divergence tracking (`shadow_tracker`).
- `test_realistic_fills.cpp`, `test_bridge_unknown_fill.cpp`, golden regressions.

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
- MC uses the same models (via synthetic provider) but with stylized L2 (constant spread + noise; see todo MC-03).
- Never calibrate realism params from testnet (synthetic liquidity, fictional funding, resets, thin books).
- Queue/impact fidelity limited by data quality and model simplicity (no hidden liquidity, etc.).
- Full per-trial MC order lifecycle + richer QuestDB still in progress (MC-06).

---

## Usage Example (shadow with realism)

```bash
./build/engine_shadow --provider binance-futures --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --persist --run-tag ... \
  --queue-model l2-snapshot --maker-queue-model uniform \
  --realistic-fills --impact-k-bps 5 \
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
- MC caveats: `docs/reference/01-instructions.md` + root `todo.md` (MC-*)

Thin synthesis only. Prefer source + instructions for implementation/usage. Update pointers on changes to models.