#!/usr/bin/env bash
set -euo pipefail

# Run a deliberately small, backtest-only sensitivity/OOS campaign.  Each
# cell delegates to capture-repro-baseline.sh, so a result is accompanied by
# its exact argv, engine dump-config snapshot, input/binary/compiler hashes, and
# two independently produced JSON/event-ledger artifacts.
# The exact argv is authoritative for CLI controls that the engine's current
# dump-config schema does not serialize.
#
# This is evidence capture, not an optimiser or a promotion gate.  It refuses
# live/shadow binaries and ambiguous deprecated controls. OOS inputs are immutable
# caller-provided chronological slices: the runner records their digests but
# cannot infer chronology from arbitrary CSV dialects.

usage() {
    cat >&2 <<'EOF'
Usage:
  capture-sensitivity-oos.sh --output-dir DIR \
    [--study-status exploratory|validated|regression-only] \
    --is-window NAME=CSV [--is-window NAME=CSV ...] \
    --oos-window NAME=CSV [--oos-window NAME=CSV ...] \
    [--oos-regime OOS_NAME] \
    --variant baseline --factor baseline \
    --variant NAME --factor FACTOR [--arg ENGINE_ARGUMENT ...] \
    [--variant ...] -- engine_backtest BASE_ARGUMENTS...

Supported factors:
  periods-per-year  fees  spread-impact  execution-delay  risk-limits

The first variant must be the unmodified baseline.  Every other variant may
override only one supported factor; pass every extra engine token with --arg,
including option names.  For example:

  --variant ppy-252 --factor periods-per-year \
    --arg --periods-per-year --arg 252 \
  --variant delay-0 --factor execution-delay \
    --arg --exec-bar-delay --arg 0

BASE_ARGUMENTS must invoke engine_backtest with --provider local, a non-zero
--seed, --thread-preset inline, and --no-pin.  This runner owns --path,
--output, --output-format, --log-events, and --dump-config.

For a `validated` study, declare at least one --oos-regime that names one of
the supplied OOS windows.  The declaration is recorded before any run; it is
not a substitute for human review of chronological boundaries or conclusions.
EOF
    exit 2
}

fail() {
    echo "sensitivity-oos: $*" >&2
    exit 1
}

valid_name() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
}

parse_window() {
    local kind=$1 spec=$2
    local name=${spec%%=*}
    local path=${spec#*=}
    [[ "$name" != "$spec" && -n "$path" ]] || \
        fail "window must use NAME=CSV: $spec"
    valid_name "$name" || fail "invalid window name: $name"
    [[ "$path" == /* ]] || fail "window input must be an absolute path: $path"
    [[ -f "$path" ]] || fail "window input does not exist: $path"
    [[ -z "${window_seen[$name]:-}" ]] || fail "duplicate window name: $name"
    path=$(realpath "$path")
    window_seen[$name]=1
    window_kinds+=("$kind")
    window_names+=("$name")
    window_paths+=("$path")
    window_hashes+=("$(sha256sum "$path" | awk '{print $1}')")
}

required_single_option_value() {
    local option=$1
    shift
    local arg value=""
    local count=0
    while (($#)); do
        arg=$1
        shift
        if [[ "$arg" == "$option="* ]]; then
            value=${arg#*=}
            ((++count))
        elif [[ "$arg" == "$option" ]]; then
            (($# > 0)) || fail "$option requires a value"
            value=$1
            shift
            ((++count))
        fi
    done
    ((count == 1)) || fail "base command must contain exactly one $option"
    printf '%s' "$value"
}

require_single_flag() {
    local expected=$1
    shift
    local arg count=0
    for arg in "$@"; do
        [[ "$arg" == "$expected" ]] && ((++count))
    done
    ((count == 1)) || fail "base command must contain exactly one $expected"
}

forbidden_engine_argument() {
    case "$1" in
        --path|--path=*|--output|--output=*|--output-format|--output-format=*|\
        --log-events|--log-events=*|--dump-config|--config|--config=*|\
        --replay|--replay=*|--replay-from|--replay-from=*|--replay-to|--replay-to=*|\
        --live|--live=*|--mode|--mode=*|--monte-carlo|--monte-carlo=*|--mc-*|\
        --record|--record=*|--replay-data|--replay-data=*|\
        --checkpoint|--checkpoint=*|--checkpoint-interval|--checkpoint-interval=*|\
        --resume|--resume=*|--web|--web=*|--web-*|--desk|--desk=*|--desk-*|\
        --persist|--persist=*|--persist-strict|--persist-strict=*|--questdb-*|\
        --run-tag|--run-tag=*|--run-notes|--run-notes=*|\
        --log-file|--log-file=*|--log-max-size|--log-max-size=*|\
        --log-keep|--log-keep=*|--compress-log|--no-compress-log|--dry-run|--dry-run=*|\
        --api-key|--api-key=*|--api-secret|--api-secret=*|\
        --api-passphrase|--api-passphrase=*|--web-token|--web-token=*)
            return 0
            ;;
    esac
    return 1
}

factor_accepts_option() {
    local factor=$1 option=$2
    case "$factor:$option" in
        periods-per-year:--periods-per-year|periods-per-year:--periods-per-year=*) ;;
        fees:--fee|fees:--fee=*|fees:--fee-value|fees:--fee-value=*|\
        fees:--maker-rate|fees:--maker-rate=*|fees:--taker-rate|fees:--taker-rate=*) ;;
        spread-impact:--mm-spread-pct|spread-impact:--mm-spread-pct=*|\
        spread-impact:--mm-vol-mult|spread-impact:--mm-vol-mult=*|\
        spread-impact:--mm-max-spread-pct|spread-impact:--mm-max-spread-pct=*|\
        spread-impact:--mm-levels|spread-impact:--mm-levels=*|\
        spread-impact:--mm-base-depth|spread-impact:--mm-base-depth=*|\
        spread-impact:--impact-k-bps|spread-impact:--impact-k-bps=*|\
        spread-impact:--impact-adv|spread-impact:--impact-adv=*) ;;
        execution-delay:--exec-bar-delay|execution-delay:--exec-bar-delay=*|\
        execution-delay:--order-latency-us|execution-delay:--order-latency-us=*|\
        execution-delay:--order-latency-stddev-us|execution-delay:--order-latency-stddev-us=*|\
        execution-delay:--wire-latency-us|execution-delay:--wire-latency-us=*) ;;
        risk-limits:--max-gross-leverage|risk-limits:--max-gross-leverage=*|\
        risk-limits:--max-daily-loss|risk-limits:--max-daily-loss=*|\
        risk-limits:--daily-reset-hour|risk-limits:--daily-reset-hour=*|\
        risk-limits:--max-trades-per-hour|risk-limits:--max-trades-per-hour=*|\
        risk-limits:--max-orders-per-minute|risk-limits:--max-orders-per-minute=*|\
        risk-limits:--max-inventory-qty|risk-limits:--max-inventory-qty=*|\
        risk-limits:--max-funding-8h-rate|risk-limits:--max-funding-8h-rate=*|\
        risk-limits:--max-mark-age-ms|risk-limits:--max-mark-age-ms=*|\
        risk-limits:--risk-require-fresh-mark|risk-limits:--risk-soft-limits|\
        risk-limits:--no-risk-soft-limits) ;;
        *) return 1 ;;
    esac
    return 0
}

option_requires_value() {
    case "$1" in
        --periods-per-year|--fee|--fee-value|--maker-rate|--taker-rate|\
        --mm-spread-pct|--mm-vol-mult|--mm-max-spread-pct|--mm-levels|\
        --mm-base-depth|--impact-k-bps|--impact-adv|--exec-bar-delay|\
        --order-latency-us|--order-latency-stddev-us|--wire-latency-us|\
        --max-gross-leverage|--max-daily-loss|--daily-reset-hour|\
        --max-trades-per-hour|--max-orders-per-minute|--max-inventory-qty|\
        --max-funding-8h-rate|--max-mark-age-ms)
            return 0
            ;;
    esac
    return 1
}

canonical_option() {
    case "$1" in
        --no-risk-soft-limits) printf '%s' --risk-soft-limits ;;
        *) printf '%s' "${1%%=*}" ;;
    esac
}

validate_variant_arguments() {
    local name=$1 factor=$2
    shift 2
    local -a args=("$@")
    local arg option next
    local i=0
    declare -A seen=()

    while ((i < ${#args[@]})); do
        arg=${args[$i]}
        [[ "$arg" == --* ]] || fail "variant $name has orphan value: $arg"
        case "$arg" in
            --bar-spread-bps|--bar-spread-bps=*|--realistic-fills|--realistic-fills=*)
                fail "$arg is deprecated/inert as a fill control; use a supported factor instead"
                ;;
        esac
        forbidden_engine_argument "$arg" && \
            fail "variant $name overrides runner-owned/unsafe argument: $arg"
        case "$arg" in
            --provider|--provider=*)
                fail "variant $name must not change the local-provider baseline"
                ;;
        esac
        factor_accepts_option "$factor" "$arg" || \
            fail "argument $arg is not allowed for factor $factor"
        option=$(canonical_option "$arg")
        [[ -z "${seen[$option]:-}" ]] || fail "variant $name repeats option: $option"
        seen[$option]=1

        if [[ "$arg" == *=* ]]; then
            if option_requires_value "$option"; then
                [[ -n "${arg#*=}" ]] || fail "variant $name has empty value for $option"
            fi
        elif option_requires_value "$option"; then
            ((i + 1 < ${#args[@]})) || fail "variant $name is missing a value for $option"
            next=${args[$((i + 1))]}
            [[ "$next" != --* ]] || fail "variant $name is missing a value for $option"
            ((++i))
        fi
        ((++i))
    done
}

command_for_variant() {
    local extra_file=$1
    local -a extra=() overridden=() result=("${command[0]}")
    local arg option base_arg
    local i=1

    mapfile -d '' -t extra < "$extra_file"
    for arg in "${extra[@]}"; do
        if [[ "$arg" == --* ]]; then
            option=$(canonical_option "$arg")
            overridden+=("$option")
        fi
    done

    while ((i < ${#command[@]})); do
        base_arg=${command[$i]}
        option=$(canonical_option "$base_arg")
        replace=false
        for arg in "${overridden[@]}"; do
            if [[ "$option" == "$arg" ]]; then
                replace=true
                break
            fi
        done
        if "$replace"; then
            if [[ "$base_arg" != *=* ]] && option_requires_value "$option"; then
                ((++i))
            fi
        else
            result+=("$base_arg")
        fi
        ((++i))
    done
    result+=("${extra[@]}")
    printf '%s\0' "${result[@]}"
}

output_dir=""
study_status="exploratory"
command=()
current_variant=-1
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/truetest-sensitivity-oos.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

declare -A window_seen=()
declare -a window_kinds=() window_names=() window_paths=() window_hashes=() oos_regimes=()
declare -a variant_names=() variant_factors=() variant_arg_files=()

while (($#)); do
    case "$1" in
        --output-dir)
            (($# >= 2)) || usage
            output_dir=$2
            shift 2
            ;;
        --study-status|--classification)
            (($# >= 2)) || usage
            study_status=$2
            shift 2
            ;;
        --is-window)
            (($# >= 2)) || usage
            parse_window is "$2"
            shift 2
            ;;
        --oos-window)
            (($# >= 2)) || usage
            parse_window oos "$2"
            shift 2
            ;;
        --oos-regime)
            (($# >= 2)) || usage
            valid_name "$2" || fail "invalid OOS regime window: $2"
            oos_regimes+=("$2")
            shift 2
            ;;
        --variant)
            (($# >= 2)) || usage
            valid_name "$2" || fail "invalid variant name: $2"
            for existing in "${variant_names[@]}"; do
                [[ "$existing" != "$2" ]] || fail "duplicate variant name: $2"
            done
            variant_names+=("$2")
            variant_factors+=("")
            variant_arg_files+=("$work_dir/variant-${#variant_names[@]}.args")
            : > "${variant_arg_files[-1]}"
            current_variant=$((${#variant_names[@]} - 1))
            shift 2
            ;;
        --factor)
            (($# >= 2 && current_variant >= 0)) || usage
            [[ -z "${variant_factors[$current_variant]}" ]] || \
                fail "factor already set for variant ${variant_names[$current_variant]}"
            variant_factors[$current_variant]=$2
            shift 2
            ;;
        --arg)
            (($# >= 2 && current_variant >= 0)) || usage
            printf '%s\0' "$2" >> "${variant_arg_files[$current_variant]}"
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

[[ -n "$output_dir" && ${#command[@]} -gt 0 ]] || usage
case "$study_status" in exploratory|validated|regression-only) ;; *) usage ;; esac
[[ ! -e "$output_dir" ]] || fail "output directory already exists: $output_dir"
[[ -x "${command[0]}" ]] || fail "command is not executable: ${command[0]}"
[[ ! -L "${command[0]}" ]] || fail "engine_backtest path must not be a symbolic link"
binary_path=$(realpath "${command[0]}")
[[ "${command[0]##*/}" == "engine_backtest" && "${binary_path##*/}" == "engine_backtest" ]] || \
    fail "only the compile-time backtest binary is allowed"
binary_hash=$(sha256sum "$binary_path" | awk '{print $1}')
[[ ${#variant_names[@]} -ge 2 ]] || fail "baseline plus at least one variant are required"
[[ "${variant_names[0]}" == "baseline" && "${variant_factors[0]}" == "baseline" ]] || \
    fail "the first variant must be '--variant baseline --factor baseline'"
[[ ${#window_names[@]} -gt 0 ]] || fail "at least one --is-window is required"

oos_count=0
for kind in "${window_kinds[@]}"; do [[ "$kind" == oos ]] && ((++oos_count)); done
((oos_count > 0)) || fail "at least one --oos-window is required"

for regime in "${oos_regimes[@]}"; do
    found=false
    for i in "${!window_names[@]}"; do
        if [[ "${window_kinds[$i]}" == oos && "${window_names[$i]}" == "$regime" ]]; then
            found=true
            break
        fi
    done
    "$found" || fail "--oos-regime does not name an OOS window: $regime"
done
if [[ "$study_status" == validated && ${#oos_regimes[@]} -eq 0 ]]; then
    fail "validated studies require --oos-regime NAME before any run"
fi
if [[ "$study_status" == validated ]]; then
    for i in "${!window_names[@]}"; do
        [[ "${window_kinds[$i]}" == is ]] || continue
        for j in "${!window_names[@]}"; do
            [[ "${window_kinds[$j]}" == oos ]] || continue
            [[ "${window_hashes[$i]}" != "${window_hashes[$j]}" ]] || \
                fail "validated IS/OOS windows must not have identical content: ${window_names[$i]} and ${window_names[$j]}"
        done
    done
fi

for arg in "${command[@]:1}"; do
    forbidden_engine_argument "$arg" && fail "base command contains runner-owned/unsafe argument: $arg"
    case "$arg" in
        --bar-spread-bps|--bar-spread-bps=*|--realistic-fills|--realistic-fills=*)
            fail "base command contains unsupported legacy control: $arg"
            ;;
    esac
done
provider=$(required_single_option_value --provider "${command[@]:1}")
[[ "$provider" == local ]] || fail "base command must contain '--provider local'"
thread_preset=$(required_single_option_value --thread-preset "${command[@]:1}")
[[ "$thread_preset" == inline ]] || \
    fail "base command must contain '--thread-preset inline'"
seed=$(required_single_option_value --seed "${command[@]:1}")
[[ "$seed" =~ ^[1-9][0-9]*$ ]] || fail "base command must contain a non-zero --seed"
require_single_flag --no-pin "${command[@]:1}"

for i in "${!variant_names[@]}"; do
    name=${variant_names[$i]}
    factor=${variant_factors[$i]}
    [[ -n "$factor" ]] || fail "missing --factor for variant: $name"
    mapfile -d '' -t extra < "${variant_arg_files[$i]}"
    if ((i == 0)); then
        [[ ${#extra[@]} -eq 0 ]] || fail "baseline must not add arguments"
        continue
    fi
    case "$factor" in
        periods-per-year|fees|spread-impact|execution-delay|risk-limits) ;;
        *) fail "unsupported factor for $name: $factor" ;;
    esac
    [[ ${#extra[@]} -gt 0 ]] || fail "variant $name has no factor arguments"
    validate_variant_arguments "$name" "$factor" "${extra[@]}"
done

script_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
capture_script="$script_root/capture-repro-baseline.sh"
[[ -x "$capture_script" ]] || fail "missing executable baseline capture wrapper: $capture_script"

mkdir -p "$output_dir"
freeze_file="$output_dir/parameters.freeze"
{
    printf 'study_status=%s\n' "$study_status"
    printf 'chronology=DECLARED_BY_CALLER_PRE_RUN\n'
    printf 'unsupported_controls=bar-constrained-vs-synthetic-fills,alternate-same-bar-ambiguity\n'
    printf 'binary_path=%q\n' "$binary_path"
    printf 'binary_sha256=%s\n' "$binary_hash"
    printf 'base_command='
    printf '%q ' "${command[@]}"
    printf '\n'
    for i in "${!window_names[@]}"; do
        printf 'window kind=%s name=%s path=%q sha256=%s\n' \
            "${window_kinds[$i]}" "${window_names[$i]}" "${window_paths[$i]}" \
            "${window_hashes[$i]}"
    done
    for regime in "${oos_regimes[@]}"; do
        printf 'oos_regime=%s\n' "$regime"
    done
    for i in "${!variant_names[@]}"; do
        printf 'variant name=%s factor=%s argv=' \
            "${variant_names[$i]}" "${variant_factors[$i]}"
        mapfile -d '' -t extra < "${variant_arg_files[$i]}"
        if ((${#extra[@]})); then printf '%q ' "${extra[@]}"; fi
        printf '\n'
    done
} > "$freeze_file"
freeze_sha=$(sha256sum "$freeze_file" | awk '{print $1}')

manifest="$output_dir/campaign.manifest"
{
    printf 'schema=sensitivity-oos-v1\n'
    printf 'status=IN_PROGRESS\n'
    printf 'study_status=%s\n' "$study_status"
    printf 'parameter_freeze_sha256=%s\n' "$freeze_sha"
    printf 'is_window_count=%s\n' "$((${#window_names[@]} - oos_count))"
    printf 'oos_window_count=%s\n' "$oos_count"
    printf 'variant_count=%s\n' "${#variant_names[@]}"
    printf 'conclusion_stability=NOT_ASSESSED\n'
    printf 'unsupported_controls=bar-constrained-vs-synthetic-fills,alternate-same-bar-ambiguity\n'
} > "$manifest"

assert_campaign_inputs_unchanged() {
    local i actual
    [[ "$(sha256sum "$binary_path" | awk '{print $1}')" == "$binary_hash" ]] || \
        fail "engine_backtest changed after parameters were frozen"
    [[ "$(sha256sum "$freeze_file" | awk '{print $1}')" == "$freeze_sha" ]] || \
        fail "parameters.freeze changed during campaign execution"
    for i in "${!window_paths[@]}"; do
        actual=$(sha256sum "${window_paths[$i]}" | awk '{print $1}')
        [[ "$actual" == "${window_hashes[$i]}" ]] || \
            fail "window input changed after parameters were frozen: ${window_paths[$i]}"
    done
}

for wi in "${!window_names[@]}"; do
    kind=${window_kinds[$wi]}
    window=${window_names[$wi]}
    input=${window_paths[$wi]}
    window_dir="$output_dir/${kind}-${window}"
    mkdir -p "$window_dir"

    baseline_dir=""
    for vi in "${!variant_names[@]}"; do
        variant=${variant_names[$vi]}
        factor=${variant_factors[$vi]}
        cell_dir="$window_dir/$variant"
        mapfile -d '' -t extra < "${variant_arg_files[$vi]}"
        mapfile -d '' -t cell_command < <(command_for_variant "${variant_arg_files[$vi]}")
        assert_campaign_inputs_unchanged
        "$capture_script" \
            --output-dir "$cell_dir" \
            --preset "sensitivity-oos:${kind}:${window}:${variant}" \
            --input "$input" \
            -- "${cell_command[@]}" --path "$input"
        assert_campaign_inputs_unchanged

        if ((vi == 0)); then
            baseline_dir=$cell_dir
        fi
        if cmp -s "$baseline_dir/run-1.json" "$cell_dir/run-1.json"; then
            result_relation=IDENTICAL
        else
            result_relation=DIFFERENT
        fi
        if cmp -s "$baseline_dir/run-1.events" "$cell_dir/run-1.events"; then
            event_relation=IDENTICAL
        else
            event_relation=DIFFERENT
        fi
        if [[ "$factor" == periods-per-year && "$event_relation" != IDENTICAL ]]; then
            fail "periods-per-year changed the event ledger for ${kind}:${window}:${variant}"
        fi
        evidence_use=SENSITIVITY_EVIDENCE
        if [[ "$factor" == baseline ]]; then evidence_use=BASELINE_EVIDENCE; fi
        if [[ "$factor" == risk-limits ]]; then evidence_use=DIAGNOSTIC_ONLY; fi
        printf 'window_kind=%s window=%s variant=%s factor=%s evidence_use=%s result_relation=%s event_relation=%s result_sha256=%s event_sha256=%s input_sha256=%s config_sha256=%s argv_sha256=%s cell_manifest_sha256=%s binary_sha256=%s compiler_sha256=%s environment_sha256=%s dirty_patch_sha256=%s untracked_sha256=%s\n' \
            "$kind" "$window" "$variant" "$factor" "$evidence_use" "$result_relation" "$event_relation" \
            "$(awk '{print $1}' "$cell_dir/result.sha256")" \
            "$(awk '{print $1}' "$cell_dir/event-log.sha256")" \
            "$(awk '{print $1}' "$cell_dir/input.sha256")" \
            "$(sha256sum "$cell_dir/effective-config.json" | awk '{print $1}')" \
            "$(sha256sum "$cell_dir/run-1.argv.sh" | awk '{print $1}')" \
            "$(sha256sum "$cell_dir/manifest.txt" | awk '{print $1}')" \
            "$(awk '{print $1}' "$cell_dir/binary.sha256")" \
            "$(awk '{print $1}' "$cell_dir/compiler.sha256")" \
            "$(sha256sum "$cell_dir/environment.txt" | awk '{print $1}')" \
            "$(sha256sum "$cell_dir/working-tree.patch" | awk '{print $1}')" \
            "$(sha256sum "$cell_dir/untracked.sha256" | awk '{print $1}')" >> "$manifest"
    done
done

assert_campaign_inputs_unchanged
complete_manifest="$work_dir/campaign.manifest.complete"
awk '{ if ($0 == "status=IN_PROGRESS") print "status=PASS"; else print }' \
    "$manifest" > "$complete_manifest"
{
    printf 'result=EVIDENCE_CAPTURED_NOT_PROMOTED\n'
    printf 'promotion_requires=human-review-of-chronology,regime-portability,and-disclosed-conclusion-stability\n'
} >> "$complete_manifest"
mv "$complete_manifest" "$manifest"

cat "$manifest"
