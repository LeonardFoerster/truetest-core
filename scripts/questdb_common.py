"""Shared helpers for QuestDB operator scripts."""

import re


RUN_TAG_PATTERN = r"[A-Za-z0-9_]{1,64}"
RUN_TAG_RE = re.compile(rf"^{RUN_TAG_PATTERN}$")


def validate_run_tag(run_tag: str) -> str:
    if not RUN_TAG_RE.fullmatch(run_tag):
        raise SystemExit(f"invalid --run-tag: must match {RUN_TAG_PATTERN}")
    return run_tag
