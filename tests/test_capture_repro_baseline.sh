#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/truetest-repro-contract.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

cp "$source_root/scripts/capture-repro-baseline.sh" "$test_root/capture.sh"
chmod +x "$test_root/capture.sh"

cat > "$test_root/fake-engine" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
output=""
events=""
nondeterministic=false
dump_config=false
while (($#)); do
    case "$1" in
        --output) output=$2; shift 2 ;;
        --output-format) shift 2 ;;
        --log-events) events=$2; shift 2 ;;
        --dump-config) dump_config=true; shift ;;
        --nondeterministic) nondeterministic=true; shift ;;
        *) shift ;;
    esac
done
if $dump_config; then
    if [[ -n "${TT_FAKE_TRACE:-}" ]]; then
        echo "dump-config inherited trace environment" >&2
        exit 9
    fi
    printf '%s\n' '{"seed":424242,"thread_preset":"inline"}'
    exit 0
fi
value=424242
if $nondeterministic; then value=$$; fi
printf '{"value":%s}\n' "$value" > "$output"
printf 'event:%s\n' "$value" > "$events"
if [[ -n "${TT_FAKE_TRACE:-}" ]]; then
    printf 'trace:%s\n' "$value" > "$TT_FAKE_TRACE"
fi
EOF
chmod +x "$test_root/fake-engine"
printf '%s\n' fixture > "$test_root/input.csv"
compiler_path=$(command -v c++)
printf 'CMAKE_CXX_COMPILER:FILEPATH=%s\n' "$compiler_path" \
    > "$test_root/CMakeCache.txt"

git -C "$test_root" init -q
git -C "$test_root" add CMakeCache.txt capture.sh fake-engine input.csv
git -C "$test_root" -c user.name=BaselineTest \
    -c user.email=baseline@example.invalid commit -qm initial

(
    cd "$test_root"
    TT_FAKE_TRACE="$test_root/inherited.trace" CXX=/bin/echo \
        ./capture.sh --output-dir "$test_root/pass" --preset contract-test \
        --input input.csv --trace-env TT_FAKE_TRACE -- ./fake-engine >/dev/null
)
[[ ! -e "$test_root/inherited.trace" ]]
grep -qx 'status=CLEAN-CANDIDATE' "$test_root/pass/manifest.txt"
grep -qx 'result_reproducibility=PASS' "$test_root/pass/manifest.txt"
grep -qx 'event_log_reproducibility=PASS' "$test_root/pass/manifest.txt"
grep -qx 'trace_invariance=PASS' "$test_root/pass/manifest.txt"
grep -Fqx "compiler_path=$(realpath "$compiler_path")" \
    "$test_root/pass/manifest.txt"
if grep -Fq 'echo (GNU coreutils)' "$test_root/pass/manifest.txt"; then
    echo "runtime CXX overrode compiler provenance" >&2
    exit 1
fi
cmp -s "$test_root/pass/run-1.json" "$test_root/pass/run-2.json"
cmp -s "$test_root/pass/run-1.events" "$test_root/pass/run-2.events"

if (
    cd "$test_root"
    ./capture.sh --output-dir "$test_root/fail" --preset contract-test \
        --input input.csv -- ./fake-engine --nondeterministic >/dev/null 2>&1
); then
    echo "expected nondeterministic run to fail" >&2
    exit 1
fi
grep -qx 'result_reproducibility=FAIL' "$test_root/fail/manifest.txt"

if (
    cd "$test_root"
    ./capture.sh --output-dir "$test_root/reserved" --preset contract-test \
        --input input.csv -- ./fake-engine --output elsewhere.json >/dev/null 2>&1
); then
    echo "expected wrapper-owned --output option to be rejected" >&2
    exit 1
fi

if (
    cd "$test_root"
    ./capture.sh --output-dir "$test_root/pass" --preset contract-test \
        --input input.csv -- ./fake-engine >/dev/null 2>&1
); then
    echo "expected existing output directory to be rejected" >&2
    exit 1
fi
