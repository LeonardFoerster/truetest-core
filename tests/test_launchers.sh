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

count_occurrences() {
  local text="$1"
  local needle="$2"
  local count=0
  while [[ $text == *"$needle"* ]]; do
    text="${text#*"$needle"}"
    count=$((count + 1))
  done
  printf '%d' "$count"
}

expect_count() {
  local command="$1"
  local option="$2"
  local expected="$3"
  local actual
  actual="$(count_occurrences "$command" "$option")"
  if [[ $actual != "$expected" ]]; then
    echo "expected $expected occurrence(s) of $option, got $actual: $command" >&2
    exit 1
  fi
}

# No source options: bounded, deterministic synthetic defaults are complete.
[[ $desk_cmd == *'--provider synthetic'* ]]
[[ $desk_cmd == *'--strategy sma'* ]]
[[ $desk_cmd == *'n_steps=500'* ]]
[[ $desk_cmd == *'--seed 424242'* ]]
expect_count "$desk_cmd" --provider 1
expect_count "$desk_cmd" --strategy 1
expect_count "$desk_cmd" --mc-params 1
expect_count "$desk_cmd" --seed 1

# Partial overrides must retain the other independent synthetic defaults.
strategy_cmd="$($root_dir/launch-desk.sh --print-command --strategy momentum)"
[[ $strategy_cmd == *'--provider synthetic'* ]]
[[ $strategy_cmd == *'--strategy momentum'* ]]
[[ $strategy_cmd != *'--strategy sma'* ]]
[[ $strategy_cmd == *'n_steps=500'* ]]
[[ $strategy_cmd == *'--seed 424242'* ]]
expect_count "$strategy_cmd" --strategy 1

params_cmd="$($root_dir/launch-desk.sh --print-command --mc-params n_steps=37)"
[[ $params_cmd == *'--provider synthetic'* ]]
[[ $params_cmd == *'--strategy sma'* ]]
[[ $params_cmd == *'--mc-params n_steps=37'* ]]
[[ $params_cmd != *'n_steps=500'* ]]
expect_count "$params_cmd" --mc-params 1

seed_cmd="$($root_dir/launch-desk.sh --print-command --seed=7)"
[[ $seed_cmd == *'--seed=7'* ]]
[[ $seed_cmd != *'--seed 424242'* ]]
expect_count "$seed_cmd" --seed 1

# A data path selects local; non-synthetic sources receive no synthetic-only
# strategy, generator, or seed defaults.
path_cmd="$($root_dir/launch-desk.sh --print-command --path fixture.csv)"
[[ $path_cmd == *'--provider local'* ]]
[[ $path_cmd != *'--provider synthetic'* ]]
expect_count "$path_cmd" --provider 1
expect_count "$path_cmd" --strategy 0
expect_count "$path_cmd" --mc-params 0
expect_count "$path_cmd" --seed 0

local_cmd="$($root_dir/launch-desk.sh --print-command --provider=local)"
[[ $local_cmd == *'--provider=local'* ]]
expect_count "$local_cmd" --provider 1
expect_count "$local_cmd" --strategy 0
expect_count "$local_cmd" --mc-params 0
expect_count "$local_cmd" --seed 0

# A fully specified synthetic launch is forwarded exactly once per option.
custom_cmd="$($root_dir/launch-desk.sh --print-command --provider synthetic --strategy breakout --mc-params n_steps=41 --seed 9)"
[[ $custom_cmd == *'--provider synthetic'* ]]
[[ $custom_cmd == *'--strategy breakout'* ]]
[[ $custom_cmd == *'--mc-params n_steps=41'* ]]
[[ $custom_cmd == *'--seed 9'* ]]
expect_count "$custom_cmd" --provider 1
expect_count "$custom_cmd" --strategy 1
expect_count "$custom_cmd" --mc-params 1
expect_count "$custom_cmd" --seed 1

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
