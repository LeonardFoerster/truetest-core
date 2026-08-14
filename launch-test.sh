#!/usr/bin/env bash
# Unit Test Launcher
# Build and run truetest_tests without the engine binary.
#
# Features:
#   - Fast builds (only test executable)
#   - Comprehensive test coverage
#   - Integration with CI/CD
#   - Test filtering and reporting
#
# Usage:
#   ./launch-test.sh                    # run all tests
#   ./launch-test.sh --filter "market*" # run specific tests
#   ./launch-test.sh --verbose          # verbose output
#   ./launch-test.sh --output xml       # generate test report

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
# Configuration Defaults (Test focus)
# ============================================================================

task="test"  # Test mode = only truetest_tests
preset="${TT_PRESET:-$(get_preset_for_task "$task")}"
build_dir="${TT_BUILD_DIR:-$root_dir/out/build/$preset}"
build_jobs="${TT_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
skip_build="${TT_SKIP_BUILD:-false}"

# Apply test config
apply_task_config "$task"

# Test-specific options
test_filter=""
test_verbose=false
test_output=""
test_repeat=1

# ============================================================================
# Parse CLI Arguments
# ============================================================================

test_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --filter)
      test_filter="$2"
      shift 2
      ;;
    --verbose|-v)
      test_verbose=true
      shift
      ;;
    --repeat)
      test_repeat="$2"
      shift 2
      ;;
    --output)
      test_output="$2"
      shift 2
      ;;
    --help|-h)
      show_help
      exit 0
      ;;
    *)
      test_args+=("$1")
      shift
      ;;
  esac
done

# ============================================================================
# Functions
# ============================================================================

show_help() {
  cat << 'EOF'
Unit Test Launcher

Usage: ./launch-test.sh [OPTIONS]

Options:
  --filter <pattern>   Run tests matching pattern (e.g., "market*", "order*")
  --verbose, -v        Verbose test output
  --repeat <n>         Run tests n times (default: 1)
  --output <format>    Output format: text (default), xml, json
  --help, -h           Show this help

Examples:
  # Run all tests
  ./launch-test.sh

  # Run tests matching pattern
  ./launch-test.sh --filter "market*"
  ./launch-test.sh --filter "OrderFlow*"

  # Verbose output with filtering
  ./launch-test.sh --filter "desk*" --verbose

  # Run each test 3 times (find flaky tests)
  ./launch-test.sh --repeat 3

  # Generate XML report for CI
  ./launch-test.sh --output xml --repeat 3

  # Run specific test suites
  ./launch-test.sh --filter "backtest*"
  ./launch-test.sh --filter "shadow*"

Test Suites:
  Market data tests    --filter "market*"
  Order management     --filter "order*"
  Desk/UI tests        --filter "desk*"
  Backtest engine      --filter "backtest*"
  Shadow mode          --filter "shadow*"
  All integration      --filter "*"

Environment Variables:
  TT_TASK=test         Task preset
  TT_SKIP_BUILD=true   Use existing binary
  TT_LOG_LEVEL=...     Test logging level

Performance:
  - Tests build without engine binary = fast
  - Run with --repeat to catch flaky tests
  - Use --filter for faster feedback during development
EOF
}

# ============================================================================
# Validation & Build
# ============================================================================

check_requirements
verify_repo_structure "$root_dir"

log_section "Building Unit Tests"
echo "  Preset: $preset"
echo "  Build directory: $build_dir"
echo "  Skip build: $skip_build"
print_config_summary

# Build tests only
build_targets_to_build=$(get_build_targets_for_task "$task")
build "$root_dir" "$build_dir" "$preset" "$build_targets_to_build" "$build_jobs" "$skip_build"

# ============================================================================
# Test Configuration
# ============================================================================

test_binary="$build_dir/truetest_tests"

verify_binary "$test_binary"

# Build test arguments
test_cmd=("$test_binary")

if [[ -n "$test_filter" ]]; then
  log_info "Test filter: $test_filter"
  test_cmd+=("--gtest_filter=$test_filter")
fi

if [[ "$test_verbose" == "true" ]]; then
  test_cmd+=("--gtest_print_time=1")
  test_cmd+=("-v")
fi

case "$test_output" in
  xml)
    test_output_file="$root_dir/test-results.xml"
    log_info "Test output: $test_output_file (XML format)"
    test_cmd+=("--gtest_output=xml:$test_output_file")
    ;;
  json)
    test_output_file="$root_dir/test-results.json"
    log_info "Test output: $test_output_file (JSON format)"
    test_cmd+=("--gtest_output=json:$test_output_file")
    ;;
esac

if [[ "$test_repeat" -gt 1 ]]; then
  log_info "Repeating tests $test_repeat times (to find flaky tests)"
  test_cmd+=("--gtest_repeat=$test_repeat")
fi

# Append additional args
test_cmd+=("${test_args[@]}")

# ============================================================================
# Execution
# ============================================================================

log_section "Running Tests"
log_info "Command: ${test_cmd[*]}"
echo ""

# Run tests
if "${test_cmd[@]}"; then
  log_success "All tests passed!"
  exit 0
else
  exit_code=$?
  log_error "Some tests failed (exit code: $exit_code)"
  exit "$exit_code"
fi
