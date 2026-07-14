#!/usr/bin/env bash
# check-layer-deps.sh
#
# Enforces the deepdive's layered dependency graph (prerequisites.md §10,
# docs/target-architecture.md §1.1) by parsing every project-local #include
# edge in src/ and rejecting any edge that points *upward* through the layering.
#
# The allowed direction is strictly one-way: each module is listed below with
# its allowed dependencies. A module may include from itself, from the standard
# library, from a third-party library (treated as external when the first path
# component is not a real src/ subdir), or from any entry in its allow-list —
# nothing else.
#
# Run locally: scripts/check-layer-deps.sh
# CI:         wired as the layer-deps job in .github/workflows/ci.yml
#
# Exit 0 on clean pass; exit 1 on any forbidden edge (with the edge printed).
#
# Deferred (documented in docs/migration.md Step 10 notes):
#   * Encoding the graph as CMake OBJECT libraries — requires header-level
#     cycle resolution (risk↔analytics coupling) beyond Step 10's scope.
#   * Header hygiene (src/<module>/include/tt/<module>/) — separate sweep.

set -euo pipefail

# Module → allowed-deps list. "same module" and std/third-party are implicit.
declare -A ALLOWED
ALLOWED[core]=""
ALLOWED[types]=""
ALLOWED[indicator]=""
ALLOWED[utils]=""
ALLOWED[debug]=""
ALLOWED[orderbook]="core types"
ALLOWED[threading]="core utils debug"
ALLOWED[execution]="core types orderbook"
ALLOWED[analytics]="core threading risk"
ALLOWED[market_maker]="core orderbook threading types"
ALLOWED[risk]="core execution analytics"
ALLOWED[strategy]="core indicator execution exits threading"
ALLOWED[data]="core types utils debug execution"
ALLOWED[providers]="core types utils data orderbook execution engine exits risk simulation threading ui"
ALLOWED[engine]="core types indicator utils debug threading orderbook execution analytics market_maker risk strategy data providers exits ui"
ALLOWED[api]="engine core data strategy execution"
ALLOWED[web]="ui analytics"   # read-only serializers: dashboard_snapshot + AnalyticsReport

# Current cross-module contracts beyond the original lower-layer graph:
#   * analytics exposes a risk snapshot used by RiskManager gatekeeping.
#   * strategies emit exit intents, and adaptive hybrid owns worker/thread knobs.
#   * data/questdb serializes execution order tracker state.
#   * providers bind venue adapters for exits, futures risk, synthetic generation,
#     watchdog callbacks, and optional UI status plumbing.
#   * engine is the composition root for exits and UI dashboard snapshots.

# Files exempted from the check.  Reason must be documented.
#   debug/debug_report.{h,cpp} + debug/memory_info.cpp  — compiled only when
#   HAS_DEBUG is defined, so cross-module includes are gated at compile time.
declare -A EXEMPT_FILES
EXEMPT_FILES[src/debug/debug_report.cpp]=1
EXEMPT_FILES[src/debug/debug_report.h]=1
EXEMPT_FILES[src/debug/memory_info.cpp]=1
EXEMPT_FILES[src/debug/hardware_info.cpp]=1
EXEMPT_FILES[src/debug/stage_timer.cpp]=1

# Determine real src-level modules (used to distinguish third-party from local).
declare -A REAL_MODULES
for d in src/*/; do
    name="$(basename "$d")"
    REAL_MODULES[$name]=1
done

violations=0

while IFS= read -r -d '' file; do
    [[ -n "${EXEMPT_FILES[$file]:-}" ]] && continue

    rel="${file#src/}"
    module="${rel%%/*}"

    [[ -z "${ALLOWED[$module]+x}" ]] && continue

    while IFS= read -r line; do
        # Match only project-local quoted includes: "foo/bar.h" or "../foo/bar.h".
        [[ "$line" =~ ^[[:space:]]*#include[[:space:]]*\"([^\"]+)\" ]] || continue
        inc="${BASH_REMATCH[1]}"

        inc_norm="${inc#./}"
        while [[ "$inc_norm" == ../* ]]; do
            inc_norm="${inc_norm#../}"
        done

        target="${inc_norm%%/*}"

        # Bare header (no slash) = same module.
        [[ "$target" == "$inc_norm" ]] && continue
        # Same module (incl. sub-dirs like providers/local/ under module=providers).
        [[ "$target" == "$module" ]] && continue
        # Third-party (first component is not a real src/ module).
        [[ -z "${REAL_MODULES[$target]:-}" ]] && continue

        # core→debug is legal when the include is guarded by HAS_DEBUG.
        if [[ "$module" == "core" && "$target" == "debug" ]]; then
            if grep -qE '^#if(def| defined)\s*\(?\s*HAS_DEBUG' "$file"; then
                continue
            fi
        fi
        # Anywhere→debug is legal when guarded by HAS_DEBUG (debug layer is a
        # zero-cost leaf when HAS_DEBUG is off; the include won't compile).
        if [[ "$target" == "debug" ]]; then
            if grep -qE '^#if(def| defined)\s*\(?\s*HAS_DEBUG' "$file"; then
                continue
            fi
        fi

        allowed=" ${ALLOWED[$module]} "
        if [[ "$allowed" != *" $target "* ]]; then
            echo "FORBIDDEN: $file includes $target/ (module $module may only depend on: ${ALLOWED[$module]})" >&2
            violations=$((violations + 1))
        fi
    done < "$file"
done < <(find src -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' \) -print0)

if (( violations > 0 )); then
    echo "" >&2
    echo "$violations forbidden include edge(s) detected. Fix the graph or update the allow-list in scripts/check-layer-deps.sh." >&2
    exit 1
fi

echo "layer-deps: OK"
