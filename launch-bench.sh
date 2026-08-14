#!/usr/bin/env bash
# Performance & Backtest Launcher
# Run the engine in backtest mode for performance testing and strategy validation.
#
# Features:
#   - No UI (faster execution)
#   - Shadow mode (safe testing)
#   - Performance metrics collection
#   - Strategy backtesting on historical data
#
# Usage:
#   ./launch-bench.sh                  # default backtest
#   ./launch-bench.sh --symbol ethusdt # different pair
#   ./launch-bench.sh --provider bitget # alternate provider
#   TT_MODE=backtest ./launch-bench.sh # explicit backtest mode

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
# Configuration Defaults (Benchmark/Backtest focus)
# ============================================================================

task="bench"  # Benchmark/backtest = no UI, performance focus
preset="${TT_PRESET:-$(get_preset_for_task "$task")}"
build_dir="${TT_BUILD_DIR:-$root_dir/out/build/$preset}"
build_jobs="${TT_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
skip_build="${TT_SKIP_BUILD:-false}"

# Apply bench config
apply_task_config "$task"

# Market data
provider="${TT_PROVIDER:-bitget}"
symbol="${TT_SYMBOL:-btcusdt}"
stream="${TT_STREAM:-trade}"
depth_stream="${TT_DEPTH_STREAM:-depth5}"
mode="${TT_MODE:-backtest}"

# Bench-specific
log_output=""
metrics_output=""

# ============================================================================
# Parse CLI Arguments
# ============================================================================

engine_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --symbol)
      symbol="$2"
      shift 2
      ;;
    --provider)
      provider="$2"
      shift 2
      ;;
    --mode)
      mode="$2"
      shift 2
      ;;
    --log-output)
      log_output="$2"
      shift 2
      ;;
    --help|-h)
      show_help
      exit 0
      ;;
    *)
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
Performance & Backtest Launcher

Usage: ./launch-bench.sh [OPTIONS]

Options:
  --symbol <symbol>       Trading pair (default: btcusdt)
  --provider <provider>   Data source (default: bitget)
  --mode <mode>          backtest (default), shadow, live
  --log-output <file>    Save engine logs to file
  --help, -h             Show this help

Modes:
  backtest  Historical data replay (best for performance testing)
  shadow    Paper trading with live data (slower but realistic)
  live      REAL trading (use only with risk controls!)

Examples:
  # Basic backtest (default)
  ./launch-bench.sh

  # Multiple symbols (run in sequence)
  ./launch-bench.sh --symbol ethusdt
  ./launch-bench.sh --symbol bnbusdt

  # Save output for analysis
  ./launch-bench.sh --symbol ethusdt --log-output bench_eth.log

  # Shadow mode with live data (realistic but slower)
  ./launch-bench.sh --mode shadow

Performance Tips:
  - Backtest mode (default) is fastest
  - No ImGui desk = lower overhead
  - Monitor CPU/memory with: watch -n 1 'top -b -n 1 | head -20'
  - Use --log-output to capture performance data

Environment Variables:
  TT_TASK=bench        Task preset
  TT_SYMBOL=...        Symbol via env
  TT_PROVIDER=...      Provider via env
  TT_MODE=backtest     Backtest mode via env
  TT_SKIP_BUILD=true   Use existing binary

Data Sources:
  bitget               Recommended for backtests (good data quality)
  binance-futures      Higher fees but very liquid
  bitunix              Good for niche pairs
EOF
}

# ============================================================================
# Validation & Build
# ============================================================================

check_requirements
verify_repo_structure "$root_dir"

log_section "Launching Backtest/Performance Mode"
echo "  Mode: Benchmark (no UI, performance optimized)"
echo "  Provider: $provider"
echo "  Symbol: $symbol"
echo "  Backtest mode: $mode"
print_config_summary

# Build (bench: only engine_shadow, no tests)
build_targets_to_build=$(get_build_targets_for_task "$task")
build "$root_dir" "$build_dir" "$preset" "$build_targets_to_build" "$build_jobs" "$skip_build"

# ============================================================================
# Engine Arguments
# ============================================================================

engine_args_final=(
  "--provider" "$provider"
  "--symbol" "$symbol"
  "--stream" "$stream"
  "--depth-stream" "$depth_stream"
  "--mode" "$mode"
  # Note: --desk is NOT included for benchmarks (performance)
)

# Append user arguments
engine_args_final+=("${engine_args[@]}")

# ============================================================================
# Logging Setup
# ============================================================================

if [[ -n "$log_output" ]]; then
  log_info "Logs will be saved to: $log_output"
  # Note: Implement in engine if needed
  # engine_args_final+=("--log-file" "$log_output")
fi

# ============================================================================
# Execution
# ============================================================================

verify_binary "$build_dir/engine_shadow"

log_section "Starting Backtest/Performance Run"
log_info "Provider: $provider | Symbol: $symbol | Mode: $mode"
log_info "Command: $build_dir/engine_shadow ${engine_args_final[*]}"
log_info "No UI — focus on performance metrics"
log_info "Press Ctrl+C to stop"
echo ""

# Run with optional logging
if [[ -n "$log_output" ]]; then
  exec "$build_dir/engine_shadow" "${engine_args_final[@]}" | tee "$log_output"
else
  exec "$build_dir/engine_shadow" "${engine_args_final[@]}"
fi
