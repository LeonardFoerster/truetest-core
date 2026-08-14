#!/usr/bin/env bash
# Live Provider Connection Launcher
# Test real market data feeds from Binance, Bitget, or BitUNIX.
#
# Features:
#   - Real-time market data from live exchanges
#   - Multiple provider support
#   - Desktop UI for monitoring
#   - Shadow mode (paper trading) for safe testing
#
# Usage:
#   ./launch-live.sh                          # default (Binance)
#   ./launch-live.sh --provider bitget        # Bitget connection
#   ./launch-live.sh --symbol ethusdt         # different symbol
#   TT_MODE=live ./launch-live.sh             # REAL trading (dangerous!)

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
# Configuration Defaults (Live data focus)
# ============================================================================

task="live"  # Live provider connections
preset="${TT_PRESET:-$(get_preset_for_task "$task")}"
build_dir="${TT_BUILD_DIR:-$root_dir/out/build/$preset}"
build_jobs="${TT_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
skip_build="${TT_SKIP_BUILD:-false}"

# Apply live config
apply_task_config "$task"

# Market data
provider="${TT_PROVIDER:-binance-futures}"
symbol="${TT_SYMBOL:-btcusdt}"
stream="${TT_STREAM:-trade}"
depth_stream="${TT_DEPTH_STREAM:-depth5}"
mode="${TT_MODE:-shadow}"  # Default to shadow; upgrade to 'live' intentionally

# ============================================================================
# Parse CLI Arguments
# ============================================================================

engine_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --provider)
      provider="$2"
      shift 2
      ;;
    --symbol)
      symbol="$2"
      shift 2
      ;;
    --stream)
      stream="$2"
      shift 2
      ;;
    --mode)
      mode="$2"
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
Live Provider Connection Launcher

Usage: ./launch-live.sh [OPTIONS]

Options:
  --provider <provider>    Exchange: binance-futures, bitget, bitunix
  --symbol <symbol>        Trading pair (default: btcusdt)
  --stream <stream>        Data stream: trade, kline (default: trade)
  --mode <mode>            shadow (safe), live (REAL), backtest
  --help, -h               Show this help

Providers:
  binance-futures  Binance Futures (default, most liquid)
  bitget           Bitget (good coverage, alt pairs)
  bitunix          BitUNIX (emerging provider)

Safety:
  Default mode is 'shadow' (paper trading) — safe for testing!
  Use '--mode live' ONLY with real capital and clear risk management.
  Read config/live*.toml and verify your risk limits before live mode.

Examples:
  # Test Binance connection (shadow mode, safe)
  ./launch-live.sh --provider binance-futures

  # Monitor multiple symbols (requires multiple terminals)
  ./launch-live.sh --symbol ethusdt
  ./launch-live.sh --symbol bnbusdt

  # Bitget connection (shadow mode)
  ./launch-live.sh --provider bitget

  # REAL trading (DANGEROUS — verify risk limits first!)
  TT_MODE=live ./launch-live.sh --provider binance-futures

Environment Variables:
  TT_PROVIDER=bitget     Provider via env
  TT_SYMBOL=...         Symbol via env
  TT_MODE=shadow|live   shadow (default, safe) or live (real money)
  TT_SKIP_BUILD=true    Use existing binary (faster)

Troubleshooting:
  - "Connection refused": Provider API may be down; check status pages
  - "Invalid symbol": Verify symbol is available on chosen provider
  - "Rate limit": Wait a few seconds before retrying
  - Check logs for detailed error messages
EOF
}

warn_live_mode() {
  if [[ "$mode" == "live" ]]; then
    log_error "⚠️ LIVE MODE ENABLED - REAL CAPITAL AT RISK ⚠️"
    echo ""
    echo "You are about to trade with REAL money on the live engine."
    echo "Verify:"
    echo "  1. Risk limits in config/live_*.toml"
    echo "  2. Provider connection is stable"
    echo "  3. You understand the market conditions"
    echo ""
    read -p "Type 'YES I UNDERSTAND' to continue: " confirm
    if [[ "$confirm" != "YES I UNDERSTAND" ]]; then
      error_exit "Live mode cancelled by user" 0
    fi
    echo ""
    log_error "LIVE MODE ENGAGED. Monitor carefully!"
    echo ""
  fi
}

# ============================================================================
# Validation & Build
# ============================================================================

check_requirements
verify_repo_structure "$root_dir"

log_section "Launching Live Provider Connection"
echo "  Provider: $provider"
echo "  Symbol: $symbol"
echo "  Mode: $mode"
echo "  Stream: $stream"
print_config_summary

# Build
build_targets_to_build=$(get_build_targets_for_task "$task")
build "$root_dir" "$build_dir" "$preset" "$build_targets_to_build" "$build_jobs" "$skip_build"

# ============================================================================
# Safety Checks
# ============================================================================

# Warn for live mode
warn_live_mode

# Verify credentials exist for live mode
if [[ "$mode" == "live" ]]; then
  config_file="$root_dir/config/live_${provider}.toml"
  if [[ ! -f "$config_file" ]]; then
    error_exit "Live config not found: $config_file"
  fi
fi

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

# Append user arguments
engine_args_final+=("${engine_args[@]}")

# ============================================================================
# Execution
# ============================================================================

verify_binary "$build_dir/engine_shadow"
cleanup_processes

log_section "Starting Live Provider Connection"
log_info "Provider: $provider | Symbol: $symbol | Mode: $mode"
log_info "Command: $build_dir/engine_shadow ${engine_args_final[*]}"
log_info "Press Ctrl+C to stop"
echo ""

exec "$build_dir/engine_shadow" "${engine_args_final[@]}"
