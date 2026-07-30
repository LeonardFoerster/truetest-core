# Phase 0 Session Note (Print & Sign)

**Session ID / run-tag**: p0_YYYYMMDD_HHMM  
**Date (UTC)**:  
**Operator**:  
**Symbol(s)**: BTCUSDT (or ...)  
**Regime** (per volatility-classifier.sh): High / Medium / Low  
**Command used** (copy from new-session.sh output):

```bash
./build/engine_live \
  --provider binance-futures \
  --symbol ... \
  ... (paste full command with your keys)
```

**Pre-flight checklist** (all must be ✓ before `--live`):
- [ ] `engine_live` binary built with correct TT_TARGET and safety features
- [ ] One-way mode confirmed in provider + UI
- [ ] DMS heartbeat visible and advancing in TUI
- [ ] Math-captcha window visible and attended the entire session
- [ ] Conservative caps applied (--max-notional, --max-leverage, --min-liq-distance-pct, --max-daily-loss, --risk-unwind)
- [ ] --reconcile-tolerance-bps ≤ 3, --dead-man-* values set
- [ ] Persist + run-tag active; binary log will be produced
- [ ] All referenced docs read (docs/governance/01-prod.md Phase 0 section + docs/operations/01-futures-phase0-operator-sop.md + this note)

**During session observations** (halt, large moves, DMS triggers, reconciler events, funding, etc.):

(attach or paste relevant TUI snippets / log excerpts)

**Post-halt mandatory grep** (run after every halt or end of session):
```bash
grep -i "POSITION-SNAPSHOT\|funding\|drift\|reconcile\|halt\|kill\|DMS" <log-or-QuestDB-output> | tail -20
```
Result: (paste clean or annotated output)

**Artifacts collected**:
- Binary event log: `.../p0_....zst`
- QuestDB run_tag: `p0_...`
- This signed note committed to `reports/phase0/`
- Volatility classification output

**Drift / fidelity verdict**: (zero unexplained / acceptable / investigate)

**Operator signature + timestamp**:

---

**Reviewer sign-off** (for batch reviews):

- Batch reviewer 1: ________________ Date: ________
- Batch reviewer 2: ________________ Date: ________

This note + artifacts count toward the Phase 0 gate only after batch sign-off and entry in PROGRESS.md.
