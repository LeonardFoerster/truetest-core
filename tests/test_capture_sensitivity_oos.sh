#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
real_engine=${1:-}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/truetest-sensitivity-oos-contract.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

cat > "$test_root/engine_backtest" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

output=""
events=""
input=""
fee="tiered"
delay="1"
periods_per_year="525600"
dump_config=false
while (($#)); do
    case "$1" in
        --output) output=$2; shift 2 ;;
        --output-format) shift 2 ;;
        --log-events) events=$2; shift 2 ;;
        --dump-config) dump_config=true; shift ;;
        --path) input=$2; shift 2 ;;
        --fee) fee=$2; shift 2 ;;
        --exec-bar-delay) delay=$2; shift 2 ;;
        --periods-per-year) periods_per_year=$2; shift 2 ;;
        *) shift ;;
    esac
done
if $dump_config; then
    if [[ "${SENSITIVITY_TEST_MUTATE_INPUT:-0}" == 1 ]]; then
        printf '2026-01-01,MUTATED,3,3,3,3,3\n' >> "$input"
    fi
    printf '{"provider":"local","seed":424242,"fee":"%s","delay":%s,"periods_per_year":%s}\n' \
        "$fee" "$delay" "$periods_per_year"
    exit 0
fi
digest=$(sha256sum "$input" | awk '{print $1}')
printf '{"input":"%s","fee":"%s","delay":%s,"periods_per_year":%s}\n' \
    "$digest" "$fee" "$delay" "$periods_per_year" > "$output"
if [[ "${SENSITIVITY_TEST_PPY_EVENT_DRIFT:-0}" == 1 ]]; then
    printf 'event input=%s fee=%s delay=%s periods_per_year=%s\n' \
        "$digest" "$fee" "$delay" "$periods_per_year" > "$events"
else
    printf 'event input=%s fee=%s delay=%s\n' "$digest" "$fee" "$delay" > "$events"
fi
EOF
chmod +x "$test_root/engine_backtest"

compiler_path=$(command -v c++)
printf 'CMAKE_CXX_COMPILER:FILEPATH=%s\n' "$compiler_path" > "$test_root/CMakeCache.txt"
printf 'date,symbol,open,high,low,close,volume\n2024-01-01,TRAIN,1,1,1,1,1\n' > "$test_root/train.csv"
printf 'date,symbol,open,high,low,close,volume\n2025-01-01,OOS,2,2,2,2,2\n' > "$test_root/oos.csv"

git -C "$test_root" init -q
git -C "$test_root" add CMakeCache.txt engine_backtest train.csv oos.csv
git -C "$test_root" -c user.name=SensitivityTest -c user.email=sensitivity@example.invalid \
    commit -qm initial

( 
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/pass" \
        --study-status exploratory \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/oos.csv" \
        --variant baseline --factor baseline \
        --variant ppy-252 --factor periods-per-year --arg --periods-per-year --arg 252 \
        --variant fee-zero --factor fees --arg --fee --arg zero \
        --variant tight-spread --factor spread-impact --arg --mm-spread-pct --arg 0.0001 \
        --variant diagnostic-uncapped --factor risk-limits \
            --arg --risk-soft-limits --arg --max-daily-loss --arg 0 \
            --arg --max-gross-leverage --arg 0 \
        --variant delay-zero --factor execution-delay --arg --exec-bar-delay --arg 0 \
        -- ./engine_backtest --provider local --strategy sma --fee tiered --seed 424242 \
            --thread-preset inline --no-pin --no-risk-soft-limits \
            --max-daily-loss 80 --max-gross-leverage 1 >/dev/null
)

grep -qx 'schema=sensitivity-oos-v1' "$test_root/pass/campaign.manifest"
grep -qx 'status=PASS' "$test_root/pass/campaign.manifest"
grep -qx 'study_status=exploratory' "$test_root/pass/campaign.manifest"
grep -qx 'is_window_count=1' "$test_root/pass/campaign.manifest"
grep -qx 'oos_window_count=1' "$test_root/pass/campaign.manifest"
grep -qx 'variant_count=6' "$test_root/pass/campaign.manifest"
test "$(grep -c '^status=' "$test_root/pass/campaign.manifest")" -eq 1
grep -q 'window_kind=is window=train variant=ppy-252 factor=periods-per-year evidence_use=SENSITIVITY_EVIDENCE result_relation=DIFFERENT event_relation=IDENTICAL' "$test_root/pass/campaign.manifest"
grep -q 'window_kind=is window=train variant=fee-zero factor=fees evidence_use=SENSITIVITY_EVIDENCE result_relation=DIFFERENT event_relation=DIFFERENT' "$test_root/pass/campaign.manifest"
grep -q 'window_kind=oos window=future variant=delay-zero factor=execution-delay evidence_use=SENSITIVITY_EVIDENCE result_relation=DIFFERENT event_relation=DIFFERENT' "$test_root/pass/campaign.manifest"
grep -q 'variant=diagnostic-uncapped factor=risk-limits evidence_use=DIAGNOSTIC_ONLY' "$test_root/pass/campaign.manifest"
grep -q '^unsupported_controls=bar-constrained-vs-synthetic-fills,alternate-same-bar-ambiguity$' "$test_root/pass/parameters.freeze"
test -s "$test_root/pass/is-train/baseline/effective-config.json"
test -s "$test_root/pass/oos-future/fee-zero/run-1.events"
test "$(grep -o -- '--fee' "$test_root/pass/is-train/fee-zero/run-1.argv.sh" | wc -l)" -eq 1
grep -q -- '--risk-soft-limits' "$test_root/pass/is-train/diagnostic-uncapped/run-1.argv.sh"
if grep -q -- '--no-risk-soft-limits' "$test_root/pass/is-train/diagnostic-uncapped/run-1.argv.sh"; then
    echo "risk flag alias override left the hard baseline flag in argv" >&2
    exit 1
fi
for artifact in run-1.json run-2.json run-1.events run-2.events run-1.argv.sh \
    effective-config.json binary.sha256 compiler.sha256 input.sha256 environment.txt \
    git-status.txt working-tree.patch untracked.sha256 manifest.txt; do
    test -e "$test_root/pass/is-train/baseline/$artifact"
done

(
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/validated" \
        --study-status validated \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/oos.csv" \
        --oos-regime future \
        --variant baseline --factor baseline \
        --variant fee-zero --factor fees --arg --fee --arg zero \
        -- ./engine_backtest --provider local --strategy sma --seed 424242 \
            --thread-preset inline --no-pin >/dev/null
)
grep -qx 'study_status=validated' "$test_root/validated/campaign.manifest"
grep -qx 'status=PASS' "$test_root/validated/campaign.manifest"

if (
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/missing-regime" \
        --study-status validated \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/oos.csv" \
        --variant baseline --factor baseline \
        --variant fee-zero --factor fees --arg --fee --arg zero \
        -- ./engine_backtest --provider local --strategy sma --seed 424242 \
            --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "validated campaign without declared regime unexpectedly succeeded" >&2
    exit 1
fi

if (
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/unknown-regime" \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/oos.csv" \
        --oos-regime absent \
        --variant baseline --factor baseline \
        --variant fee-zero --factor fees --arg --fee --arg zero \
        -- ./engine_backtest --provider local --strategy sma --seed 424242 \
            --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "unknown OOS regime unexpectedly succeeded" >&2
    exit 1
fi

if (
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/deprecated-axis" \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/oos.csv" \
        --variant baseline --factor baseline \
        --variant inert --factor spread-impact --arg --bar-spread-bps --arg 10 \
        -- ./engine_backtest --provider local --strategy sma --seed 424242 \
            --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "deprecated spread control unexpectedly succeeded" >&2
    exit 1
fi

for legacy_control in --realistic-fills --bar-spread-bps; do
    legacy_args=("$legacy_control")
    if [[ "$legacy_control" == --bar-spread-bps ]]; then legacy_args+=(10); fi
    if (
        cd "$test_root"
        bash "$source_root/scripts/capture-sensitivity-oos.sh" \
            --output-dir "$test_root/base-legacy-${legacy_control#--}" \
            --is-window train="$test_root/train.csv" \
            --oos-window future="$test_root/oos.csv" \
            --variant baseline --factor baseline \
            --variant fee-zero --factor fees --arg --fee --arg zero \
            -- ./engine_backtest --provider local --strategy sma --seed 424242 \
                --thread-preset inline --no-pin "${legacy_args[@]}" >/dev/null 2>&1
    ); then
        echo "legacy base control $legacy_control unexpectedly succeeded" >&2
        exit 1
    fi
done

if (
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/missing-value" \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/oos.csv" \
        --variant baseline --factor baseline \
        --variant broken --factor fees --arg --fee \
        -- ./engine_backtest --provider local --strategy sma --seed 424242 \
            --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "factor option without a value unexpectedly succeeded" >&2
    exit 1
fi

if (
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/orphan-value" \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/oos.csv" \
        --variant baseline --factor baseline \
        --variant broken --factor fees --arg orphan --arg --fee --arg zero \
        -- ./engine_backtest --provider local --strategy sma --seed 424242 \
            --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "orphan factor value unexpectedly succeeded" >&2
    exit 1
fi

if (
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/conflicting-seed" \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/oos.csv" \
        --variant baseline --factor baseline \
        --variant fee-zero --factor fees --arg --fee --arg zero \
        -- ./engine_backtest --provider local --strategy sma --seed 424242 --seed 0 \
            --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "duplicate/conflicting seed unexpectedly succeeded" >&2
    exit 1
fi

for unsafe_control in --live=true --monte-carlo --web --api-secret=do-not-record; do
    if (
        cd "$test_root"
        bash "$source_root/scripts/capture-sensitivity-oos.sh" \
            --output-dir "$test_root/unsafe-${unsafe_control#--}" \
            --is-window train="$test_root/train.csv" \
            --oos-window future="$test_root/oos.csv" \
            --variant baseline --factor baseline \
            --variant fee-zero --factor fees --arg --fee --arg zero \
            -- ./engine_backtest --provider local --strategy sma --seed 424242 \
                --thread-preset inline --no-pin "$unsafe_control" >/dev/null 2>&1
    ); then
        echo "unsafe base control $unsafe_control unexpectedly succeeded" >&2
        exit 1
    fi
done

if (
    cd "$test_root"
    bash "$source_root/scripts/capture-sensitivity-oos.sh" \
        --output-dir "$test_root/same-data-validated" \
        --study-status validated \
        --is-window train="$test_root/train.csv" \
        --oos-window future="$test_root/train.csv" \
        --oos-regime future \
        --variant baseline --factor baseline \
        --variant fee-zero --factor fees --arg --fee --arg zero \
        -- ./engine_backtest --provider local --strategy sma --seed 424242 \
            --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "validated campaign with identical IS/OOS content unexpectedly succeeded" >&2
    exit 1
fi

if (
    cd "$test_root"
    SENSITIVITY_TEST_PPY_EVENT_DRIFT=1 \
        bash "$source_root/scripts/capture-sensitivity-oos.sh" \
            --output-dir "$test_root/ppy-event-drift" \
            --study-status regression-only \
            --is-window train="$test_root/train.csv" \
            --oos-window future="$test_root/oos.csv" \
            --variant baseline --factor baseline \
            --variant ppy-252 --factor periods-per-year \
                --arg --periods-per-year --arg 252 \
            -- ./engine_backtest --provider local --strategy sma --seed 424242 \
                --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "periods-per-year event-ledger drift unexpectedly succeeded" >&2
    exit 1
fi

cp "$test_root/train.csv" "$test_root/mutable.csv"
if (
    cd "$test_root"
    SENSITIVITY_TEST_MUTATE_INPUT=1 \
        bash "$source_root/scripts/capture-sensitivity-oos.sh" \
            --output-dir "$test_root/input-drift" \
            --study-status regression-only \
            --is-window train="$test_root/mutable.csv" \
            --oos-window future="$test_root/oos.csv" \
            --variant baseline --factor baseline \
            --variant fee-zero --factor fees --arg --fee --arg zero \
            -- ./engine_backtest --provider local --strategy sma --seed 424242 \
                --thread-preset inline --no-pin >/dev/null 2>&1
); then
    echo "input mutation after parameter freeze unexpectedly succeeded" >&2
    exit 1
fi

if [[ -n "$real_engine" ]]; then
    [[ -x "$real_engine" ]] || {
        echo "real engine_backtest is not executable: $real_engine" >&2
        exit 1
    }
    (
        cd "$source_root"
        bash "$source_root/scripts/capture-sensitivity-oos.sh" \
            --output-dir "$test_root/real-engine" \
            --study-status regression-only \
            --is-window fixture-is="$source_root/tests/golden/sma_basic.csv" \
            --oos-window fixture-oos="$source_root/tests/golden/sma_basic.csv" \
            --variant baseline --factor baseline \
            --variant ppy-252 --factor periods-per-year \
                --arg --periods-per-year --arg 252 \
            -- "$real_engine" --provider local --strategy sma --symbol GOLD \
                --balance 10000 --fee zero --seed 424242 --thread-preset inline \
                --no-pin --status-format off --no-tui >/dev/null
    )
    grep -qx 'status=PASS' "$test_root/real-engine/campaign.manifest"
    grep -q 'factor=periods-per-year evidence_use=SENSITIVITY_EVIDENCE result_relation=DIFFERENT event_relation=IDENTICAL' \
        "$test_root/real-engine/campaign.manifest"
fi
