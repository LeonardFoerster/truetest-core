#!/usr/bin/env bash
# check-mm-reference.sh
#
# Verifies that tests/golden/mm/expected.json still matches what the
# independent reference implementation produces from tests/golden/mm/cases.json
# and tests/golden/mm/reference_config.json.
#
# The C++ golden test (MMStrategyGolden.ReferenceExpectationsMatchTheEngine)
# compares the engine against the checked-in expected file. That only proves
# the engine agrees with a *file*; this script proves the file still agrees
# with the reference model, closing the loop. Both must pass.
#
# Run locally: scripts/check-mm-reference.sh
# CI:          run alongside the other check-*.sh gates
#
# Exit 0 when the expectations are current; 1 when they are stale (regenerate
# with `python3 tests/reference/mm_strategy_reference.py --write` and review
# the diff before committing).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "[mm-reference-check] $PYTHON not found; skipping" >&2
    exit 0
fi

cd "$REPO_ROOT"
if "$PYTHON" tests/reference/mm_strategy_reference.py --check; then
    echo "mm-reference: OK (golden expectations match the reference model)"
    exit 0
fi

echo "" >&2
echo "[mm-reference-check] tests/golden/mm/expected.json no longer matches the" >&2
echo "reference model. If the change was intentional, regenerate and review:" >&2
echo "  python3 tests/reference/mm_strategy_reference.py --write" >&2
exit 1
