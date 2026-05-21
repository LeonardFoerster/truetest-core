#!/usr/bin/env bash
#
# Phase 0 create-evidence-bundle.sh
# Packages the entire reports/phase0/ tree (all session artifacts, PROGRESS, notes,
# batch reviews, volatility log, etc.) plus a machine-generated summary into a
# single tarball + markdown index. This is the final deliverable for Phase 0 exit.
#
# Run this after the 15th (or 18th) qualifying session and after the final batch review.
#
# Usage:
#   ./scripts/phase0/create-evidence-bundle.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

PHASE0_DIR="$REPO_ROOT/reports/phase0"
if [[ ! -d "$PHASE0_DIR" ]]; then
    echo "ERROR: $PHASE0_DIR does not exist"
    exit 1
fi

DATE=$(date -u +%Y%m%d_%H%M)
BUNDLE_NAME="phase0-evidence-bundle-${DATE}"
BUNDLE_DIR="$REPO_ROOT/${BUNDLE_NAME}"
BUNDLE_TAR="$REPO_ROOT/${BUNDLE_NAME}.tar.gz"

mkdir -p "$BUNDLE_DIR"

echo "=================================================================="
echo "PHASE 0 — EVIDENCE BUNDLE CREATOR"
echo "=================================================================="

# Copy the entire phase0 tree (preserves structure and all signed notes)
cp -a "$PHASE0_DIR" "$BUNDLE_DIR/reports-phase0"

# Generate a quick machine summary (counts, regimes, etc.)
SUMMARY="$BUNDLE_DIR/PHASE0_EVIDENCE_SUMMARY.md"

cat > "$SUMMARY" <<SUM
# Phase 0 Completion Evidence Bundle
**Generated**: $(date -u +"%Y-%m-%d %H:%M UTC")
**Bundle**: ${BUNDLE_NAME}

## Contents
- Full \`reports/phase0/\` tree (sessions, PROGRESS.md, batch reviews, templates, ops/)
- This summary file

## Quick Audit (run these commands on the unpacked tree)

\`\`\`bash
# Count qualifying sessions
grep -c "Y (see" reports/phase0/PROGRESS.md || echo "check PROGRESS.md manually"

# Regime distribution
grep -E "High|Medium|Low" reports/phase0/PROGRESS.md | cut -d'|' -f5 | sort | uniq -c

# List all session directories with signed notes
find reports/phase0 -name session-note.md | wc -l
\`\`\`

## Required for Phase 0 Exit (per prod.md)
- Minimum 15 rows in PROGRESS.md with "Y" in Artifacts and reviewer initials
- At least one (ideally 5+) session in each of High / Medium / Low
- Every session dir contains .bin.zst + signed session-note.md
- Final batch review package signed
- No unexplained drift > tolerance across the campaign

## Next Steps After Bundle
1. Attach this tarball + the signed final batch review to the Phase 0 exit declaration.
2. Update prod.md / todo.md / prerequisites.md with Phase 0 complete.
3. Proceed to the 8-hour mainnet shadow for Phase 1 sign-off.

*This bundle + the git history of reports/phase0/ is the authoritative evidence package.*
SUM

# Create the final tarball (zstd if available, else gzip)
if command -v zstd >/dev/null 2>&1; then
    tar -C "$(dirname "$BUNDLE_DIR")" -cf - "$(basename "$BUNDLE_DIR")" | zstd -T0 -q -o "${BUNDLE_TAR}.zst"
    echo "Created: ${BUNDLE_TAR}.zst"
else
    tar -czf "$BUNDLE_TAR" -C "$(dirname "$BUNDLE_DIR")" "$(basename "$BUNDLE_DIR")"
    echo "Created: $BUNDLE_TAR (gzip)"
fi

echo ""
echo "Bundle ready at: ${BUNDLE_TAR}*"
echo "Unpack and run the audit commands in the summary to verify completeness before declaring Phase 0 exit."
echo "=================================================================="