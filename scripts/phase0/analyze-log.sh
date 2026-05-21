#!/usr/bin/env bash
#
# Phase 0 analyze-log.sh
# Given a run-tag (and optionally the log path), prints a ready-to-run
# replay command that produces objective metrics (equity, drawdown, fills)
# from the binary event log using the engine's own replay + export machinery.
#
# Later iterations can parse the ndjson output and auto-fill the session note.
#
# Usage:
#   ./scripts/phase0/analyze-log.sh p0_20260520_1430
#   ./scripts/phase0/analyze-log.sh p0_20260520_1430 /path/to/event_log.bin.zst

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <run-tag> [optional/path/to/event_log.bin*]"
    exit 1
fi

RUN_TAG="$1"
LOG_PATH="${2:-}"

if [[ -z "$LOG_PATH" ]]; then
    CANDIDATE=$(find "$REPO_ROOT" -maxdepth 4 -path "*/reports/phase0/*${RUN_TAG}*" -name "event_log*.bin*" -type f 2>/dev/null | head -1 || true)
    if [[ -n "$CANDIDATE" ]]; then
        LOG_PATH="$CANDIDATE"
    fi
fi

if [[ -z "$LOG_PATH" || ! -f "$LOG_PATH" ]]; then
    echo "ERROR: Could not locate event log for ${RUN_TAG}."
    echo "Pass the full path as the second argument."
    exit 1
fi

# Find the canonical target dir if it exists
TODAY=$(date -u +%Y-%m-%d)
POSSIBLE_DIR=$(find "$REPO_ROOT/reports/phase0" -maxdepth 1 -type d -name "*${RUN_TAG}" 2>/dev/null | head -1 || true)
if [[ -n "$POSSIBLE_DIR" ]]; then
    OUT_DIR="$POSSIBLE_DIR"
else
    OUT_DIR="$REPO_ROOT/reports/phase0/${TODAY}_analyze_${RUN_TAG}"
    mkdir -p "$OUT_DIR"
fi

SUMMARY_TXT="$OUT_DIR/replay-summary.txt"
ANALYTICS_NDJSON="$OUT_DIR/session-analytics.ndjson"

echo "=================================================================="
echo "PHASE 0 — LOG ANALYZER"
echo "Run tag : ${RUN_TAG}"
echo "Log     : ${LOG_PATH}"
echo "Output  : ${OUT_DIR}"
echo "=================================================================="
echo ""

# Build the replay command (use light preset for speed; strategy is mostly irrelevant for portfolio/analytics replay of a live log)
cat <<CMD

# === COPY-PASTE THIS COMMAND ===
./build/engine_backtest \
  --replay "${LOG_PATH}" \
  --strategy mean-reversion \
  --thread-preset light \
  --output "${ANALYTICS_NDJSON}" \
  --output-format ndjson \
  2>&1 | tee "${SUMMARY_TXT}"

# After it finishes, open:
#   ${SUMMARY_TXT}          (human-readable report)
#   ${ANALYTICS_NDJSON}     (machine-readable equity curve, trades, etc.)

# Key values to look for in the output:
#   - Final equity / total PnL
#   - Max drawdown (for peak daily loss)
#   - Total orders / fills
#   - Any warnings or risk rejections printed during replay

CMD

echo ""
echo "When the replay completes, re-run the post-session helper or edit the session-note.md directly with the extracted numbers."
echo "You can also paste relevant excerpts from the live terminal log (DMS counter, drift lines, halts, funding) into the Incident section."
echo "=================================================================="

# Future: a small Python parser could read the ndjson here and emit a filled note or PROGRESS suggestion.
# For now this gives the operator the authoritative engine-derived numbers without duplicating parsing logic.