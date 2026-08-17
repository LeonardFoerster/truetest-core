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
Safe shadow desk launcher

Usage: ./launch-desk.sh [--skip-build] [--print-command] [ENGINE_SHADOW_ARGS...]

This wrapper only builds and runs engine_shadow. Live flags are refused.
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

cmd=("$build_dir/engine_shadow" --desk --mode shadow "${engine_args[@]}")
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
