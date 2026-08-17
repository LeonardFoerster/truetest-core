#!/usr/bin/env bash
# Run the fixed, argumentless Codex quality pipeline for the current worktree.
set -euo pipefail

if (($# != 0)); then
    echo "Usage: $0" >&2
    echo "This workflow accepts no arguments; the current worktree is always the task." >&2
    exit 64
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
skill_root="$repo_root/.agents/skills/local-quality-loop"
gate_result="$(mktemp "${TMPDIR:-/tmp}/local-quality-loop-gates.XXXXXX.json")"
push_result="$(mktemp "${TMPDIR:-/tmp}/local-quality-loop-push.XXXXXX.json")"
trap 'rm -f "$gate_result" "$push_result"' EXIT

codex -a never exec \
    -C "$repo_root" \
    -s workspace-write \
    --output-schema "$skill_root/scripts/gate-result.schema.json" \
    --output-last-message "$gate_result" \
    - <"$skill_root/assets/fixed-prompt.md"

python3 "$skill_root/scripts/validate-pipeline-result.py" "$gate_result"
gate_digest="$(sha256sum "$gate_result" | awk '{print $1}')"
pre_push_head="$(git -C "$repo_root" rev-parse HEAD)"
pre_push_branch="$(git -C "$repo_root" branch --show-current)"
pre_push_upstream="$(git -C "$repo_root" rev-parse "refs/remotes/origin/$pre_push_branch" 2>/dev/null || true)"
pre_push_tree="$(bash "$skill_root/scripts/worktree-tree-oid.sh")"
gate_fingerprint="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["verified_fingerprint"])' "$gate_result")"
current_fingerprint="$(bash "$skill_root/scripts/worktree-fingerprint.sh")"
if [[ "$current_fingerprint" != "$gate_fingerprint" ]]; then
    echo "LOCAL QUALITY LOOP: FAIL - worktree changed before push phase" >&2
    exit 1
fi
pre_push_dirty=0
if [[ -n "$(git -C "$repo_root" status --porcelain --untracked-files=all)" ]]; then
    pre_push_dirty=1
fi

push_prompt="$(<"$skill_root/assets/fixed-push-prompt.md")"
codex --approve-for-me exec \
    -C "$repo_root" \
    -s workspace-write \
    --output-schema "$skill_root/scripts/push-result.schema.json" \
    --output-last-message "$push_result" \
    "$push_prompt" <"$gate_result"

if [[ "$(sha256sum "$gate_result" | awk '{print $1}')" != "$gate_digest" ]]; then
    echo "LOCAL QUALITY LOOP: FAIL - validated gate evidence changed during push phase" >&2
    exit 1
fi

python3 "$skill_root/scripts/validate-pipeline-result.py" \
    "$gate_result" \
    "$push_result" \
    "$pre_push_head" \
    "$pre_push_upstream" \
    "$pre_push_branch" \
    "$pre_push_dirty" \
    "$pre_push_tree"
