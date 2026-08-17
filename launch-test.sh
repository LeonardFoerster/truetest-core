#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
preset="linux-tests"
build_dir="$root_dir/out/build/$preset"
skip_build=false
print_command=false
test_args=()

show_help() {
  cat <<'EOF'
TrueTest unit-test launcher

Usage: ./launch-test.sh [--skip-build] [--print-command] [--filter PATTERN] [GTEST_ARGS...]
EOF
}

while (($#)); do
  case "$1" in
    --help|-h) show_help; exit 0 ;;
    --skip-build) skip_build=true; shift ;;
    --print-command) print_command=true; shift ;;
    --filter)
      [[ $# -ge 2 ]] || { echo "launch-test.sh: --filter requires a value" >&2; exit 2; }
      test_args+=("--gtest_filter=$2")
      shift 2
      ;;
    *) test_args+=("$1"); shift ;;
  esac
done

cmd=("$build_dir/truetest_tests" "${test_args[@]}")
if [[ $print_command == true ]]; then
  printf '%q ' "${cmd[@]}"
  printf '\n'
  exit 0
fi

if [[ $skip_build != true ]]; then
  cmake --preset "$preset" -S "$root_dir"
  cmake --build --preset "$preset" -j1 --target truetest_tests
fi

exec "${cmd[@]}"
