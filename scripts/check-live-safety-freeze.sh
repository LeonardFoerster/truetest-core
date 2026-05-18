#!/usr/bin/env bash
#
# check-live-safety-freeze.sh
#
# Enforces the Phase 1 live-safety freeze defined in prod.md.
#
# Usage:
#   ./scripts/check-live-safety-freeze.sh                  # check HEAD~1 (normal pre-commit / CI use)
#   ./scripts/check-live-safety-freeze.sh --base <commit>  # check against arbitrary base
#   ./scripts/check-live-safety-freeze.sh --help
#
# Exit codes:
#   0 = OK (no frozen files touched, or token present)
#   1 = Violation (frozen file changed without LIVE_SAFETY_CCB_APPROVED token)
#
# The list of frozen files is the single source of truth for Phase 1.
# When new files are added to the freeze, update the FROZEN_FILES array below
# and the corresponding comment block in those files.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# =============================================================================
# FROZEN FILES — Phase 1 Live-Safety Surface (see prod.md Phase 1)
# Any modification to these paths requires the token in the commit message.
# =============================================================================
FROZEN_FILES=(
    "src/core/tt_target.h"
    "src/engine/engine.cpp"
    "src/providers/binance/binance_futures_provider.h"
    "src/providers/binance/binance_futures_dead_mans_switch.h"
    "src/providers/binance/binance_futures_kill_switch.h"
    "src/providers/binance/binance_futures_reconciler.h"
    "src/risk/risk_manager.h"
    "src/risk/futures_risk_check.h"
    "src/execution/live_safety.h"
    "src/threading/worker_watchdog.h"
)

REQUIRED_TOKEN="LIVE_SAFETY_CCB_APPROVED"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --base <commit>     Compare against this commit instead of HEAD~1
  --help              Show this help

The script fails (exit 1) if any file in the frozen list was modified in the
diff and the commit message does not contain the token: ${REQUIRED_TOKEN}
EOF
}

BASE="HEAD~1"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --base)
            BASE="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

# Get the commit message of the current HEAD (the commit being checked)
COMMIT_MSG=$(git log -1 --pretty=%B 2>/dev/null || echo "")

# Get list of changed files between BASE and HEAD
# Use --name-only and handle renames/deletes gracefully
CHANGED_FILES=$(git diff --name-only --diff-filter=ACMR "${BASE}" HEAD 2>/dev/null || true)

if [[ -z "$CHANGED_FILES" ]]; then
    # No changes (e.g. first commit or identical trees) — pass
    exit 0
fi

# Check whether any frozen file was touched
TOUCHED_FROZEN=()
for f in "${FROZEN_FILES[@]}"; do
    if echo "$CHANGED_FILES" | grep -Fxq "$f"; then
        TOUCHED_FROZEN+=("$f")
    fi
done

if [[ ${#TOUCHED_FROZEN[@]} -eq 0 ]]; then
    # No frozen files touched — pass
    exit 0
fi

# Frozen files were touched — require the magic token
if echo "$COMMIT_MSG" | grep -qF "$REQUIRED_TOKEN"; then
    # Token present — allowed
    echo "[check-live-safety-freeze] Frozen files changed with required token present. OK."
    exit 0
fi

# Violation
echo ""
echo "=================================================================="
echo "LIVE-SAFETY FREEZE VIOLATION (Phase 1 — see prod.md)"
echo "=================================================================="
echo ""
echo "The following protected files were modified:"
for f in "${TOUCHED_FROZEN[@]}"; do
    echo "  - $f"
done
echo ""
echo "Any change to these files requires explicit two-person CCB review"
echo "AND the commit message must contain the token:"
echo ""
echo "    $REQUIRED_TOKEN"
echo ""
echo "Example good commit message:"
echo "    feat(risk): tighten liquidation distance calc"
echo "    "
echo "    LIVE_SAFETY_CCB_APPROVED"
echo "    Reviewed-by: Alice <alice@example.com>"
echo "    Reviewed-by: Bob <bob@example.com>"
echo ""
echo "See prod.md Phase 1 and the comment blocks in the protected files."
echo "=================================================================="
echo ""

exit 1
