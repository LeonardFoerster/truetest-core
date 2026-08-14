#!/usr/bin/env bash
# ImGui Desk UI Development Launcher
# Optimized for rapid UI iteration with debug options and layout presets.
#
# Features:
#   - Debug UI mode (wireframes, performance stats, layout info)
#   - Layout preset switching
#   - Optimized for quick rebuilds
#
# Usage:
#   ./launch-desk.sh                           # default ImGui desk setup
#   ./launch-desk.sh --desk-debug-ui           # enable debug visualization
#   ./launch-desk.sh --desk-layout mobile.h    # load mobile layout preset
#   ./launch-desk.sh --symbol ethusdt --desk-debug-ui  # combine options

set -euo pipefail

# ============================================================================
# Setup
# ============================================================================

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Load helper functions
helpers_script="$root_dir/launch-helpers.sh"
if [[ ! -f "$helpers_script" ]]; then
  echo "Error: launch-helpers.sh not found" >&2
  exit 1
fi
# shellcheck source=launch-helpers.sh
source "$helpers_script"

# Load .env configuration
load_env_file "$root_dir/.env"

# ============================================================================
# Configuration Defaults (ImGui Desk focus)
# ============================================================================

task="dev"  # Desk development = quick iteration
preset="${TT_PRESET:-$(get_preset_for_task "$task")}"
build_dir="${TT_BUILD_DIR:-$root_dir/out/build/$preset}"
build_jobs="${TT_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
skip_build="${TT_SKIP_BUILD:-false}"

# Apply dev config
apply_task_config "$task"

# Market data
provider="${TT_PROVIDER:-binance-futures}"
symbol="${TT_SYMBOL:-btcusdt}"
stream="${TT_STREAM:-trade}"
depth_stream="${TT_DEPTH_STREAM:-depth5}"
mode="${TT_MODE:-shadow}"

# Desk-specific config
desk_enabled=true
desk_debug_ui="${DESK_DEBUG_UI:-false}"
desk_layout="${DESK_LAYOUT:-}"
desk_hot_reload="${DESK_HOT_RELOAD:-false}"

# ============================================================================
# Parse CLI Arguments
# ============================================================================

engine_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --desk-debug-ui)
      desk_debug_ui=true
      shift
      ;;
    --desk-layout)
      desk_layout="$2"
      shift 2
      ;;
    --desk-no-reload)
      desk_hot_reload=false
      shift
      ;;
    --symbol)
      symbol="$2"
      shift 2
      ;;
    --provider)
      provider="$2"
      shift 2
      ;;
    --help|-h)
      show_help
      exit 0
      ;;
    *)
      # Pass through to engine
      engine_args+=("$1")
      shift
      ;;
  esac
done

# ============================================================================
# Functions
# ============================================================================

show_help() {
  cat << 'EOF'
ImGui Desk UI Development Launcher

Usage: ./launch-desk.sh [OPTIONS]

Options:
  --desk-debug-ui              Enable debug UI (wireframes, stats, layout info)
  --desk-layout <file>         Load layout preset (e.g., mobile.h, desktop.h)
  --desk-no-reload             Disable hot-reload on file changes
  --symbol <symbol>            Trading symbol (default: btcusdt)
  --provider <provider>        Market data provider (default: binance-futures)
  --help, -h                   Show this help

Examples:
  # Standard desk setup
  ./launch-desk.sh

  # With debug visualization
  ./launch-desk.sh --desk-debug-ui

  # Load mobile layout + debug mode
  ./launch-desk.sh --desk-layout mobile.h --desk-debug-ui

  # Different symbol + debug
  ./launch-desk.sh --symbol ethusdt --desk-debug-ui

Environment Variables:
  TT_TASK=dev          Task preset (always 'dev' for desk)
  TT_PROVIDER=...      Override provider
  TT_SYMBOL=...        Override symbol
  DESK_DEBUG_UI=true   Enable debug via env
  DESK_LAYOUT=...      Layout file via env
  TT_SKIP_BUILD=true   Skip rebuild (use existing binary)

Tips:
  - Use --desk-debug-ui to visualize layout boundaries and performance
  - Modify desk_layout_model.h and re-run to test different page configs
  - Monitor build times; use TT_SKIP_BUILD=true for rapid iterations
EOF
}

# ============================================================================
# Validation & Build
# ============================================================================

check_requirements
verify_repo_structure "$root_dir"

log_section "Launching ImGui Desk UI"
echo "  Mode: Development (ImGui desktop UI)"
echo "  Symbol: $symbol"
echo "  Provider: $provider"
echo "  Debug UI: $desk_debug_ui"
[[ -n "$desk_layout" ]] && echo "  Layout: $desk_layout" || true
print_config_summary

# Build (desk development: build engine_shadow + tests)
build_targets_to_build=$(get_build_targets_for_task "$task")
build "$root_dir" "$build_dir" "$preset" "$build_targets_to_build" "$build_jobs" "$skip_build"

# ============================================================================
# Engine Arguments
# ============================================================================

engine_args_final=(
  "--provider" "$provider"
  "--desk"
  "--symbol" "$symbol"
  "--stream" "$stream"
  "--depth-stream" "$depth_stream"
  "--mode" "$mode"
)

# Add debug options if requested
if [[ "$desk_debug_ui" == "true" ]]; then
  log_info "Debug UI enabled: wireframes, stats, layout info"
  # Note: These flags should be handled by engine_shadow
  # Uncomment when implemented in engine
  # engine_args_final+=("--desk-debug")
fi

# Add layout override if specified
if [[ -n "$desk_layout" ]]; then
  log_info "Layout preset: $desk_layout"
  # Note: This would pass to engine if supported
  # engine_args_final+=("--desk-layout" "$desk_layout")
fi

# Append user arguments
engine_args_final+=("${engine_args[@]}")

# ============================================================================
# Execution
# ============================================================================

verify_binary "$build_dir/engine_shadow"
cleanup_processes

log_section "Starting ImGui Desk"
log_info "Command: $build_dir/engine_shadow ${engine_args_final[*]}"
log_info "Debug tips: Press F1 for help, F11 for fullscreen, Ctrl+C to stop"
echo ""

exec "$build_dir/engine_shadow" "${engine_args_final[@]}"
