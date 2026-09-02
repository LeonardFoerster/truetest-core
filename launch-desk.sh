#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
preset="linux-dev"
build_dir="$root_dir/out/build/$preset"
skip_build=false
print_command=false
engine_args=()

if [[ ${TT_MODE-} == live ]]; then
  echo "launch-desk.sh: TT_MODE=live is not supported" >&2
  exit 2
fi

show_help() {
  cat <<'EOF'
TrueTest trading command-center launcher

Usage: ./launch-desk.sh [--skip-build] [--print-command] [ENGINE_SHADOW_ARGS...]

Builds and runs the shadow ImGui desk. With no provider/path arguments it uses
the deterministic synthetic provider (SMA, 500 steps, seed 424242) so the command center
opens immediately instead of entering the interactive provider menu.

Explicit provider/path arguments override the synthetic defaults. Live flags,
credentials, and mode overrides are refused.
EOF
}

while (($#)); do
  case "$1" in
    --help|-h) show_help; exit 0 ;;
    --skip-build) skip_build=true; shift ;;
    --print-command) print_command=true; shift ;;
    --live|--live=*|--mode=live|--api-key|--api-secret|--api-passphrase|--api-key=*|--api-secret=*|--api-passphrase=*)
      echo "launch-desk.sh: live mode is not supported" >&2
      exit 2
      ;;
    --mode|--mode=*)
      echo "launch-desk.sh: mode overrides are not supported; this wrapper is shadow-only" >&2
      exit 2
      ;;
    *) engine_args+=("$1"); shift ;;
  esac
done

has_option() {
  local option="$1"
  local arg
  for arg in "${engine_args[@]}"; do
    if [[ "$arg" == "$option" || "$arg" == "$option="* ]]; then
      return 0
    fi
  done
  return 1
}

has_synthetic_provider() {
  local index
  for ((index = 0; index < ${#engine_args[@]}; ++index)); do
    if [[ "${engine_args[index]}" == --provider=synthetic ]]; then
      return 0
    fi
    if [[ "${engine_args[index]}" == --provider && "${engine_args[index + 1]-}" == synthetic ]]; then
      return 0
    fi
  done
  return 1
}

# A desk launch should be immediately useful and must not fall into the
# interactive provider menu when no data source was supplied. Source selection
# and each synthetic default are independent so a partial caller override does
# not create duplicate options.
launch_defaults=()
if ! has_option --thread-preset; then
  launch_defaults+=(--thread-preset inline)
fi
if ! has_option --no-pin; then
  launch_defaults+=(--no-pin)
fi
if ! has_option --status-format && ! has_option --no-tui; then
  launch_defaults+=(--status-format off)
fi
use_synthetic_provider=false
if ! has_option --provider; then
  if has_option --path; then
    launch_defaults+=(--provider local)
  else
    launch_defaults+=(--provider synthetic)
    use_synthetic_provider=true
  fi
elif has_synthetic_provider; then
  use_synthetic_provider=true
fi

if [[ $use_synthetic_provider == true ]]; then
  if ! has_option --strategy; then
    launch_defaults+=(--strategy sma)
  fi
  if ! has_option --mc-params; then
    launch_defaults+=(--mc-params 'mu=0.0,sigma=0.1,n_steps=500')
  fi
  if ! has_option --seed; then
    launch_defaults+=(--seed 424242)
  fi
fi

cmd=("$build_dir/engine_shadow" --desk --mode shadow "${launch_defaults[@]}" "${engine_args[@]}")
if [[ $print_command == true ]]; then
  printf '%q ' "${cmd[@]}"
  printf '\n'
  exit 0
fi

if [[ $skip_build != true ]]; then
  cmake --preset "$preset" -S "$root_dir"
  cmake --build --preset "$preset" -j1 --target engine_shadow
fi

exec "${cmd[@]}"
