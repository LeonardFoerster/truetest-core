#!/usr/bin/env bash
set -euo pipefail

# Capture the deterministic evidence bundle required by docs/improvements/01.
# The command after -- must be a headless engine invocation. Result and event-log
# paths are owned by this wrapper so each process writes an independent artifact.

usage() {
    cat >&2 <<'EOF'
Usage:
  capture-repro-baseline.sh --output-dir DIR --preset NAME --input FILE \
    [--input FILE ...] [--trace-env NAME] -- COMMAND [ARGS...]

The wrapper appends --log-events, --output, and --output-format json. COMMAND
must therefore not contain those options. When --trace-env is supplied, a third
run sets NAME to the trace artifact path and must remain byte-identical.
EOF
    exit 2
}

fail() {
    echo "baseline: $*" >&2
    exit 1
}

output_dir=""
preset=""
trace_env=""
input_paths=()
command=()

while (($#)); do
    case "$1" in
        --output-dir)
            (($# >= 2)) || usage
            output_dir=$2
            shift 2
            ;;
        --preset)
            (($# >= 2)) || usage
            preset=$2
            shift 2
            ;;
        --input)
            (($# >= 2)) || usage
            input_paths+=("$2")
            shift 2
            ;;
        --trace-env)
            (($# >= 2)) || usage
            trace_env=$2
            shift 2
            ;;
        --)
            shift
            command=("$@")
            break
            ;;
        *)
            usage
            ;;
    esac
done

[[ -n "$output_dir" && -n "$preset" && ${#input_paths[@]} -gt 0 \
    && ${#command[@]} -gt 0 ]] || usage
[[ -x "${command[0]}" ]] || fail "command is not executable: ${command[0]}"
[[ -z "$trace_env" || "$trace_env" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] \
    || fail "invalid trace environment variable name: $trace_env"
[[ ! -e "$output_dir" ]] \
    || fail "output directory already exists (refusing to overwrite): $output_dir"

for input_path in "${input_paths[@]}"; do
    [[ -f "$input_path" ]] || fail "input file does not exist: $input_path"
done

for arg in "${command[@]:1}"; do
    case "$arg" in
        --output|--output=*|--output-format|--output-format=*|\
        --log-events|--log-events=*|--dump-config)
            fail "COMMAND contains wrapper-owned option: $arg"
            ;;
    esac
done

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) \
    || fail "must run from a Git working tree"
snapshot_dir=$(mktemp -d "${TMPDIR:-/tmp}/truetest-baseline-snapshot.XXXXXX")
trap 'rm -rf "$snapshot_dir"' EXIT

# Snapshot repository identity before creating an output directory, which may
# itself live under the working tree.
git status --short > "$snapshot_dir/git-status.txt"
git diff --binary HEAD > "$snapshot_dir/working-tree.patch"
git ls-files --others --exclude-standard -z \
    | while IFS= read -r -d '' path; do
        printf '%s  %s\n' "$(sha256sum "$repo_root/$path" | awk '{print $1}')" "$path"
    done > "$snapshot_dir/untracked.sha256"

if [[ -s "$snapshot_dir/git-status.txt" ]]; then
    tree_status="DIRTY-EXPLORATORY"
else
    tree_status="CLEAN-CANDIDATE"
fi

mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")
binary_path=$(realpath "${command[0]}")
binary_dir=$(dirname "$binary_path")
cmake_cache="$binary_dir/CMakeCache.txt"
[[ -f "$cmake_cache" ]] \
    || fail "CMakeCache.txt not found next to binary: $cmake_cache"
compiler_path=$(sed -n -E \
    's/^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=//p' "$cmake_cache" | head -n 1)
[[ -n "$compiler_path" && -x "$compiler_path" ]] \
    || fail "CMake compiler is missing or not executable: ${compiler_path:-UNSET}"
compiler_path=$(realpath "$compiler_path")
cp "$snapshot_dir/git-status.txt" "$snapshot_dir/working-tree.patch" \
    "$snapshot_dir/untracked.sha256" "$output_dir/"

baseline_prefix=()
if [[ -n "$trace_env" ]]; then
    baseline_prefix=(env -u "$trace_env")
fi

write_argv() {
    local path=$1
    shift
    printf '%q ' "$@" > "$path"
    printf '\n' >> "$path"
}

run_one() {
    local name=$1
    local result_path="$output_dir/$name.json"
    local event_path="$output_dir/$name.events"
    write_argv "$output_dir/$name.argv.sh" "${baseline_prefix[@]}" "${command[@]}" \
        --log-events "$event_path" --output "$result_path" --output-format json
    "${baseline_prefix[@]}" "${command[@]}" --log-events "$event_path" \
        --output "$result_path" --output-format json \
        > "$output_dir/$name.log" 2>&1
    [[ -s "$result_path" ]] || fail "$name produced no JSON result"
    [[ -s "$event_path" ]] || fail "$name produced no event log"
}

run_one run-1
run_one run-2

result_reproducibility="PASS"
event_log_reproducibility="PASS"
if ! cmp -s "$output_dir/run-1.json" "$output_dir/run-2.json"; then
    result_reproducibility="FAIL"
    diff -u "$output_dir/run-1.json" "$output_dir/run-2.json" \
        > "$output_dir/result.diff" || true
fi
if ! cmp -s "$output_dir/run-1.events" "$output_dir/run-2.events"; then
    event_log_reproducibility="FAIL"
    cmp -l "$output_dir/run-1.events" "$output_dir/run-2.events" \
        > "$output_dir/event-log.diff" || true
fi

trace_invariance="NOT-RUN"
if [[ -n "$trace_env" ]]; then
    trace_path="$output_dir/trace-enabled.trace"
    trace_result="$output_dir/trace-enabled.json"
    trace_events="$output_dir/trace-enabled.events"
    write_argv "$output_dir/trace-enabled.argv.sh" env "$trace_env=$trace_path" \
        "${command[@]}" --log-events "$trace_events" \
        --output "$trace_result" --output-format json
    env "$trace_env=$trace_path" "${command[@]}" --log-events "$trace_events" \
        --output "$trace_result" --output-format json \
        > "$output_dir/trace-enabled.log" 2>&1
    trace_invariance="PASS"
    if [[ ! -s "$trace_path" ]]; then
        trace_invariance="FAIL-NO-TRACE"
    elif ! cmp -s "$output_dir/run-1.json" "$trace_result" \
        || ! cmp -s "$output_dir/run-1.events" "$trace_events"; then
        trace_invariance="FAIL"
    fi
fi

write_argv "$output_dir/dump-config.argv.sh" "${baseline_prefix[@]}" \
    "${command[@]}" --dump-config
"${baseline_prefix[@]}" "${command[@]}" --dump-config \
    > "$output_dir/effective-config.json" \
    2> "$output_dir/dump-config.stderr"
[[ -s "$output_dir/effective-config.json" ]] \
    || fail "--dump-config produced no effective configuration"

sha256sum "$output_dir/run-1.json" > "$output_dir/result.sha256"
sha256sum "$output_dir/run-1.events" > "$output_dir/event-log.sha256"
sha256sum "$binary_path" > "$output_dir/binary.sha256"
sha256sum "$compiler_path" > "$output_dir/compiler.sha256"
for input_path in "${input_paths[@]}"; do
    absolute_input=$(realpath "$input_path")
    printf '%s  %s\n' "$(sha256sum "$absolute_input" | awk '{print $1}')" \
        "$absolute_input"
done > "$output_dir/input.sha256"

{
    for name in TZ LANG LC_ALL OMP_NUM_THREADS MALLOC_CONF ASAN_OPTIONS \
        TSAN_OPTIONS UBSAN_OPTIONS; do
        if [[ -v "$name" ]]; then
            printf '%s=%q\n' "$name" "${!name}"
        else
            printf '%s=UNSET\n' "$name"
        fi
    done
} > "$output_dir/environment.txt"

{
    printf 'status=%s\n' "$tree_status"
    printf 'result_reproducibility=%s\n' "$result_reproducibility"
    printf 'event_log_reproducibility=%s\n' "$event_log_reproducibility"
    printf 'trace_invariance=%s\n' "$trace_invariance"
    printf 'commit=%s\n' "$(git rev-parse HEAD)"
    printf 'preset=%s\n' "$preset"
    printf 'working_tree_patch_sha256=%s\n' \
        "$(sha256sum "$output_dir/working-tree.patch" | awk '{print $1}')"
    printf 'untracked_manifest_sha256=%s\n' \
        "$(sha256sum "$output_dir/untracked.sha256" | awk '{print $1}')"
    printf 'binary=%s\n' "$binary_path"
    printf 'binary_sha256=%s\n' "$(awk '{print $1}' "$output_dir/binary.sha256")"
    printf 'input_count=%s\n' "${#input_paths[@]}"
    printf 'compiler_path=%s\n' "$compiler_path"
    printf 'compiler_sha256=%s\n' "$(awk '{print $1}' "$output_dir/compiler.sha256")"
    printf 'compiler=%s\n' "$("$compiler_path" --version | head -n 1)"
    printf 'kernel=%s\n' "$(uname -srmo)"
    printf 'effective_config=%s\n' "$output_dir/effective-config.json"
} > "$output_dir/manifest.txt"

cat "$output_dir/manifest.txt"
if [[ "$result_reproducibility" != PASS \
    || "$event_log_reproducibility" != PASS \
    || "$trace_invariance" == FAIL* ]]; then
    fail "reproducibility checks failed; inspect $output_dir"
fi
