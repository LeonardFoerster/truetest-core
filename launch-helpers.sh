#!/usr/bin/env bash
# Shared helper functions for launch scripts
# Source this file: source "$(dirname "$0")/launch-helpers.sh"

set -euo pipefail

# ============================================================================
# Colors for output (work in light/dark themes)
# ============================================================================
readonly COLOR_RESET='\033[0m'
readonly COLOR_BOLD='\033[1m'
readonly COLOR_GREEN='\033[32m'
readonly COLOR_YELLOW='\033[33m'
readonly COLOR_RED='\033[31m'
readonly COLOR_BLUE='\033[34m'
readonly COLOR_CYAN='\033[36m'

# ============================================================================
# Logging Functions
# ============================================================================

log_info() {
  echo -e "${COLOR_BLUE}ℹ${COLOR_RESET} $*"
}

log_success() {
  echo -e "${COLOR_GREEN}✓${COLOR_RESET} $*"
}

log_warn() {
  echo -e "${COLOR_YELLOW}⚠${COLOR_RESET} $*" >&2
}

log_error() {
  echo -e "${COLOR_RED}✗${COLOR_RESET} $*" >&2
}

log_section() {
  echo ""
  echo -e "${COLOR_BOLD}${COLOR_CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${COLOR_RESET}"
  echo -e "${COLOR_BOLD}$*${COLOR_RESET}"
  echo -e "${COLOR_BOLD}${COLOR_CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${COLOR_RESET}"
}

# ============================================================================
# Error Handling
# ============================================================================

error_exit() {
  local msg="$1"
  local exit_code="${2:-1}"
  log_error "$msg"
  exit "$exit_code"
}

# Trap errors with context
trap_error() {
  local line=$1
  local exit_code=$2
  log_error "Command failed at line $line (exit code: $exit_code)"
  exit "$exit_code"
}

# ============================================================================
# Environment & Config Loading
# ============================================================================

# Load .env file if it exists
load_env_file() {
  local env_file="${1:-.env}"

  if [[ -f "$env_file" ]]; then
    log_info "Loading config from $env_file"
    # shellcheck source=/dev/null
    source "$env_file"
  fi
}

# Validate required variable is set
require_var() {
  local var_name="$1"
  local var_value="${!var_name:-}"

  if [[ -z "$var_value" ]]; then
    error_exit "Required variable not set: $var_name"
  fi
}

# ============================================================================
# Build Management
# ============================================================================

# Get preset based on task
get_preset_for_task() {
  local task="${1:-dev}"
  case "$task" in
    dev|development) echo "linux-dev" ;;
    test|tests) echo "linux-tests" ;;
    bench|benchmark) echo "linux-bench" ;;
    live) echo "linux-dev" ;; # Same as dev, but different engine args
    *) echo "linux-dev" ;;
  esac
}

# Get build targets based on task
get_build_targets_for_task() {
  local task="${1:-dev}"
  case "$task" in
    dev|development) echo "engine_shadow truetest_tests" ;;
    test|tests) echo "truetest_tests" ;;
    bench|benchmark) echo "engine_shadow truetest_tests" ;;
    live) echo "engine_shadow" ;;
    *) echo "engine_shadow truetest_tests" ;;
  esac
}

# Configure CMake for custom build directory
configure_cmake() {
  local root_dir="$1"
  local build_dir="$2"
  local preset="$3"

  log_info "Configuring CMake: $preset → $build_dir"

  cmake -S "$root_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTS=ON \
    -DENABLE_IMGUI=ON \
    -DENABLE_LIVE_DATA=ON \
    -DENABLE_BINANCE=ON \
    -DENABLE_BITGET=ON \
    -DENABLE_BITUNIX=ON \
    -DENABLE_DEBUG=OFF \
    -DENABLE_BENCHMARKS=OFF \
    -DENABLE_NATIVE_OPT=OFF || error_exit "CMake configuration failed"
}

# Build targets
build_targets() {
  local build_dir="$1"
  local targets="$2"
  local jobs="${3:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"

  log_section "Building: $targets"

  if ! cmake --build "$build_dir" --target $targets -j "$jobs"; then
    error_exit "Build failed" 1
  fi

  log_success "Build completed"
}

# Build with preset or custom directory
build() {
  local root_dir="$1"
  local build_dir="$2"
  local preset="${3:-linux-dev}"
  local targets="${4:-engine_shadow truetest_tests}"
  local jobs="${5:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
  local skip_build="${6:-false}"

  if [[ "$skip_build" == "true" && -f "$build_dir/engine_shadow" ]]; then
    log_info "Skipping build (binary exists)"
    return 0
  fi

  if [[ -n "${TT_BUILD_DIR:-}" ]]; then
    # Custom build directory: configure explicitly
    configure_cmake "$root_dir" "$build_dir" "$preset"
  else
    # Use preset
    log_info "Configuring CMake preset: $preset"
    cmake --preset "$preset" || true
  fi

  build_targets "$build_dir" "$targets" "$jobs"
}

# ============================================================================
# Binary & Executable Management
# ============================================================================

# Check if binary exists and is executable
verify_binary() {
  local binary_path="$1"

  if [[ ! -f "$binary_path" ]]; then
    error_exit "Binary not found: $binary_path"
  fi

  if [[ ! -x "$binary_path" ]]; then
    error_exit "Binary is not executable: $binary_path"
  fi
}

# Cleanup old processes (optional port release)
cleanup_processes() {
  local port="${1:-}"

  if [[ -n "$port" ]]; then
    log_info "Checking for processes on port $port..."
    if lsof -Pi ":$port" -sTCP:LISTEN -t >/dev/null 2>&1; then
      log_warn "Port $port already in use"
      log_info "Run 'lsof -i :$port' to see details"
    fi
  fi
}

# ============================================================================
# Engine Arguments Builder
# ============================================================================

# Build engine arguments with defaults
build_engine_args() {
  local task="${1:-dev}"
  local provider="${2:-binance-futures}"
  local symbol="${3:-btcusdt}"
  local stream="${4:-trade}"
  local depth_stream="${5:-depth5}"
  local mode="${6:-shadow}"
  local user_args=("${@:7}")

  local args=(
    "--provider" "$provider"
    "--desk"
    "--symbol" "$symbol"
    "--stream" "$stream"
    "--depth-stream" "$depth_stream"
    "--mode" "$mode"
  )

  # Append user arguments
  args+=("${user_args[@]}")

  # Override with explicit flags if provided
  for arg in "${user_args[@]}"; do
    case "$arg" in
      --provider=*|--symbol=*|--stream=*|--depth-stream=*|--mode=*)
        # These will override the defaults due to position in array
        ;;
    esac
  done

  echo "${args[@]}"
}

# ============================================================================
# Validation & Checks
# ============================================================================

# Verify repository structure
verify_repo_structure() {
  local root_dir="$1"
  local required_files=(
    "CMakeLists.txt"
    "CMakePresets.json"
    "src"
  )

  for file in "${required_files[@]}"; do
    if [[ ! -e "$root_dir/$file" ]]; then
      error_exit "Repository structure invalid: missing $file"
    fi
  done
}

# Check for required tools
check_requirements() {
  local required_tools=("cmake" "bash")

  for tool in "${required_tools[@]}"; do
    if ! command -v "$tool" &>/dev/null; then
      error_exit "Required tool not found: $tool"
    fi
  done
}

# ============================================================================
# Cleanup & Signal Handling
# ============================================================================

# Graceful cleanup on Ctrl+C
cleanup_on_interrupt() {
  log_warn "Interrupted by user"
  # Add any cleanup logic here (kill subprocesses, etc.)
  exit 130  # Standard exit code for SIGINT
}

trap cleanup_on_interrupt SIGINT SIGTERM

# ============================================================================
# Task-specific Configuration
# ============================================================================

# Apply task-based configuration
apply_task_config() {
  local task="${1:-dev}"

  case "$task" in
    dev|development)
      export TT_PRESET="${TT_PRESET:-linux-dev}"
      export TT_PROVIDER="${TT_PROVIDER:-binance-futures}"
      export TT_MODE="${TT_MODE:-shadow}"
      export DESK_ENABLED="${DESK_ENABLED:-true}"
      log_info "Task config: dev (rapid iteration, ImGui desk, shadow mode)"
      ;;
    bench|benchmark)
      export TT_PRESET="${TT_PRESET:-linux-bench}"
      export TT_PROVIDER="${TT_PROVIDER:-bitget}"
      export TT_MODE="${TT_MODE:-backtest}"
      export DESK_ENABLED="${DESK_ENABLED:-false}"
      log_info "Task config: bench (performance testing, no UI)"
      ;;
    test|tests)
      export TT_PRESET="${TT_PRESET:-linux-tests}"
      export TT_MODE="${TT_MODE:-shadow}"
      export DESK_ENABLED="${DESK_ENABLED:-false}"
      log_info "Task config: test (unit tests only)"
      ;;
    live)
      export TT_PRESET="${TT_PRESET:-linux-dev}"
      export TT_PROVIDER="${TT_PROVIDER:-binance-futures}"
      export TT_MODE="${TT_MODE:-live}"
      export DESK_ENABLED="${DESK_ENABLED:-true}"
      log_info "Task config: live (real provider connection)"
      ;;
  esac
}

# ============================================================================
# Summary & Debugging
# ============================================================================

print_config_summary() {
  log_section "Configuration Summary"
  echo "  Root directory: ${ROOT_DIR:-$(pwd)}"
  echo "  Build directory: ${BUILD_DIR:-not set}"
  echo "  CMake preset: ${TT_PRESET:-auto}"
  echo "  Task: ${TT_TASK:-dev}"
  echo "  Provider: ${TT_PROVIDER:-auto}"
  echo "  Engine mode: ${TT_MODE:-shadow}"
  echo "  Symbol: ${TT_SYMBOL:-btcusdt}"
  echo "  Desk UI: ${DESK_ENABLED:-true}"
  [[ "${DESK_DEBUG_UI:-false}" == "true" ]] && echo "  Desk debug: enabled" || true
}

print_help_task_modes() {
  log_section "Available Task Modes"
  cat << 'EOF'
  dev         Quick iteration (ImGui desk, shadow mode)
  bench       Performance testing (backtest, no UI)
  test        Unit tests only (no engine)
  live        Live provider connection (real market data)

Examples:
  TT_TASK=dev ./launch-default.sh
  TT_TASK=bench TT_SYMBOL=ethusdt ./launch-bench.sh
  TT_TASK=live ./launch-live.sh --symbol=btcusdt
  TT_TASK=test ./launch-test.sh
EOF
}

# ============================================================================
# Export for sourcing
# ============================================================================
export -f log_info log_success log_warn log_error log_section
export -f error_exit require_var
export -f get_preset_for_task get_build_targets_for_task
export -f configure_cmake build_targets build verify_binary cleanup_processes
export -f build_engine_args verify_repo_structure check_requirements
export -f apply_task_config print_config_summary print_help_task_modes
