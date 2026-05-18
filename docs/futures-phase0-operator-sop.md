# Phase 0 Operator SOP — Safe Tiny-Size USDT-M Futures Mainnet Validation

**Version**: 1.0 (tied to prod.md Phase 0)  
**Status**: Mandatory for every live `engine_live --provider binance-futures` run until Phase 0 exit is declared.  
**Print this document, sign it, and keep a copy next to the trading terminal for every session.**

---

## 1. Purpose

This SOP codifies the **only currently approved way** to run real-money futures on mainnet while the engine is still maturing (see `prod.md` "Phase 0 — Safe Tiny-Size Operation").

Tiny size + full safety nets + disciplined logging = the only acceptable risk level today.

---

## 2. The Official Phase 0 Command Template

```bash
export BINANCE_FUTURES_KEY=...
export BINANCE_FUTURES_SECRET=...

./build/engine_live \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade \
  --depth-stream depth20@100ms \
  --live \
  --api-key "${BINANCE_FUTURES_KEY}" --api-secret "${BINANCE_FUTURES_SECRET}" \
  --persist --run-tag p0_$(date +%Y%m%d_%H%M) \
  --reconcile-tolerance-bps 3 \
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \
  --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 7 \
  --max-daily-loss 80 --risk-unwind 0.4
```

**You must use (or exceed) these safety settings** unless you have written justification and a second reviewer.

---

## 3. Pre-Session Checklist (Complete Before Typing `--live`)

- [ ] Binary was built with `-DENABLE_BINANCE=ON`
- [ ] `engine_live` binary (never `engine_shadow` or `engine_backtest` for real orders)
- [ ] Account is confirmed **one-way mode** in the Binance UI (the provider will refuse hedge mode)
- [ ] You have the **math-captcha terminal window** open and visible (mainnet only)
- [ ] QuestDB is reachable (or you consciously accept soft-fail and note it in the session log)
- [ ] Printed copy of this SOP + signature block below is on the desk
- [ ] TUI / dashboard will be monitored for the entire session
- [ ] Daily loss limit and venue risk caps are set conservatively for the capital tier

**Sign here before every session**:

Operator: _______________________________ Date/Time (UTC): _______________  
Reviewer (optional but recommended): _______________________________ Date: _______________

---

## 4. The Four Non-Negotiable Rules from prod.md (Must Be Verified Live)

1. **One-way mode confirmation**  
   The provider already refuses dual-side. Double-check in the Binance UI before the first order.

2. **DMS liveness counter advancing**  
   Before you submit the first strategy order, look at the TUI (Health or Debug panel) and confirm the dead-man's switch heartbeat counter is increasing.

3. **Math-captcha window visible the entire session** (mainnet)  
   Never minimize or cover the terminal that shows the captcha prompt.

4. **Post-halt review**  
   After **any** halt (risk, DMS, manual, reconciler, etc.), before considering resume or new orders, run:
   ```bash
   grep -i "POSITION-SNAPSHOT\|funding\|drift" <your-event-log>
   ```
   Review the output. Only proceed if you understand every line.

---

## 5. During-Session Discipline

- Stay at the terminal. Do not leave the machine unattended while orders are live.
- Watch the DMS heartbeat and risk panel.
- If the TUI shows any unexpected rejection or position snapshot warning, pause strategy and investigate.
- Never increase size mid-session beyond the caps you started with.
- If daily loss approaches 70-80% of the limit, consider flattening early.

---

## 6. Post-Session Artifact Collection (Mandatory for the Session to Count)

Within 30 minutes of stopping the engine:

1. Copy the binary event log (usually `event_log_*.bin` or the one written with your `--run-tag`).
2. Compress it if large: `zstd -T0 event_log_*.bin`
3. Note your exact QuestDB run tag (from the startup log or `--run-tag`).
4. Fill out a **one-page observation note** using the template:
   - `reports/phase0/templates/phase0-session-note.md`
5. Create a directory under `reports/phase0/` named `YYYY-MM-DD_SYMBOL_regime_p0_YYYYMMDD_HHMM`
6. Put the log, the filled note, and any screenshots inside it.
7. Add **one row** to `reports/phase0/PROGRESS.md`

Only sessions with complete artifacts + a filled note + a row in PROGRESS.md count toward the 15 required for Phase 0 exit.

---

## 7. Volatility Regime (for the 3-regime requirement)

Label the session **High / Medium / Low** based on recent BTC realized volatility (7-day or 14-day preferred).

A small helper script will be added later to make this more objective. For now, use a consistent external source and note which one you used in the session note.

---

## 8. Retroactive Sessions

Small mainnet runs performed **before** this SOP existed may still count if:
- The command line was close to the Phase 0 template
- You can still recover the event log + QuestDB data + write a retrospective note
- Drift was within tolerance and there were no major unexplained incidents

Mark them clearly as **"Retroactive"** in the PROGRESS tracker.

---

## 9. What to Do on First Sign of Trouble

- Any risk halt or daily-loss breach → follow the unwind + review rule (#4 above).
- DMS heartbeat stops advancing → treat as imminent kill-switch situation.
- Reconciler refuses at startup → do not force it. Investigate drift first.
- Unexpected funding P&L shock → note it, consider pausing until you understand the funding schedule for that symbol.

**When in doubt, flatten and review the logs before the next run.**

---

## 10. Sign-Off for This Session

I have read and followed this SOP in full for the run tagged `p0_________________`.

Operator Signature: _______________________________ Date/Time (UTC): _______________

Second Reviewer (batch or per session): _______________________________ Date: _______________

---

**Document Control**

- This SOP must be updated when prod.md Phase 0 parameters or rules change.
- Current authoritative source: `prod.md` (Phase 0 section) + this file.
- Print a fresh copy for every new batch of sessions.

**End of Phase 0 Operator SOP v1.0**
