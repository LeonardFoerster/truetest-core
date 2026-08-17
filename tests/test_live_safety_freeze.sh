#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

extract_script_frozen_files() {
    awk '
        /^FROZEN_FILES=\(/ { inside = 1; next }
        inside && /^\)/ { exit }
        inside {
            line = $0
            gsub(/^[[:space:]]*"|"[[:space:]]*$/, "", line)
            if (line ~ /^src\//) print line
        }
    ' "$source_root/scripts/check-live-safety-freeze.sh" | sort -u
}

extract_mirror_frozen_files() {
    awk '
        tolower($0) ~ /frozen files/ { found = 1 }
        found && /^```/ {
            if (!inside) { inside = 1; next }
            exit
        }
        inside && /^src\// { print }
    ' "$1" | sort -u
}

for mirror in \
    "$source_root/AGENTS.md" \
    "$source_root/docs/governance/02-prerequisites.md" \
    "$source_root/docs/todos/02-P1-freeze.md"
do
    if ! diff -u \
        <(extract_script_frozen_files) \
        <(extract_mirror_frozen_files "$mirror")
    then
        echo "frozen-file mirror drift: $mirror" >&2
        exit 1
    fi
done

test_root="$(mktemp -d)"
trap 'rm -rf -- "$test_root"' EXIT

mkdir -p "$test_root/scripts" "$test_root/src/engine"
cp "$source_root/scripts/check-live-safety-freeze.sh" "$test_root/scripts/"
chmod +x "$test_root/scripts/check-live-safety-freeze.sh"
cd "$test_root"

git init -q
git config user.name "Freeze Contract Test"
git config user.email "freeze-contract@example.invalid"
printf 'base\n' > src/engine/engine.cpp
git add .
git commit -q -m "test: base" \
    -m "LIVE_SAFETY_CCB_APPROVED"

expect_failure() {
    if "$@" >/dev/null 2>&1; then
        echo "expected failure: $*" >&2
        exit 1
    fi
}

./scripts/check-live-safety-freeze.sh

printf 'dirty\n' >> src/engine/engine.cpp
expect_failure ./scripts/check-live-safety-freeze.sh
LIVE_SAFETY_APPROVAL_TOKEN=LIVE_SAFETY_CCB_APPROVED \
    ./scripts/check-live-safety-freeze.sh

git add src/engine/engine.cpp
git commit -q -m "fix: authorized" \
    -m "LIVE_SAFETY_CCB_APPROVED"
./scripts/check-live-safety-freeze.sh

printf 'later dirty\n' >> src/engine/engine.cpp
expect_failure ./scripts/check-live-safety-freeze.sh
git restore src/engine/engine.cpp

git rm -q src/engine/engine.cpp
expect_failure ./scripts/check-live-safety-freeze.sh
git restore --staged src/engine/engine.cpp
git restore src/engine/engine.cpp

expect_failure ./scripts/check-live-safety-freeze.sh --base not-a-commit

printf 'unauthorized commit\n' >> src/engine/engine.cpp
git add src/engine/engine.cpp
git commit -q -m "fix: missing approval"
expect_failure env LIVE_SAFETY_APPROVAL_TOKEN=LIVE_SAFETY_CCB_APPROVED \
    ./scripts/check-live-safety-freeze.sh
