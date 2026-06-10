#!/usr/bin/env bash
# check-hotpath-json.sh
#
# Enforces prerequisites.md §11 "Hot-path JSON stays hand-rolled":
#   nlohmann/json is linked only for static config files and the C-API
#   result/config surface. Any #include <nlohmann/json.hpp> (or
#   nlohmann:: reference) outside the allow-list is a hot-path regression
#   and fails CI.
#
# The intent: keeps the engine core, providers, strategies, analytics, and
# threading code free of heap-allocating JSON so a later swap to simdjson
# (or hand-rolled parsing) stays a drop-in change.
#
# Run locally: scripts/check-hotpath-json.sh
# CI:          wired as the hotpath-json-check job in .github/workflows/ci.yml
#
# Exit 0 on clean pass; exit 1 on any hit outside the allow-list.
#
# Allow-list rationale:
#   * src/bin/main.inc           — CLI boot: parses ${config_path} once at
#     startup. Never invoked on the event loop.
#   * src/api/truetest_api.cpp   — C-API boundary: config-in, results-out
#     JSON for embedding hosts (Python/Node). Not on the hot path.
#   * tests/                     — test fixtures + golden-regression parser;
#     not shipped in any binary.

set -euo pipefail

ALLOW=(
    '^src/bin/main\.inc$'
    '^src/api/truetest_api\.cpp$'
    '^tests/'
    '^src/strategy/adaptive_hybrid_config\.cpp$'   # isolated ctor-time loader only (never hot path)
)

pattern='nlohmann/json\.hpp|nlohmann::'
hits=()

while IFS= read -r -d '' file; do
    if grep -qE "$pattern" "$file"; then
        ok=0
        for allow_re in "${ALLOW[@]}"; do
            if [[ "$file" =~ $allow_re ]]; then
                ok=1
                break
            fi
        done
        if (( ok == 0 )); then
            hits+=("$file")
        fi
    fi
done < <(find src tests \
            -type f \
            \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \
               -o -name '*.cc' -o -name '*.inc' \) \
            -print0)

if (( ${#hits[@]} > 0 )); then
    printf '\033[31m[hotpath-json-check] nlohmann/json used outside the allow-list:\033[0m\n' >&2
    for h in "${hits[@]}"; do
        printf '  - %s\n' "$h" >&2
        grep -nE "$pattern" "$h" >&2 | sed 's/^/      /'
    done
    printf '\nHot-path JSON must stay hand-rolled (snprintf for write, string\n' >&2
    printf 'extraction for read). Either move the usage into src/bin/main.inc /\n' >&2
    printf 'src/api/truetest_api.cpp, or extend the allow-list in\n' >&2
    printf 'scripts/check-hotpath-json.sh with a written justification.\n' >&2
    exit 1
fi

echo "hotpath-json-check: OK (nlohmann/json confined to the allow-list)"
