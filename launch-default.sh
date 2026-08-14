#!/usr/bin/env bash
# Local development launcher
# Builds safe-shadow binary and desk UI, then starts with sensible defaults.
#
# Context-aware: Task configuration (dev/bench/test/live) auto-sets appropriate
# compiler flags and engine parameters. Override via environment or CLI flags.
#
# Usage:
#   ./launch-default.sh                    # start with defaults
#   ./launch-default.sh --symbol ethusdt   # override symbol
#   TT_TASK=bench ./launch-default.sh      # benchmark config
#   TT_PROVIDER=bitget ./launch-default.sh # different provider

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

# Load .env configuration if it exists
load_env_file "$root_dir/.env"

# ============================================================================
# Configuration
# ============================================================================

# Set defaults (can be overridden by .env or env vars)
task="${TT_TASK:-dev}"
preset="${TT_PRESET:-$(get_preset_for_task "$task")}"
build_dir="${TT_BUILD_DIR:-$root_dir/out/build/$preset}"
build_jobs="${TT_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
skip_build="${TT_SKIP_BUILD:-false}"

# Apply task-based configuration
apply_task_config "$task"

# Market data defaults (can be overridden)
provider="${TT_PROVIDER:-binance-futures}"
symbol="${TT_SYMBOL:-btcusdt}"
stream="${TT_STREAM:-trade}"
depth_stream="${TT_DEPTH_STREAM:-depth5}"
mode="${TT_MODE:-shadow}"

# ============================================================================
# Validation & Build
# ============================================================================

check_requirements
verify_repo_structure "$root_dir"

log_section "Launching TrueTest Engine"
print_config_summary

# Build targets
build_targets_to_build=$(get_build_targets_for_task "$task")
build "$root_dir" "$build_dir" "$preset" "$build_targets_to_build" "$build_jobs" "$skip_build"

# ============================================================================
# Engine Arguments
# ============================================================================

# Start with base arguments
engine_args=(
  "--provider" "$provider"
  "--desk"
  "--symbol" "$symbol"
  "--stream" "$stream"
  "--depth-stream" "$depth_stream"
  "--mode" "$mode"
)

# Append user arguments (from CLI)
engine_args+=("$@")

# ============================================================================
# Cleanup & Execution
# ============================================================================

verify_binary "$build_dir/engine_shadow"
cleanup_processes  # Check for port conflicts

log_section "Starting Engine"
log_info "Command: $build_dir/engine_shadow ${engine_args[*]}"
log_info "Press Ctrl+C to stop"
echo ""

exec "$build_dir/engine_shadow" "${engine_args[@]}"
