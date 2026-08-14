#!/usr/bin/env bash
# clean-builds.sh — reclaim disk from duplicate CMake trees under core/
#
# TrueTest isolates each CMake preset under out/build/<preset>/ and also
# tolerates ad-hoc dirs (build/, build-*/). Each tree re-fetches FetchContent
# deps, so keeping many warm trees multiplies disk (often multi-GB each).
#
# Default is dry-run (print what would be removed). Pass --apply to delete.
#
# Usage:
#   ./scripts/clean-builds.sh
#   ./scripts/clean-builds.sh --keep linux-tests --keep linux-dev
#   ./scripts/clean-builds.sh --all --apply
#   ./scripts/clean-builds.sh --stale 14 --apply
#   ./scripts/clean-builds.sh --list
#
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

apply=0
do_all=0
list_only=0
stale_days=""
keep_presets=()

usage() {
  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --apply) apply=1; shift ;;
    --all) do_all=1; shift ;;
    --list) list_only=1; shift ;;
    --keep)
      [[ $# -ge 2 ]] || { echo "error: --keep needs a preset name" >&2; exit 2; }
      keep_presets+=("$2"); shift 2 ;;
    --stale)
      [[ $# -ge 2 ]] || { echo "error: --stale needs day count" >&2; exit 2; }
      stale_days="$2"; shift 2 ;;
    -h|--help) usage 0 ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage 2
      ;;
  esac
done

if [[ ${#keep_presets[@]} -eq 0 ]]; then
  keep_presets=(linux-tests)
fi

# Collect candidate roots that are always disposable when empty of keeps.
candidates=()

add_if_exists() {
  local p="$1"
  [[ -e "$p" ]] || return 0
  candidates+=("$p")
}

add_if_exists "build"
add_if_exists "Testing"
add_if_exists "compile_commands.json"

# Ad-hoc build-* trees (build-dev, build-asan, …)
shopt -s nullglob
for d in build-*; do
  [[ -d "$d" ]] && candidates+=("$d")
done

# Preset trees under out/build/*
if [[ -d out/build ]]; then
  for d in out/build/*; do
    [[ -d "$d" ]] || continue
    candidates+=("$d")
  done
fi
# orphan out/ without build children still listed via parent wipe when --all
if [[ -d out ]] && [[ ! -d out/build ]]; then
  candidates+=("out")
fi
shopt -u nullglob

is_kept_preset_tree() {
  local path="$1"
  local base
  base="$(basename -- "$path")"
  local k
  for k in "${keep_presets[@]}"; do
    if [[ "$path" == "out/build/$k" || "$base" == "$k" ]]; then
      return 0
    fi
  done
  return 1
}

human_size() {
  local p="$1"
  if [[ -e "$p" ]]; then
    du -sh "$p" 2>/dev/null | cut -f1
  else
    echo "-"
  fi
}

mtime_days_ago() {
  local p="$1"
  if [[ ! -e "$p" ]]; then
    echo "99999"
    return
  fi
  # portable-ish: seconds since mtime via stat
  local now mt
  now="$(date +%s)"
  mt="$(stat -c %Y "$p" 2>/dev/null || stat -f %m "$p" 2>/dev/null || echo "$now")"
  echo $(( (now - mt) / 86400 ))
}

echo "TrueTest build trees under: $root_dir"
echo "Mode: $([[ $apply -eq 1 ]] && echo APPLY || echo dry-run)"
if [[ $do_all -eq 0 ]]; then
  echo "Keep presets: ${keep_presets[*]}"
fi
if [[ -n "$stale_days" ]]; then
  echo "Stale threshold: ${stale_days} day(s)"
fi
echo

total_bytes=0
printf '%-36s %10s %8s %s\n' "PATH" "SIZE" "AGE_D" "ACTION"
printf '%-36s %10s %8s %s\n' "----" "----" "-----" "------"

to_delete=()

for path in "${candidates[@]+"${candidates[@]}"}"; do
  [[ -e "$path" ]] || continue
  sz="$(human_size "$path")"
  age="$(mtime_days_ago "$path")"
  bytes="$(du -sb "$path" 2>/dev/null | cut -f1 || echo 0)"
  total_bytes=$((total_bytes + bytes))

  action="keep"
  if [[ $do_all -eq 1 ]]; then
    action="delete"
  elif is_kept_preset_tree "$path"; then
    action="keep"
  elif [[ -n "$stale_days" ]]; then
    if [[ "$age" -ge "$stale_days" ]]; then
      action="delete-stale"
    else
      action="keep-fresh"
    fi
  else
    # default: delete everything not in keep list
    action="delete"
  fi

  # Never delete non-build random paths; candidates are already constrained.
  # Keep list always wins unless --all.
  if [[ $do_all -eq 0 ]] && is_kept_preset_tree "$path"; then
    action="keep"
  fi

  printf '%-36s %10s %8s %s\n' "$path" "$sz" "$age" "$action"
  if [[ "$action" == delete || "$action" == delete-stale ]]; then
    to_delete+=("$path")
  fi
done

if [[ ${#candidates[@]} -eq 0 ]]; then
  echo "(no build trees found)"
fi

echo
if command -v numfmt >/dev/null 2>&1; then
  echo "Total listed: $(numfmt --to=iec-i --suffix=B "$total_bytes" 2>/dev/null || echo "${total_bytes} bytes")"
else
  echo "Total listed: ${total_bytes} bytes"
fi

if [[ $list_only -eq 1 ]]; then
  exit 0
fi

if [[ ${#to_delete[@]} -eq 0 ]]; then
  echo "Nothing to delete."
  exit 0
fi

echo
echo "Would remove ${#to_delete[@]} path(s):"
for p in "${to_delete[@]}"; do
  echo "  $p"
done

if [[ $apply -eq 0 ]]; then
  echo
  echo "Dry-run only. Re-run with --apply to delete."
  exit 0
fi

for p in "${to_delete[@]}"; do
  rm -rf -- "$p"
  echo "removed: $p"
done

# Drop empty out/build and out/ shells
if [[ -d out/build ]] && [[ -z "$(find out/build -mindepth 1 -maxdepth 1 2>/dev/null | head -1)" ]]; then
  rmdir out/build 2>/dev/null || true
fi
if [[ -d out ]] && [[ -z "$(find out -mindepth 1 -maxdepth 1 2>/dev/null | head -1)" ]]; then
  rmdir out 2>/dev/null || true
fi

echo "Done."
