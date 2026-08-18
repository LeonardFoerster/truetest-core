# Futures Phase 0 Operator SOP (Printable)

**Status**: Active Doc Phase 1 operator checklist. Authoritative gates remain in docs/governance/01-prod.md and reports/phase0/.

**Purpose**: One-page printable checklist for every tiny-size mainnet futures qualifying session (P0-03). Print, fill, sign. Use with `scripts/phase0/new-session.sh` + `post-session.sh`.

**Current status (2026)**: 0/15 qualifying. Collection paused during MC work (gates/ritual unchanged per todo.md + prod.md). All entries require clean 4h+ `engine_shadow` mainnet first.

**Last updated**: 2026-07-06 (path reconciled to numbered operations doc; references updated).

---

## Canonical Command Template (conservative; copy from new-session.sh)

```bash
export BINANCE_FUTURES_KEY=...
export BINANCE_FUTURES_SECRET=...
RUN_TAG="p0_$(date -u +%Y%m%d_%H%M)"

./build/engine_live \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --live \
  --api-key "${BINANCE_FUTURES_KEY}" --api-secret "${BINANCE_FUTURES_SECRET}" \
  --log-events "./event_log_${RUN_TAG}.bin" \
  --persist --run-tag "${RUN_TAG}" \
  --reconcile-tolerance-bps 3 \
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \
  --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 0.07 \
  --max-daily-loss 80 --risk-unwind
```

**Why each element (from prod.md)**:
- `--depth-stream depth20@100ms`: enables realistic queue/impact/L2 models in shadow.
- `--log-events`: binary zstd event log is the **mandatory durable truth**; leave event-log rotation disabled for authoritative replay.
- `--persist`: secondary QuestDB observability keyed by `--run-tag`.
- DMS (`--dead-man-*`): venue auto-cancel on heartbeat loss (protects orders only).
- Reconciler (`--reconcile-tolerance-bps 3`): blocks on drift > tol.
- Futures risk caps + daily-loss + unwind: layered pre-trade + post-fill safety.
- Tiny notional/low leverage: prove the *system*, not P&L.

**Exit criteria (Phase 0 → 1)**: 15+ sessions, ≥3 volatility regimes (High/Med/Low via `volatility-classifier.sh`), zero unexplained drift > tol, full artifacts per session, two-person batch sign-off every 5.

---

## Pre-Flight Checklist (all ✓ before `--live`)

- [ ] `engine_live` binary (correct TT_TARGET, safety features enabled)
- [ ] One-way mode confirmed (Binance UI + provider will refuse hedge)
- [ ] DMS heartbeat visible + advancing in TUI
- [ ] Math-captcha window open and will stay visible/attended entire session (mainnet)
- [ ] Conservative caps applied (notional/leverage/liq/daily-loss/unwind as template)
- [ ] Reconcile tol ≤3 bps; DMS timers set; `--log-events`, persist + run-tag active
- [ ] All referenced docs read (`prod.md` Phase 0 + this SOP + prerequisites)
- [ ] `new-session.sh` run; target reports/phase0/ dir ready
- [ ] Prior clean ≥4h `engine_shadow` mainnet run completed (no unexplained drift)

**Operator initials + timestamp**: ________ / ________

---

## During Session

- Physical presence at terminal the entire time. Never leave machine unattended with live orders.
- Monitor TUI continuously: DMS counter, risk panels, position snapshots, health, reconciler events.
- Keep math-captcha visible and attended (mainnet requirement).
- On any halt / large move / DMS trigger / funding / drift event: note immediately.
- **Post any halt before resume**: run mandatory grep (see below). Do not resume without clean review.

**Observations / events** (attach TUI snippets / log excerpts):

---

## Post-Halt / End-of-Session Mandatory Grep

```bash
grep -i "POSITION-SNAPSHOT\|funding\|drift\|reconcile\|halt\|kill\|DMS" <log-or-QuestDB-output> | tail -20
```

**Result** (paste clean/annotated; must be reviewed before next orders or close):

---

## Post-Session Actions

1. Run `./scripts/phase0/post-session.sh <run-tag>` (collects log, drafts note).
2. Run volatility classifier: `./scripts/phase0/volatility-classifier.sh` (on 7/14d BTC realized vol).
3. Fill + sign this note (session ID, regime, drift verdict, artifacts).
4. Commit: signed note + zstd binary event log + classifier output + QuestDB run_tag ref under `reports/phase0/`.
5. Append row to `reports/phase0/PROGRESS.md`.
6. (Every 5 sessions) Batch review + two signatures in ops/.

**Artifacts collected**:
- Binary event log: `.../p0_....zst` (mandatory durable truth)
- QuestDB run_tag: `p0_...`
- This signed note
- Volatility output

**Drift / fidelity verdict**: zero unexplained / acceptable / investigate

**Operator signature + timestamp**:

---

**Batch Reviewer Sign-off** (every 5 sessions; required for qualifying count):

- Reviewer 1: ________________ Date: ________
- Reviewer 2: ________________ Date: ________

**This session counts toward Phase 0 gate only after batch sign-off and PROGRESS update.**

---

**References** (do not duplicate long-form here):
- Full ritual + why: `docs/governance/01-prod.md` (Phase 0 section)
- Evidence layout + process: `reports/phase0/README.md`
- Session note template: `reports/phase0/templates/phase0-session-note.md`
- Task: `docs/todos/01-P0-phase0.md` P0-03 (high-level pointer in `docs/governance/03-todo.md`)
- Detailed steps + checklists: `docs/reference/01-instructions.md` (§ Phase 0 Qualifying Session Ritual)

Print this page, use on every session, update after each. "Future operators cannot say 'we forgot why we were careful.'"
