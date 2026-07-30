#!/usr/bin/env bash
#
# Phase 0 new-session.sh
# Generates the canonical command line + target artifact directory
# for a new tiny-size mainnet futures validation run.
#
# Usage:
#   ./scripts/phase0/new-session.sh --symbol BTCUSDT --regime high
#   ./scripts/phase0/new-session.sh --symbol BTCUSDT,ETHUSDT --regime medium --notes "FOMC day"
#
# It prints:
#   1. The exact command you should copy-paste (with fresh run-tag)
#   2. The target reports/phase0/ directory name
#   3. A short pre-filled SOP checklist header you can print/sign
#
# This is the single source of truth for the Phase 0 command template
# (see docs/governance/01-prod.md for the current ritual + command template;
# printable SOP: docs/operations/01-futures-phase0-operator-sop.md).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

SYMBOL="BTCUSDT"
REGIME="medium"
NOTES=""
DATE_UTC=$(date -u +%Y%m%d_%H%M)
RUN_TAG="p0_${DATE_UTC}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --symbol) SYMBOL="$2"; shift 2 ;;
        --regime) REGIME="$2"; shift 2 ;;
        --notes) NOTES="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 --symbol BTCUSDT [--regime high|medium|low] [--notes 'text']"
            exit 0
            ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Canonical Phase 0 hardened template (prod.md)
# Adjust only if you have written justification + second reviewer.
CMD=$(cat <<EOF
export BINANCE_FUTURES_KEY=...
export BINANCE_FUTURES_SECRET=...

./build/engine_live \\
  --preset futures-phase0 \\
  --provider binance-futures \\
  --symbol ${SYMBOL} \\
  --stream trade \\
  --depth-stream depth20@100ms \\
  --live \\
  --api-key "\${BINANCE_FUTURES_KEY}" --api-secret "\${BINANCE_FUTURES_SECRET}" \\
  --persist --run-tag ${RUN_TAG}
EOF
)

DIR_NAME="reports/phase0/$(date -u +%Y-%m-%d)_${SYMBOL//,/_}_${REGIME}_${RUN_TAG}"

echo "=================================================================="
echo "PHASE 0 — NEW SESSION GENERATOR"
echo "=================================================================="
echo ""
echo "Run tag     : ${RUN_TAG}"
echo "Symbol(s)   : ${SYMBOL}"
echo "Regime      : ${REGIME}"
echo "Target dir  : ${DIR_NAME}"
if [[ -n "$NOTES" ]]; then
    echo "Notes       : ${NOTES}"
fi
echo ""
echo "------------------------------------------------------------------"
echo "COPY-PASTE COMMAND (replace the two export lines with real keys)"
echo "------------------------------------------------------------------"
echo ""
echo "$CMD"
echo ""
echo "------------------------------------------------------------------"
echo "TARGET ARTIFACT DIRECTORY (create after the run)"
echo "------------------------------------------------------------------"
echo "${DIR_NAME}"
echo ""
echo "------------------------------------------------------------------"
echo "PRE-SESSION SOP SIGN-OFF HEADER (print this + the full SOP)"
echo "------------------------------------------------------------------"
cat <<EOP

Phase 0 Operator SOP — Run Tag: ${RUN_TAG}
Date/Time (UTC): $(date -u +"%Y-%m-%d %H:%M")
Operator: _______________________________   Reviewer (opt): ________________

[ ] Binary is the freshly built engine_live with ENABLE_BINANCE=ON + ENABLE_QUESTDB=ON
[ ] engine_live (never shadow/backtest) for real orders
[ ] Account confirmed one-way mode in Binance futures UI
[ ] Math-captcha terminal window is open and will stay visible
[ ] QuestDB is reachable (or soft-fail is accepted and noted)
[ ] Printed SOP + this header is on the desk
[ ] --preset futures-phase0 (or equivalent DMS/risk caps/reconcile/risk-unwind) in use

I have read and will follow the full Phase 0 Operator SOP for this session.

Operator Signature: _______________________________   Date: ________

EOP

echo "=================================================================="
echo "Next steps:"
echo "  1. Copy the command above"
echo "  2. Print/sign the SOP checklist: docs/operations/01-futures-phase0-operator-sop.md"
echo "  3. After the run, run:  ./scripts/phase0/post-session.sh ${RUN_TAG}"
echo "=================================================================="
