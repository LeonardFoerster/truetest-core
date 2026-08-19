#!/usr/bin/env bash
# check-layer-deps.sh
#
# Enforces the deepdive's layered dependency graph (prerequisites.md §10,
# docs/architecture/01-target-architecture.md §1.1) by parsing every project-local #include
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
# Deferred (documented historically; current layer rules: docs/architecture/01-target-architecture.md):
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
ALLOWED[analytics]="core threading risk types"
ALLOWED[market_maker]="core orderbook threading types"
ALLOWED[risk]="core execution analytics"
ALLOWED[strategy]="core types indicator execution exits threading"
ALLOWED[data]="core types utils debug execution"
ALLOWED[exits]="core"
ALLOWED[simulation]="analytics data engine execution exits providers strategy"
ALLOWED[ui]="analytics core providers"
ALLOWED[presets]=""
ALLOWED[providers]="core types utils data orderbook execution engine exits risk simulation threading ui"
ALLOWED[engine]="core types indicator utils debug threading orderbook execution analytics market_maker risk strategy data providers exits ui"
ALLOWED[api]="engine core data strategy execution"
ALLOWED[web]="ui analytics"   # read-only serializers: dashboard_snapshot + AnalyticsReport
# Executable composition root: it is allowed to wire every application layer,
# but is still checked so a newly introduced src/ module cannot bypass review.
ALLOWED[bin]="core data debug engine execution market_maker orderbook presets providers simulation strategy threading ui utils web"

# Current cross-module contracts beyond the original lower-layer graph:
#   * analytics/footprint (footprint.md §2.2) aggregates the leaf PublicTrade
#     POD (types layer) into footprint bars - cold research math, no provider
#     or engine dependency, so only the `types` edge was added.
#   * analytics exposes a risk snapshot used by RiskManager gatekeeping.
#   * strategies use dense SymbolTable ids via SymbolStateStore (types layer).
#   * strategies emit exit intents; no strategy owns engine worker/thread knobs.
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

# A new src-level module must receive an explicit graph rule. Silently skipping
# an unknown module would let every include edge from that module bypass this
# gate until somebody happened to notice the omission.
for module in "${!REAL_MODULES[@]}"; do
    if [[ -z "${ALLOWED[$module]+x}" ]]; then
        echo "UNMAPPED: src/$module/ has no dependency rule in scripts/check-layer-deps.sh" >&2
        violations=$((violations + 1))
    fi
done

while IFS= read -r -d '' file; do
    [[ -n "${EXEMPT_FILES[$file]:-}" ]] && continue

    rel="${file#src/}"
    module="${rel%%/*}"

    [[ -z "${ALLOWED[$module]+x}" ]] && continue  # already reported above

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
done < <(find src -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.inc' \) -print0)

if (( violations > 0 )); then
    echo "" >&2
    echo "$violations forbidden include edge(s) detected. Fix the graph or update the allow-list in scripts/check-layer-deps.sh." >&2
    exit 1
fi

# =============================================================================
# Extension-boundary checks (engine-decomposition Phase 3, 2026-08).
# See docs/architecture/05-engine-boundaries.md for the invariants these
# encode. Both are explicit deny-lists over a small, named set of files —
# not a blanket regex sweep — matching the module-graph check's philosophy
# above.
# =============================================================================

# --- Check A: no vendor-specific provider headers outside providers/ or bin/ ---
# "generic engine/pipeline code contains no unjustified venue-specific
# branching" (Step 3/7). Only the provider module itself (which implements
# venue adapters) and the composition root (bin/, which wires a concrete
# provider at startup) may name a vendor directly.
VENDOR_DIRS="binance bitget bitunix"
vendor_violations=0
while IFS= read -r -d '' file; do
    case "$file" in
        src/providers/*|src/bin/*) continue ;;
    esac
    for vendor in $VENDOR_DIRS; do
        if grep -qE "^[[:space:]]*#include[[:space:]]*\"providers/${vendor}/" "$file"; then
            echo "FORBIDDEN: $file includes providers/${vendor}/ directly — venue-specific headers may only be included from src/providers/ (adapter implementation) or src/bin/ (composition root)" >&2
            vendor_violations=$((vendor_violations + 1))
        fi
    done
done < <(find src -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.inc' \) -print0)

if (( vendor_violations > 0 )); then
    echo "" >&2
    echo "$vendor_violations vendor-provider leak(s) detected." >&2
    exit 1
fi

# --- Check B: extracted engine collaborators may not depend on the concrete
# `engine` class (only on named domain-subsystem references passed at
# construction — the FillProcessor pattern). "processors do not use Engine
# as a generic service bag" (Step 4). Extend this list as new collaborators
# are extracted (e.g. a future OrderIntentProcessor / MarketEventProcessor).
ENGINE_COLLABORATOR_HEADERS="
src/engine/dashboard_snapshot_builder.h
src/engine/execution_router.h
src/engine/checkpoint.h
src/engine/instrument_spec_cache.h
src/engine/order_audit_sink.h
src/engine/fill_processor.h
src/engine/live_safety_session.h
"
backref_violations=0
for hdr in $ENGINE_COLLABORATOR_HEADERS; do
    [[ -f "$hdr" ]] || continue
    if grep -qE '#include[[:space:]]*"engine\.h"' "$hdr"; then
        echo "FORBIDDEN: $hdr includes engine.h — extracted collaborators take named domain-subsystem references, not the concrete engine class" >&2
        backref_violations=$((backref_violations + 1))
    fi
    if grep -qE '\bclass[[:space:]]+engine[[:space:]]*;' "$hdr"; then
        echo "FORBIDDEN: $hdr forward-declares 'class engine' — extracted collaborators take named domain-subsystem references, not the concrete engine class" >&2
        backref_violations=$((backref_violations + 1))
    fi
done

if (( backref_violations > 0 )); then
    echo "" >&2
    echo "$backref_violations engine-backreference violation(s) detected." >&2
    exit 1
fi

echo "layer-deps: OK"
