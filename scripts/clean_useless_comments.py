#!/usr/bin/env python3
"""
Remove useless comments from C++ source files:
1. Closing namespace comments: "} // namespace" and "} // namespace foo"
2. Empty comment lines: lines that are only whitespace + "//" + optional whitespace

Usage:
  python3 scripts/clean_useless_comments.py --dry-run   # show what would change
  python3 scripts/clean_useless_comments.py --apply     # actually edit files
"""

import os
import re
import sys
import argparse
from pathlib import Path

ROOT = Path(__file__).parent.parent
SRC_DIRS = [ROOT / "src", ROOT / "tests"]

def is_useless_namespace_closer(line):
    """Match lines like:    } // namespace   or   } // namespace foo::bar"""
    # Keep the leading whitespace and the '}'
    m = re.match(r'^(\s*)}(\s*//.*namespace.*)$', line)
    if m:
        return True, m.group(1) + '}'  # return (is_match, replacement_line)
    return False, None

def is_empty_comment_line(line):
    """Match lines that are only whitespace followed by // and optional whitespace."""
    return bool(re.match(r'^\s*//\s*$', line))

def process_file(path, apply=False):
    """Process a single file. Returns (original_comment_lines, new_comment_lines, changed)."""
    try:
        with open(path, 'r', encoding='utf-8', errors='ignore') as f:
            original_lines = f.readlines()
    except Exception as e:
        return 0, 0, False, str(e)

    new_lines = []
    changed = False

    for line in original_lines:
        is_ns, replacement = is_useless_namespace_closer(line)
        if is_ns:
            new_lines.append(replacement + '\n')
            changed = True
            continue

        if is_empty_comment_line(line):
            changed = True
            # skip the line entirely
            continue

        new_lines.append(line)

    if not changed:
        return 0, 0, False, None

    # Count comments in original vs new
    orig_content = ''.join(original_lines)
    new_content = ''.join(new_lines)

    orig_lc, orig_clc = count_comment_lines(orig_content)
    new_lc, new_clc = count_comment_lines(new_content)

    if apply:
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)

    return orig_clc, new_clc, True, None

def count_comment_lines(content):
    """Accurate comment line counter (same logic as baseline script)."""
    n = len(content)
    pos = 0
    line_num = 1
    comment_line_nums = set()

    in_line_comment = False
    in_block_comment = False
    in_string = False
    string_char = None
    in_raw_string = False
    raw_delim = ""

    while pos < n:
        c = content[pos]
        next_c = content[pos+1] if pos+1 < n else ''

        if c == '\n':
            line_num += 1
            if in_line_comment:
                in_line_comment = False
            pos += 1
            continue

        if in_line_comment:
            comment_line_nums.add(line_num)
            pos += 1
            continue

        if in_block_comment:
            comment_line_nums.add(line_num)
            if c == '*' and next_c == '/':
                in_block_comment = False
                pos += 2
            else:
                pos += 1
            continue

        if in_raw_string:
            if c == ')' and pos+1 < n:
                end_marker = ')"' + raw_delim
                if content[pos:pos+len(end_marker)] == end_marker:
                    in_raw_string = False
                    pos += len(end_marker)
                    continue
            pos += 1
            continue

        if in_string:
            if c == '\\':
                pos += 2
                continue
            if c == string_char:
                in_string = False
            pos += 1
            continue

        if c == '/' and next_c == '/':
            in_line_comment = True
            comment_line_nums.add(line_num)
            pos += 2
            continue

        if c == '/' and next_c == '*':
            in_block_comment = True
            comment_line_nums.add(line_num)
            pos += 2
            continue

        if c == 'R' and next_c == '"':
            pos += 2
            delim = ""
            while pos < n and content[pos] != '(':
                delim += content[pos]
                pos += 1
            if pos < n and content[pos] == '(':
                raw_delim = delim
                in_raw_string = True
            pos += 1
            continue

        if c in ('"', "'"):
            in_string = True
            string_char = c
            pos += 1
            continue

        pos += 1

    total_lines = content.count('\n') + (0 if content.endswith('\n') else 1)
    return total_lines, len(comment_line_nums)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true', help='Actually write changes (default is dry-run)')
    ap.add_argument('--verbose', '-v', action='store_true')
    args = ap.parse_args()

    all_files = []
    for d in SRC_DIRS:
        for p in d.rglob('*'):
            if p.is_file() and p.suffix in ('.cpp', '.h', '.hpp'):
                if 'cmake-build' not in str(p) and '.claude' not in str(p):
                    all_files.append(p)

    all_files.sort()

    total_orig_comments = 0
    total_new_comments = 0
    files_changed = 0
    errors = []

    print(f"Scanning {len(all_files)} files...")
    print()

    for path in all_files:
        orig_clc, new_clc, changed, err = process_file(path, apply=args.apply)
        if err:
            errors.append((path, err))
            continue
        total_orig_comments += orig_clc
        total_new_comments += new_clc
        if changed:
            files_changed += 1
            delta = orig_clc - new_clc
            rel = path.relative_to(ROOT)
            if args.verbose or delta > 0:
                print(f"  {rel}: {orig_clc} → {new_clc} comments (-{delta})")

    print()
    print("=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"Files scanned:     {len(all_files)}")
    print(f"Files changed:     {files_changed}")
    print(f"Pre-clean comments:  {total_orig_comments}")
    print(f"Post-clean comments: {total_new_comments}")
    print(f"Comments removed:    {total_orig_comments - total_new_comments}")
    print(f"Reduction:           {100*(total_orig_comments-total_new_comments)/total_orig_comments:.1f}%")
    print()

    if errors:
        print("Errors:")
        for p, e in errors:
            print(f"  {p}: {e}")

    if not args.apply:
        print("DRY RUN - no files were modified. Use --apply to write changes.")

if __name__ == '__main__':
    main()
