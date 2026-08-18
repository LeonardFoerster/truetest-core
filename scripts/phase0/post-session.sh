#!/usr/bin/env bash
#
# Phase 0 post-session.sh
# After a live run, this helps collect the mandatory artifacts:
#   - compressed event log
#   - session-note.md (draft)
#   - target directory under reports/phase0/
#   - suggested PROGRESS.md row
#
# Usage:
#   ./scripts/phase0/post-session.sh p0_20260520_1430
#   ./scripts/phase0/post-session.sh p0_20260520_1430 /path/to/specific/event_log.bin

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <run-tag> [--symbol BTCUSDT] [--regime high|medium|low] [optional/path/to/event_log.bin]"
    echo "Example: $0 p0_20260520_1430 --symbol BTCUSDT --regime high"
    exit 1
fi

RUN_TAG="$1"
shift

SYMBOL="UNKNOWN"
REGIME="medium"
LOG_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --symbol) SYMBOL="$2"; shift 2 ;;
        --regime) REGIME="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 <run-tag> [--symbol SYMBOL] [--regime high|medium|low] [logpath]"
            exit 0
            ;;
        *)
            if [[ -z "$LOG_PATH" && -f "$1" ]]; then
                LOG_PATH="$1"
            fi
            shift
            ;;
    esac
done

# Try to find a recent event log if not supplied
if [[ -z "$LOG_PATH" ]]; then
    # Look for the most recent event_log*.bin or .bin.zst in common locations
    CANDIDATE=$(find "$REPO_ROOT" -maxdepth 3 -name "event_log*.bin*" -type f -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2- || true)
    if [[ -n "$CANDIDATE" ]]; then
        LOG_PATH="$CANDIDATE"
    fi
fi

# Derive directory name using symbol + regime when provided (matches new-session.sh convention)
TODAY=$(date -u +%Y-%m-%d)
SAFE_SYMBOL="${SYMBOL//,/_}"
TARGET_DIR="$REPO_ROOT/reports/phase0/${TODAY}_${SAFE_SYMBOL}_${REGIME}_${RUN_TAG}"

mkdir -p "$TARGET_DIR"

echo "=================================================================="
echo "PHASE 0 — POST SESSION ARTIFACT COLLECTOR"
echo "Run tag : ${RUN_TAG}"
echo "Target  : ${TARGET_DIR}"
echo "=================================================================="

# 1. Handle the event log
if [[ -n "$LOG_PATH" && -f "$LOG_PATH" ]]; then
    echo "Found log: $LOG_PATH"
    BASENAME=$(basename "$LOG_PATH")
    if [[ "$BASENAME" == *.zst ]]; then
        cp "$LOG_PATH" "$TARGET_DIR/"
        echo "  (already compressed) copied as-is"
    else
        echo "  Compressing with zstd..."
        zstd -T0 -q "$LOG_PATH" -o "$TARGET_DIR/${BASENAME}.zst"
        echo "  Created ${BASENAME}.zst"
    fi
else
    echo "WARNING: Could not locate an event_log for ${RUN_TAG}."
    echo "         You will need to manually copy + compress it into ${TARGET_DIR}"
fi

# 2. Generate a draft session-note.md (enhanced with analysis instructions)
NOTE="$TARGET_DIR/session-note.md"

cat > "$NOTE" <<NOTE_EOF
# Phase 0 Session Observation Note

**Run Tag**: ${RUN_TAG}
**Date / Time (UTC)**: $(date -u +"%Y-%m-%d %H:%M")
**Operator**: _______________________________
**Symbol(s)**: ${SYMBOL}
**Volatility Regime** (High/Med/Low): ${REGIME}

## Command Used (redact keys)

\`\`\`bash
./build/engine_live \\
  --provider binance-futures \\
  --symbol ${SYMBOL} \\
  --stream trade \\
  --depth-stream depth20@100ms \\
  --live \\
  --log-events ./event_log_${RUN_TAG}.bin \\
  --persist --run-tag ${RUN_TAG} \\
  --reconcile-tolerance-bps 3 \\
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \\
  --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 0.07 \\
  --max-daily-loss 80 --risk-unwind
\`\`\`

## Extract Real Metrics (Strongly Recommended)

After the run, replay the captured event log through the engine to obtain objective numbers for equity, drawdown, fills, etc.

Recommended analysis command (run from repo root, adjust strategy if you used a different one in the live session):

\`\`\`bash
./build/engine_backtest \\
  --replay ${TARGET_DIR}/$(basename "$LOG_PATH" 2>/dev/null || echo "event_log.bin.zst") \\
  --strategy mean-reversion \\
  --thread-preset light \\
  --output ${TARGET_DIR}/session-analytics.ndjson \\
  --output-format ndjson 2>&1 | tee ${TARGET_DIR}/replay-summary.txt
\`\`\`

Then open \`replay-summary.txt\` + the ndjson for:
- Final equity / PnL
- Max drawdown (maps to peak daily loss)
- Total fills / orders
- Any warnings emitted during replay

**Manually transcribe the live-specific observations** (DMS heartbeat, reconciler drift lines, halts, funding messages, TUI screenshots) from your terminal scrollback / operational log into the sections below.

## Key Metrics (fill from replay output + live terminal log)

- Start equity (USDT): _______________
- End equity (USDT): _______________
- Peak daily loss observed: _______________ / limit 80
- Max position notional during run: _______________
- Reconciler drift (max bps): _______________ (tolerance: 3)
- DMS heartbeat status at end: _______________ (advancing? Y/N)
- Any halts or risk-unwinds? _______________ (reason + recovery)
- Funding events observed: _______________ (P&L impact)
- QuestDB persisted? Y/N (note if soft-fail was accepted)

## Incident / Anomaly Log

(Include any POSITION-SNAPSHOT, funding, drift, rejection, or TUI warnings — paste exact lines from the live run)

1. ________________________________________________________________
2. ________________________________________________________________

## Root Cause & Operator Notes

(Especially anything that could explain drift > tolerance)

____________________________________________________________________
____________________________________________________________________

## Declaration

- [ ] All safety wires (DMS, reconciler, risk caps, daily-loss, one-way mode) were active for the entire session.
- [ ] Math-captcha terminal was visible the whole time (mainnet).
- [ ] No unexplained drift > tolerance.
- [ ] Full artifacts collected (event log + this note + replay summary).

**Operator Signature + Date**: _______________________________ / ________

**Batch Reviewer Sign-off**:
Name: ________________ Date: ________ Initials: ____

NOTE_EOF

echo ""
echo "Created draft note: $NOTE"
echo "(edit the blanks — the replay command above will give you objective equity/drawdown numbers)"
echo "After you run the replay command, copy key values from replay-summary.txt into the note."
echo ""

# 3. Suggest PROGRESS.md row + next steps
echo "------------------------------------------------------------------"
echo "SUGGESTED ROW FOR reports/phase0/PROGRESS.md  (copy & edit)"
echo "------------------------------------------------------------------"
cat <<PROG
|   | $(date -u +%Y-%m-%d) | ${RUN_TAG} | ${SYMBOL} | ${REGIME} | 15000 | [peak loss from replay] | [max drift] | Y (see ${TODAY}_${SAFE_SYMBOL}_${REGIME}_${RUN_TAG}) | N |  | [notes + DMS/funding status] |
PROG

echo ""
echo "After you:"
echo "  1. Run the replay command printed in the session-note.md"
echo "  2. Fill real numbers + incidents in the note"
echo "  3. Sign the note"
echo ""
echo "Then commit:"
echo "  git add reports/phase0/ && git commit -m \"phase0: session ${RUN_TAG} — ${REGIME} ${SYMBOL}\""
echo ""
echo "Next: run ./scripts/phase0/analyze-log.sh ${RUN_TAG} if you want a machine-readable summary extracted."
echo "=================================================================="
