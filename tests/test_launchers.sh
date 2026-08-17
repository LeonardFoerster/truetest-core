#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

bash -n "$root_dir/launch-desk.sh" "$root_dir/launch-test.sh"
"$root_dir/launch-desk.sh" --help >/dev/null
"$root_dir/launch-test.sh" --help >/dev/null

desk_cmd="$($root_dir/launch-desk.sh --print-command --symbol 'BTC USDT')"
[[ $desk_cmd == *'/linux-dev/engine_shadow '* ]]
[[ $desk_cmd == *'--mode shadow'* ]]
[[ $desk_cmd == *'BTC\ USDT'* ]]

test_cmd="$($root_dir/launch-test.sh --print-command --filter 'Registry*')"
[[ $test_cmd == *'/linux-tests/truetest_tests '* ]]
[[ $test_cmd == *'--gtest_filter=Registry\*'* ]]

if "$root_dir/launch-desk.sh" --print-command --live >/dev/null 2>&1; then
  echo "launcher accepted --live" >&2
  exit 1
fi
if "$root_dir/launch-desk.sh" --print-command --live=true >/dev/null 2>&1; then
  echo "launcher accepted --live=true" >&2
  exit 1
fi
if TT_MODE=live "$root_dir/launch-desk.sh" --print-command >/dev/null 2>&1; then
  echo "launcher accepted TT_MODE=live" >&2
  exit 1
fi
if "$root_dir/launch-desk.sh" --mode backtest --print-command >/dev/null 2>&1; then
  echo "launcher accepted a non-shadow mode override" >&2
  exit 1
fi
