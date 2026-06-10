#!/usr/bin/env python3
"""
Phase 5: Simple post-run reconciliation between QuestDB and binary log.

In a real setup the binary log (.ttlog.zst) would be parsed to count events.
This script provides the QuestDB side + a placeholder for the binary side.

For now it prints counts from key tables and suggests the manual step.
"""

import argparse
import sys

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)


def count_table(host: str, port: int, table: str) -> int:
    sql = f"SELECT count(*) FROM {table}"
    url = f"http://{host}:{port}/exec"
    r = requests.get(url, params={"query": sql}, timeout=15)
    data = r.json()
    if data.get("error"):
        return -1
    return data.get("dataset", [[0]])[0][0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-tag", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    args = parser.parse_args()

    rt = args.run_tag
    print(f"=== QuestDB vs Binary Log Reconciliation ===")
    print(f"Run tag: {rt}\n")

    tables = ["orders", "fills", "events", "rejections"]
    for t in tables:
        cnt = count_table(args.host, args.port, f"{rt}_{t}")
        print(f"QuestDB {t}: {cnt}")

    print("\nManual step (operator SOP):")
    print("  1. Parse the corresponding binary log for the same run_tag.")
    print("  2. Compare counts (within tolerance for timing).")
    print("  3. Sign off in the campaign evidence bundle if they match.")


if __name__ == "__main__":
    main()
