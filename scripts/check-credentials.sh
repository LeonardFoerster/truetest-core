#!/usr/bin/env bash
# check-credentials.sh
#
# Enforces prerequisites.md §10 "Physically isolate live credentials":
#   api_key / api_secret identifiers are only allowed in files explicitly
#   authorized to handle credential state. Every other hit fails CI.
#
# The intent: the next time someone adds a credential-touching field in, say,
# strategy/ or analytics/, this check stops the PR before it lands.
#
# Run locally: scripts/check-credentials.sh
# CI:          wired as the credentials-check job in .github/workflows/ci.yml
#
# Exit 0 on clean pass; exit 1 on any hit outside the allow-list.
#
# Allow-list rationale:
#   * src/bin/engine_live/       — live binary entry point (future).
#   * src/providers/binance/     — Binance provider owns the REST credential
#     store and HMAC signing. Per prerequisites.md §10 the deepdive moves this
#     into src/bin/engine_live/ later; until that move lands (tracked in
#     docs/migration.md Step 10 notes), the provider directory is allow-listed.
#   * src/main.cpp               — the three binaries still share one main.cpp
#     during the current transition. When src/bin/<target>/main.cpp lands
#     (Step 10 src/bin/ split, currently in-flight), this entry should be
#     tightened to engine_live's main.cpp only.

set -euo pipefail

ALLOW=(
    'src/main\.cpp'
    '^src/bin/engine_live/'
    '^src/providers/binance/'
)

pattern='api_key|api_secret'
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
done < <(find src -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' \) -print0)

if (( ${#hits[@]} > 0 )); then
    printf '\033[31m[credentials-check] api_key / api_secret found outside the credential allow-list:\033[0m\n' >&2
    for h in "${hits[@]}"; do
        printf '  - %s\n' "$h" >&2
        grep -nE "$pattern" "$h" >&2 | sed 's/^/      /'
    done
    printf '\nEither move the credential handling into src/bin/engine_live/ or extend\nthe allow-list in scripts/check-credentials.sh with a written justification.\n' >&2
    exit 1
fi

echo "credentials-check: OK (api_key/api_secret confined to the allow-list)"
