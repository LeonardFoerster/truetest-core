#!/usr/bin/env bash
# Enforces the deterministic RNG ownership boundary for backtest/simulation.
# Observability clocks remain legal outside the listed stochastic owners. The
# live operator challenge is isolated in a TT_TARGET_LIVE-only header, and
# QuestDB run-tag entropy is isolated in its observability-only source file.

set -euo pipefail

source_root="${TT_DETERMINISM_AUDIT_ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$source_root"

source_globs=(
    '--glob' '*.{h,hpp,cpp,cc,inc}'
)

standard_entropy_pattern='std::random_device|std::mt19937(_64)?|std::default_random_engine|std::seed_seq|std::(uniform|normal|lognormal|exponential|bernoulli|poisson|gamma|weibull|extreme_value|discrete|piecewise_constant|piecewise_linear)_[a-z_]*distribution'

unexpected_standard_entropy=()
while IFS= read -r hit; do
    [[ -z "$hit" ]] && continue
    case "$hit" in
        src/bin/live_operator_challenge.h:*'std::random_device random_device;'*) ;;
        src/bin/live_operator_challenge.h:*'std::mt19937 random_engine(random_device());'*) ;;
        src/bin/live_operator_challenge.h:*'std::uniform_int_distribution<int> challenge_operand(100, 9999);'*) ;;
        src/data/questdb/run_tag.cpp:*'std::mt19937_64 rng(test_seed);'*) ;;
        src/data/questdb/run_tag.cpp:*'std::random_device rd;'*) ;;
        src/data/questdb/run_tag.cpp:*'std::mt19937_64 rng(static_cast<std::uint64_t>(rd())'*) ;;
        *) unexpected_standard_entropy+=("$hit") ;;
    esac
done < <(rg -n "$standard_entropy_pattern" src "${source_globs[@]}" || true)

if (( ${#unexpected_standard_entropy[@]} > 0 )); then
    echo "deterministic-entropy-check: forbidden direct standard entropy/RNG use" >&2
    printf '  %s\n' "${unexpected_standard_entropy[@]}" >&2
    echo "Use reproducibility/DeterministicSeedDeriver and DeterministicRng." >&2
    exit 1
fi

# A centralized PRNG can still be defeated by feeding it wall-clock entropy.
# Audit all production sources for the two relevant source-to-seed shapes,
# including multiline constructor expressions. This is deliberately narrower
# than banning clocks: receipt, telemetry, live challenge, and timeout clocks
# remain legal when they are not coupled to a seed/RNG expression.
clock_seed_pattern='(seed|rng|entropy|random)[^;\n]*(system_clock|steady_clock|high_resolution_clock)::now|(system_clock|steady_clock|high_resolution_clock)::now[^;\n]*(seed|rng|entropy|random)|(seed|rng|entropy|random)[^;\n]*std::time[[:space:]]*\(|std::time[[:space:]]*\([^;\n]*(seed|rng|entropy|random)'
clock_seed_hits="$(rg -n -i "$clock_seed_pattern" src "${source_globs[@]}" || true)"
if [[ -n "$clock_seed_hits" ]]; then
    echo "deterministic-entropy-check: clock-derived seed/RNG expression" >&2
    printf '  %s\n' "$clock_seed_hits" >&2
    exit 1
fi

multiline_rng_clock_pattern='Deterministic(Rng|SeedDeriver)[[:space:]]*[({][^;{}]{0,512}((system_clock|steady_clock|high_resolution_clock)::now|std::time[[:space:]]*\()'
multiline_rng_clock_hits="$(rg -n -U "$multiline_rng_clock_pattern" src "${source_globs[@]}" || true)"
if [[ -n "$multiline_rng_clock_hits" ]]; then
    echo "deterministic-entropy-check: deterministic RNG constructed from a clock" >&2
    printf '  %s\n' "$multiline_rng_clock_hits" >&2
    exit 1
fi

# Process/thread identity and object addresses are entropy too, even when they
# are first cast to an integer or passed through std::hash. Keep this scan
# source-to-seed specific so ordinary PID logging and pointer ownership remain
# legal outside a seed expression.
identity_entropy='getpid|gettid|pthread_self|this_thread::get_id|thread::get_id|reinterpret_cast|uintptr_t|addressof|std::hash'
identity_seed_pattern="(seed|rng|entropy|random)[^;\n]{0,768}(${identity_entropy})|(${identity_entropy})[^;\n]{0,768}(seed|rng|entropy|random)"
identity_seed_hits="$(rg -n -U -i "$identity_seed_pattern" src "${source_globs[@]}" || true)"
if [[ -n "$identity_seed_hits" ]]; then
    echo "deterministic-entropy-check: process/thread/address-derived seed expression" >&2
    printf '  %s\n' "$identity_seed_hits" >&2
    exit 1
fi

multiline_rng_identity_pattern="Deterministic(Rng|SeedDeriver)[[:space:]]*[({][^;{}]{0,768}(${identity_entropy})"
multiline_rng_identity_hits="$(rg -n -U "$multiline_rng_identity_pattern" src "${source_globs[@]}" || true)"
if [[ -n "$multiline_rng_identity_hits" ]]; then
    echo "deterministic-entropy-check: deterministic RNG constructed from process/thread/address identity" >&2
    printf '  %s\n' "$multiline_rng_identity_hits" >&2
    exit 1
fi

deterministic_owners=(
    src/simulation
    src/providers/synthetic
    src/market_maker
    src/reproducibility
    src/execution/latency_model.h
    src/execution/impact_model.h
    src/execution/queue_model.h
    src/execution/execution_adapter.h
    src/execution/queue_aware_book_adapter.h
    src/orderbook/fill_model.h
)

existing_owners=()
for owner in "${deterministic_owners[@]}"; do
    [[ -e "$owner" ]] && existing_owners+=("$owner")
done

clock_pattern='(system_clock|steady_clock|high_resolution_clock)::now|std::time[[:space:]]*\('
unexpected_clocks=()
if (( ${#existing_owners[@]} > 0 )); then
    while IFS= read -r hit; do
        [[ -z "$hit" ]] && continue
        case "$hit" in
            src/simulation/monte_carlo_controller.cpp:*'auto start = std::chrono::steady_clock::now();'*) ;;
            src/simulation/monte_carlo_controller.cpp:*'auto end = std::chrono::steady_clock::now();'*) ;;
            src/execution/execution_adapter.h:*'std::chrono::steady_clock::now().time_since_epoch()).count();'*) ;;
            src/execution/queue_aware_book_adapter.h:*'std::chrono::steady_clock::now().time_since_epoch()).count();'*) ;;
            *) unexpected_clocks+=("$hit") ;;
        esac
    done < <(rg -n "$clock_pattern" "${existing_owners[@]}" "${source_globs[@]}" || true)
fi

if (( ${#unexpected_clocks[@]} > 0 )); then
    echo "deterministic-entropy-check: wall/monotonic clock entered a stochastic owner" >&2
    printf '  %s\n' "${unexpected_clocks[@]}" >&2
    echo "Clocks are allowed for receipts/telemetry, never seed or simulation state." >&2
    exit 1
fi

hash_hits="$(rg -n 'std::hash[[:space:]]*<' "${existing_owners[@]}" \
    "${source_globs[@]}" || true)"
if [[ -n "$hash_hits" ]]; then
    echo "deterministic-entropy-check: std::hash entered a deterministic owner" >&2
    printf '  %s\n' "$hash_hits" >&2
    echo "Use a versioned repository-owned hash/seed derivation." >&2
    exit 1
fi

echo "deterministic-entropy-check: OK (central seed/RNG boundary enforced)"
