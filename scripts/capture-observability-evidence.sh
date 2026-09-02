#!/usr/bin/env bash
set -euo pipefail

# Build the Plan 08 test-component evidence bundle. The selected GoogleTest
# runs once with buffered trace collection disabled and once enabled; metrics
# and effective configuration must remain byte-identical across processes.

usage() {
    cat >&2 <<'EOF'
Usage:
  capture-observability-evidence.sh --output-dir DIR \
    --test-binary /absolute/path/truetest_tests \
    --input /absolute/path/observability_one_trade.csv

This is TEST_COMPONENT_HARNESS evidence. It does not instrument or certify
production engine routing, live/shadow paths, or per-rule RiskManager passes.
EOF
    exit 2
}

fail() {
    echo "observability-evidence: $*" >&2
    exit 1
}

output_dir=""
test_binary=""
input_path=""
original_argv=("$0" "$@")

while (($#)); do
    case "$1" in
        --output-dir)
            (($# >= 2)) || usage
            output_dir=$2
            shift 2
            ;;
        --test-binary)
            (($# >= 2)) || usage
            test_binary=$2
            shift 2
            ;;
        --input)
            (($# >= 2)) || usage
            input_path=$2
            shift 2
            ;;
        *) usage ;;
    esac
done

[[ -n "$output_dir" && -n "$test_binary" && -n "$input_path" ]] || usage
[[ "$test_binary" == /* && "$input_path" == /* ]] \
    || fail "test binary and input paths must be absolute"
[[ -x "$test_binary" && ! -L "$test_binary" ]] \
    || fail "test binary must be an executable, non-symlink file"
[[ "${test_binary##*/}" == truetest_tests ]] \
    || fail "test binary basename must be truetest_tests"
[[ -f "$input_path" ]] || fail "input file does not exist: $input_path"
[[ ! -e "$output_dir" ]] || fail "output directory already exists: $output_dir"

test_binary=$(realpath "$test_binary")
input_path=$(realpath "$input_path")
script_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output_dir=$(realpath -m "$output_dir")
case "$output_dir/" in
    "$script_root/"*) fail "output directory must be outside the source tree" ;;
esac
output_parent=$(dirname "$output_dir")
output_name=$(basename "$output_dir")
[[ -n "$output_name" && "$output_name" != . && "$output_name" != .. ]] \
    || fail "invalid output directory"
mkdir -p "$output_parent"
output_parent=$(realpath "$output_parent")
output_dir="$output_parent/$output_name"
case "$output_dir/" in
    "$script_root/"*) fail "output directory must be outside the source tree" ;;
esac

stage=$(mktemp -d "$output_parent/.observability-evidence.XXXXXX")
work=$(mktemp -d "${TMPDIR:-/tmp}/truetest-observability-evidence.XXXXXX")
cleanup() {
    [[ -z "${stage:-}" ]] || rm -rf "$stage"
    rm -rf "$work"
}
trap cleanup EXIT

disabled="$work/disabled"
enabled="$work/enabled"
mkdir "$disabled" "$enabled"
filter='ObservabilityEvidence.OneTradeLinksPhysicalInputToReconciledReport'

snapshot_untracked() {
    local destination=$1
    git -C "$script_root" ls-files --others --exclude-standard -z |
        while IFS= read -r -d '' path; do
            digest=$(sha256sum "$script_root/$path" | awk '{print $1}')
            printf '%s  %q\n' "$digest" "$path"
        done > "$destination"
}

# Resolve and fingerprint every executable/input/repository source before any
# test code runs. The same fingerprints are recomputed after both processes.
binary_dir=$(dirname "$test_binary")
cmake_cache="$binary_dir/CMakeCache.txt"
[[ -f "$cmake_cache" ]] || fail "CMakeCache.txt not found next to test binary"
compiler_path=$(sed -n -E \
    's/^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=//p' "$cmake_cache" | head -n 1)
[[ -n "$compiler_path" && -x "$compiler_path" ]] \
    || fail "configured C++ compiler is unavailable"
compiler_path=$(realpath "$compiler_path")

sha256sum "$input_path" > "$work/input.before.sha256"
sha256sum "$test_binary" > "$work/binary.before.sha256"
sha256sum "$compiler_path" > "$work/compiler.before.sha256"
sha256sum "$cmake_cache" > "$work/cmake-cache.before.sha256"
git_commit_before=$(git -C "$script_root" rev-parse HEAD)
git -C "$script_root" diff --binary HEAD > "$work/working-tree.before.patch"
git -C "$script_root" status --short > "$work/git-status.before.txt"
snapshot_untracked "$work/untracked.before.sha256"

env -u TT_OBSERVABILITY_TRACE -u TT_OBSERVABILITY_INPUT \
    -u TT_OBSERVABILITY_EVIDENCE_DIR \
    TT_OBSERVABILITY_TRACE=0 \
    TT_OBSERVABILITY_INPUT="$input_path" \
    TT_OBSERVABILITY_EVIDENCE_DIR="$disabled" \
    "$test_binary" --gtest_filter="$filter" \
    > "$work/disabled.test-results.txt" 2>&1

env -u TT_OBSERVABILITY_TRACE -u TT_OBSERVABILITY_INPUT \
    -u TT_OBSERVABILITY_EVIDENCE_DIR \
    TT_OBSERVABILITY_TRACE=1 \
    TT_OBSERVABILITY_INPUT="$input_path" \
    TT_OBSERVABILITY_EVIDENCE_DIR="$enabled" \
    "$test_binary" --gtest_filter="$filter" \
    > "$work/enabled.test-results.txt" 2>&1

cmp -s "$disabled/metrics.json" "$enabled/metrics.json" \
    || fail "trace-enabled metrics differ from trace-disabled metrics"
cmp -s "$disabled/effective-config.json" "$enabled/effective-config.json" \
    || fail "trace-enabled effective config differs from trace-disabled config"
cmp -s "$disabled/semantic-result.csv" "$enabled/semantic-result.csv" \
    || fail "trace-enabled canonical report differs from trace-disabled report"

artifacts=(
    effective-config.json
    signals.csv
    orders.csv
    risk-decisions.csv
    fills.csv
    trade-reconciliation.csv
    metrics.json
    semantic-result.csv
    bars.csv
    indicators.csv
    execution.csv
    positions.csv
    exits.csv
    completeness.csv
)
for artifact in "${artifacts[@]}"; do
    [[ -s "$enabled/$artifact" ]] || fail "trace did not produce $artifact"
    cp "$enabled/$artifact" "$stage/$artifact"
done

{
    printf '=== trace disabled ===\n'
    cat "$work/disabled.test-results.txt"
    printf '\n=== trace enabled ===\n'
    cat "$work/enabled.test-results.txt"
} > "$stage/test-results.txt"

sha256sum "$input_path" > "$stage/input.sha256"
sha256sum "$test_binary" > "$stage/binary.sha256"
sha256sum "$compiler_path" > "$stage/compiler.sha256"
sha256sum "$cmake_cache" > "$stage/cmake-cache.sha256"

git_commit=$(git -C "$script_root" rev-parse HEAD)
git_status=$(git -C "$script_root" status --short)
if [[ -n "$git_status" ]]; then
    tree_status=DIRTY-EXPLORATORY
else
    tree_status=CLEAN-CANDIDATE
fi
git -C "$script_root" diff --binary HEAD > "$stage/working-tree.patch"
git -C "$script_root" status --short > "$stage/git-status.txt"
snapshot_untracked "$stage/untracked.sha256"

cmp -s "$work/input.before.sha256" "$stage/input.sha256" \
    || fail "input changed during evidence capture"
cmp -s "$work/binary.before.sha256" "$stage/binary.sha256" \
    || fail "test binary changed during evidence capture"
cmp -s "$work/compiler.before.sha256" "$stage/compiler.sha256" \
    || fail "compiler changed during evidence capture"
cmp -s "$work/cmake-cache.before.sha256" "$stage/cmake-cache.sha256" \
    || fail "CMake cache changed during evidence capture"
[[ "$git_commit_before" == "$git_commit" ]] \
    || fail "git commit changed during evidence capture"
cmp -s "$work/working-tree.before.patch" "$stage/working-tree.patch" \
    || fail "tracked working tree changed during evidence capture"
cmp -s "$work/git-status.before.txt" "$stage/git-status.txt" \
    || fail "git status changed during evidence capture"
cmp -s "$work/untracked.before.sha256" "$stage/untracked.sha256" \
    || fail "untracked files changed during evidence capture"

{
    printf 'TZ=%s\n' "${TZ-<unset>}"
    printf 'LANG=%s\n' "${LANG-<unset>}"
    printf 'LC_ALL=%s\n' "${LC_ALL-<unset>}"
    printf 'OMP_NUM_THREADS=%s\n' "${OMP_NUM_THREADS-<unset>}"
    printf 'MALLOC_CONF=%s\n' "${MALLOC_CONF-<unset>}"
    printf 'ASAN_OPTIONS=%s\n' "${ASAN_OPTIONS-<unset>}"
    printf 'TSAN_OPTIONS=%s\n' "${TSAN_OPTIONS-<unset>}"
    printf 'UBSAN_OPTIONS=%s\n' "${UBSAN_OPTIONS-<unset>}"
} > "$stage/environment.txt"

{
    "$compiler_path" --version
    cmake --version
    git --version
    printf 'bash %s\n' "$BASH_VERSION"
    uname -a
} > "$stage/toolchain.txt"

{
    printf '%q ' "${original_argv[@]}"
    printf '\n'
} > "$stage/capture.argv.sh"

input_display=$(printf '%q' "$input_path")
binary_display=$(printf '%q' "$test_binary")
compiler_display=$(printf '%q' "$compiler_path")

{
    printf '# TrueTest observability evidence manifest\n\n'
    printf -- '- schema: `truetest-observability-evidence-v1`\n'
    printf -- '- status: `EVIDENCE_CAPTURED_WITH_UNVERIFIED_LINKS`\n'
    printf -- '- scope: `TEST_COMPONENT_HARNESS`\n'
    printf -- '- trace invariance: `PASS` (fresh-process config, metrics, complete AnalyticsReport, and selected final harness-state byte identity)\n'
    printf -- '- production engine trace: `UNVERIFIED`\n'
    printf -- '- per-rule risk pass sequence: `UNVERIFIED`\n'
    printf -- '- physical-row carriage into engine: `UNVERIFIED`\n'
    printf -- '- tree status: `%s`\n' "$tree_status"
    printf -- '- commit: `%s`\n' "$git_commit"
    printf -- '- created at UTC: `%s`\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf -- '- input (shell-escaped): %s\n' "$input_display"
    printf -- '- test binary (shell-escaped): %s\n' "$binary_display"
    printf -- '- compiler (shell-escaped): %s\n' "$compiler_display"
    printf -- '- CMake cache digest: `cmake-cache.sha256`\n'
    printf -- '- toolchain details: `toolchain.txt`\n'
    printf -- '- untracked snapshot: `untracked.sha256`\n'
    printf -- '- selected environment: `environment.txt`\n\n'
    printf '## Provenance vocabulary\n\n'
    printf -- '- `PRODUCTION_RETURN_VALUE`: returned by the named production component.\n'
    printf -- '- `PRODUCTION_STATE_SNAPSHOT`: read through a production accessor.\n'
    printf -- '- `INDEPENDENT_ORACLE`: independently recomputed and asserted against production output.\n'
    printf -- '- `HARNESS_ASSIGNED`: identity or timing assigned by the deterministic harness.\n'
    printf -- '- `HARNESS_JOIN`: correlation performed by explicit IDs inside this test harness.\n\n'
    printf -- '- `HARNESS_DERIVED`: deterministic harness arithmetic over provenance-labelled source fields.\n\n'
    printf '## Completeness\n\n'
    printf 'See `completeness.csv`. Missing production links are `UNVERIFIED`; they are never inferred.\n\n'
    printf '## Integrity boundary\n\n'
    printf '`artifacts.sha256` covers this manifest and every captured artifact except the checksum file itself. '
    printf 'It detects accidental or post-capture edits when checked against an externally trusted copy; it is not a signature or authenticity proof.\n'
} > "$stage/manifest.md"

checksum_files=(
    manifest.md effective-config.json input.sha256 binary.sha256 cmake-cache.sha256 signals.csv
    orders.csv risk-decisions.csv fills.csv trade-reconciliation.csv metrics.json
    semantic-result.csv test-results.txt bars.csv indicators.csv execution.csv positions.csv exits.csv
    completeness.csv compiler.sha256 toolchain.txt working-tree.patch git-status.txt
    untracked.sha256 environment.txt capture.argv.sh
)
(
    cd "$stage"
    sha256sum "${checksum_files[@]}"
) > "$stage/artifacts.sha256"

[[ ! -e "$output_dir" ]] || fail "output directory appeared during capture: $output_dir"
mv -T "$stage" "$output_dir"
stage=""
cat "$output_dir/manifest.md"
