# Futures Testnet Validation Guide

**Status**: Active Doc Phase 1 guide for non-qualifying Binance Futures testnet drills. This guide does not replace Phase 0 evidence, and testnet runs do not count toward the 15 qualifying mainnet sessions.

**Authoritative gates**: `docs/governance/01-prod.md`, `docs/todos/01-P0-phase0.md`, and `reports/phase0/`.

**Last updated**: 2026-07-06 (created to realize D-02 and replace stale planned `futures-testnet.md` references).

---

## Purpose

Use Binance Futures testnet to rehearse the mechanics before tiny-size mainnet sessions:

- API credentials and provider startup.
- One-way mode refusal behavior.
- DMS heartbeat and venue countdown-cancel behavior.
- Reconciler refusal/warning paths.
- Kill-switch and shutdown handling.
- Artifact capture and post-session review.

Testnet is an operator drill and wiring check only. It has account resets, unrealistic liquidity, and different operational behavior from mainnet. It cannot satisfy Phase 0 qualifying evidence.

---

## Setup Checklist

- [ ] Build binaries with Binance and QuestDB enabled if available:

```bash
cmake -B build -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON
cmake --build build -j"$(nproc)"
```

- [ ] Export testnet credentials:

```bash
export BINANCE_FUTURES_TESTNET_KEY=...
export BINANCE_FUTURES_TESTNET_SECRET=...
```

- [ ] Confirm one-way position mode in the Binance Futures testnet UI.
- [ ] Start QuestDB if persistence is part of the drill.
- [ ] Open `docs/operations/01-futures-phase0-operator-sop.md` and `reports/phase0/templates/phase0-session-note.md` so the operator flow is rehearsed exactly.

---

## Conservative Testnet Command

Use `engine_live` only when intentionally testing signed testnet order paths. Use tiny values and add `--testnet`.

```bash
./build/engine_live \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade \
  --depth-stream depth20@100ms \
  --testnet \
  --live \
  --api-key "${BINANCE_FUTURES_TESTNET_KEY}" --api-secret "${BINANCE_FUTURES_TESTNET_SECRET}" \
  --persist --run-tag testnet_$(date -u +%Y%m%d_%H%M) \
  --reconcile-tolerance-bps 3 \
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \
  --max-notional 150 --max-leverage 2.5 --min-liq-distance-pct 7 \
  --max-daily-loss 5 --risk-unwind 0.4
```

For a no-order rehearsal, prefer `scripts/phase0/dry-run-phase0.sh` or `engine_shadow`.

---

## DMS / Kill-Switch Drills

Record run tag, timestamps, expected behavior, observed behavior, and artifacts for each drill.

| Scenario | Action | Expected result | Pass criteria |
|---|---|---|---|
| A. Clean shutdown | Start testnet run, then send SIGINT. | Engine halts, kill-switch cancels open orders, DMS disarms or expires harmlessly. | No unexpected open orders or positions after shutdown. |
| B. SIGKILL | Start testnet run, then `kill -9` the process. | Venue DMS countdown cancels open orders after heartbeat loss. | Open orders are gone after countdown window; positions reviewed manually. |
| C. Network loss | Temporarily block network or disconnect. | WS failure/halt is visible; DMS venue timer still protects orders. | Logs show loud halt path; orders cancel after countdown if heartbeat is lost. |
| D. SIGSTOP | Pause process with SIGSTOP long enough for DMS expiry, then resume. | DMS should fire at venue despite local process suspension. | Orders canceled by venue; operator documents that process suspension remains a live operational foot-gun. |
| E. Reconciler refusal | Create or simulate position/order drift, then start. | Startup refuses or produces the documented testnet-reset advisory path. | No silent startup through unexplained drift. |

Useful inspection aliases:

```bash
alias bf-orders='curl -s "$BINANCE_FAPI_TESTNET_BASE/fapi/v1/openOrders"'
alias bf-position='curl -s "$BINANCE_FAPI_TESTNET_BASE/fapi/v2/positionRisk"'
```

Use signed helper tooling instead of raw curl when signatures are required.

---

## Recording

Store drill notes under `reports/phase0/ops/` or a dated testnet subdirectory. Include:

- Command and run tag.
- Binary event log path, if produced.
- QuestDB run tag, if persistence was enabled.
- Open-order and position snapshots before and after each drill.
- Mandatory grep output:

```bash
grep -i "POSITION-SNAPSHOT\|funding\|drift\|reconcile\|halt\|kill\|DMS" <log-or-QuestDB-output> | tail -20
```

Testnet artifacts are useful readiness evidence, but they remain separate from Phase 0 qualifying mainnet artifacts.
