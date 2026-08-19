#!/usr/bin/env bash
#
# check-live-safety-freeze.sh
#
# Enforces the Phase 1 live-safety freeze defined in prod.md.
#
# Usage:
#   ./scripts/check-live-safety-freeze.sh                  # check commit + index + worktree
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
    "src/engine/engine.h"
    "src/engine/engine_config.h"
    "src/engine/engine_lifecycle.cpp"
    "src/engine/engine_market.cpp"
    "src/engine/engine_orders.cpp"
    "src/engine/engine_fills.cpp"
    "src/engine/engine_workers.cpp"
    "src/engine/engine_observability.cpp"
    "src/engine/engine_pending.cpp"
    "src/engine/fill_processor.h"
    "src/engine/fill_processor.cpp"
    "src/engine/order_attribution_store.h"
    "src/engine/order_attribution_store.cpp"
    "src/engine/pending_order_scheduler.h"
    "src/engine/pending_order_scheduler.cpp"
    "src/engine/order_intent_processor.h"
    "src/engine/order_intent_processor.cpp"
    "src/engine/engine_hotpath_sink.h"
    "src/engine/risk_unwind_sink.h"
    "src/engine/live_safety_session.cpp"
    "src/engine/live_safety_session.h"
    "src/bin/main.inc"
    "src/bin/provider_open_policy.h"
    "src/execution/execution_bridge.h"
    "src/execution/fill_parser.h"
    "src/execution/async_support.h"
    "src/execution/order_transport.h"
    "src/providers/provider.h"
    "src/providers/bounded_ws_open.h"
    "src/providers/bounded_ws_frame_reader.h"
    "src/providers/data_bridge.h"
    "src/providers/recovery_payload.h"
    "src/providers/socket_readiness.h"
    "src/providers/thread_safe_callback.h"
    "src/providers/transport.h"
    "src/providers/binance/binance_transport.h"
    "src/providers/binance/binance_combined_transport.h"
    "src/providers/binance/binance_user_data_transport.h"
    "src/providers/binance/binance_provider.h"
    "src/providers/binance/binance_kill_switch.h"
    "src/providers/binance/binance_reconciler.h"
    "src/providers/binance/binance_rest_client.h"
    "src/providers/binance/binance_rest_order_transport.h"
    "src/providers/binance/binance_oco_bracket_adapter.h"
    "src/providers/binance/binance_futures_provider.h"
    "src/providers/binance/binance_futures_dead_mans_switch.h"
    "src/providers/binance/binance_futures_kill_switch.h"
    "src/providers/binance/binance_futures_reconciler.h"
    "src/providers/binance/binance_futures_user_data_parser.h"
    "src/providers/binance/binance_futures_register.cpp"
    "src/providers/binance/binance_futures_bracket_adapter.h"
    "src/providers/bitget/bitget_futures_provider.h"
    "src/providers/bitget/bitget_transport.h"
    "src/providers/bitget/bitget_combined_transport.h"
    "src/providers/bitget/bitget_private_ws_transport.h"
    "src/providers/bitget/bitget_futures_dead_mans_switch.h"
    "src/providers/bitget/bitget_futures_kill_switch.h"
    "src/providers/bitget/bitget_futures_reconciler.h"
    "src/providers/bitget/bitget_futures_user_data_parser.h"
    "src/providers/bitget/bitget_rest_client.h"
    "src/providers/bitget/bitget_rest_order_transport.h"
    "src/providers/bitget/bitget_futures_register.cpp"
    "src/providers/bitget/bitget_futures_bracket_adapter.h"
    "src/risk/risk_manager.h"
    "src/risk/futures_risk_check.h"
    "src/execution/live_safety.h"
    "src/threading/worker.h"
    "src/threading/worker_watchdog.h"
)

REQUIRED_TOKEN="LIVE_SAFETY_CCB_APPROVED"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --base <commit>     Compare against this commit instead of HEAD~1
  --help              Show this help

Committed and dirty changes are authorized independently. Every commit in the
selected range that touches a frozen file must carry ${REQUIRED_TOKEN} in its
own message. Dirty staged/unstaged/untracked frozen changes require the explicit
pre-commit acknowledgement LIVE_SAFETY_APPROVAL_TOKEN. That acknowledgement
never authorizes already-committed history.
EOF
}

BASE="HEAD~1"
BASE_EXPLICIT=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --base)
            BASE="$2"
            BASE_EXPLICIT=true
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

if ! HEAD_COMMIT=$(git rev-parse --verify 'HEAD^{commit}' 2>/dev/null); then
    echo "check-live-safety-freeze: HEAD is not a commit" >&2
    exit 1
fi

ROOT_RANGE=false
if BASE_COMMIT=$(git rev-parse --verify "${BASE}^{commit}" 2>/dev/null); then
    COMMIT_RANGE="${BASE_COMMIT}..${HEAD_COMMIT}"
elif [[ "$BASE_EXPLICIT" == false && "$BASE" == "HEAD~1" ]] \
    && ! git rev-parse --verify 'HEAD^' >/dev/null 2>&1; then
    ROOT_RANGE=true
    COMMIT_RANGE="$HEAD_COMMIT"
else
    echo "check-live-safety-freeze: invalid base commit: ${BASE}" >&2
    exit 1
fi

touches_frozen() {
    local changed_files="$1"
    local frozen
    for frozen in "${FROZEN_FILES[@]}"; do
        if grep -Fxq "$frozen" <<<"$changed_files"; then
            return 0
        fi
    done
    return 1
}

append_touched_frozen() {
    local changed_files="$1"
    local frozen
    for frozen in "${FROZEN_FILES[@]}"; do
        if grep -Fxq "$frozen" <<<"$changed_files"; then
            TOUCHED_FROZEN+=("$frozen")
        fi
    done
}

# Validate each touching commit independently. --no-renames intentionally
# exposes both the deleted source and added destination of a rename.
if [[ "$ROOT_RANGE" == true ]]; then
    COMMITS=("$HEAD_COMMIT")
else
    mapfile -t COMMITS < <(git rev-list --reverse "$COMMIT_RANGE")
fi

UNAUTHORIZED_COMMITS=()
TOUCHED_FROZEN=()
for commit in "${COMMITS[@]}"; do
    COMMIT_FILES=$(git diff-tree --root -m --no-commit-id --name-only -r \
        --no-renames --diff-filter=ACMRD "$commit" | sort -u)
    if touches_frozen "$COMMIT_FILES"; then
        append_touched_frozen "$COMMIT_FILES"
        COMMIT_MSG=$(git show -s --format=%B "$commit")
        if ! grep -qF "$REQUIRED_TOKEN" <<<"$COMMIT_MSG"; then
            UNAUTHORIZED_COMMITS+=("$commit")
        fi
    fi
done

# Dirty candidate state is deliberately separate from committed history.
DIRTY_FILES=$({
    git diff --cached --no-renames --name-only --diff-filter=ACMRD
    git diff --no-renames --name-only --diff-filter=ACMRD
    git ls-files --others --exclude-standard
} | sort -u)
DIRTY_FROZEN=()
for f in "${FROZEN_FILES[@]}"; do
    if grep -Fxq "$f" <<<"$DIRTY_FILES"; then
        DIRTY_FROZEN+=("$f")
        TOUCHED_FROZEN+=("$f")
    fi
done

if [[ ${#TOUCHED_FROZEN[@]} -eq 0 ]]; then
    exit 0
fi

mapfile -t TOUCHED_FROZEN < <(printf '%s\n' "${TOUCHED_FROZEN[@]}" | sort -u)

COMMITTED_OK=true
if [[ ${#UNAUTHORIZED_COMMITS[@]} -gt 0 ]]; then
    COMMITTED_OK=false
fi
DIRTY_OK=true
if [[ ${#DIRTY_FROZEN[@]} -gt 0 \
    && "${LIVE_SAFETY_APPROVAL_TOKEN:-}" != "$REQUIRED_TOKEN" ]]; then
    DIRTY_OK=false
fi

if [[ "$COMMITTED_OK" == true && "$DIRTY_OK" == true ]]; then
    echo "[check-live-safety-freeze] Frozen changes carry the required independent approvals. OK."
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
if [[ ${#UNAUTHORIZED_COMMITS[@]} -gt 0 ]]; then
    echo "Commits missing the required token:"
    for commit in "${UNAUTHORIZED_COMMITS[@]}"; do
        echo "  - $commit"
    done
    echo ""
fi
if [[ "$DIRTY_OK" == false ]]; then
    echo "Dirty frozen changes require LIVE_SAFETY_APPROVAL_TOKEN after recorded CCB approval."
    echo "A token in HEAD never authorizes later dirty changes."
    echo ""
fi
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
