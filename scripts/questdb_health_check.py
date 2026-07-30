#!/usr/bin/env python3
"""
Phase 4: Lightweight QuestDB health check for long-running campaigns.

Can be called from operator SOPs, cron, or CI pipelines.

Checks:
- QuestDB is reachable (HTTP /exec ping)
- For a given --run-tag: at least one recent row exists in key tables
  (orders, fills, events) within the last N minutes.

Exit code 0 = healthy, non-zero = issues found.
"""

import argparse
import sys
from datetime import datetime, timedelta

from questdb_common import validate_run_tag

try:
    import requests
except ImportError:
    print("ERROR: 'requests' package required. pip install requests")
    sys.exit(2)


def ping_questdb(host: str, port: int, timeout=5) -> bool:
    try:
        url = f"http://{host}:{port}/exec"
        r = requests.get(url, params={"query": "SELECT 1"}, timeout=timeout)
        return r.status_code == 200 and "dataset" in r.json()
    except Exception as e:
        print(f"QuestDB ping failed: {e}")
        return False


def has_recent_rows(host: str, port: int, run_tag: str, minutes: int = 30) -> dict:
    cutoff = (datetime.utcnow() - timedelta(minutes=minutes)).isoformat() + "Z"
    tables = ["orders", "fills", "events"]
    results = {}

    for t in tables:
        sql = f"""
            SELECT count(*) 
            FROM {run_tag}_{t} 
            WHERE ts > '{cutoff}'
        """
        try:
            url = f"http://{host}:{port}/exec"
            r = requests.get(url, params={"query": sql}, timeout=10)
            data = r.json()
            if data.get("error"):
                results[t] = f"error: {data['error']}"
            else:
                count = data.get("dataset", [[0]])[0][0]
                results[t] = int(count)
        except Exception as e:
            results[t] = f"error: {e}"

    return results


def main():
    parser = argparse.ArgumentParser(description="QuestDB health check for campaign runs")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--run-tag", help="Run tag to check for recent activity")
    parser.add_argument("--recent-minutes", type=int, default=30,
                        help="Consider rows within last N minutes as 'recent'")
    parser.add_argument("--require-activity", action="store_true",
                        help="Fail if no recent rows found in key tables")
    args = parser.parse_args()

    run_tag = validate_run_tag(args.run_tag) if args.run_tag else None
    print(f"QuestDB Health Check @ {args.host}:{args.port}")

    healthy = ping_questdb(args.host, args.port)
    if not healthy:
        print("❌ QuestDB is unreachable")
        sys.exit(1)

    print("✅ QuestDB is reachable")

    if run_tag:
        print(f"\nChecking recent activity for run_tag={run_tag} (last {args.recent_minutes} min)")
        counts = has_recent_rows(args.host, args.port, run_tag, args.recent_minutes)

        total_recent = 0
        for table, count in counts.items():
            if isinstance(count, int):
                print(f"  {table}: {count} recent rows")
                total_recent += count
            else:
                print(f"  {table}: {count}")

        if args.require_activity and total_recent == 0:
            print("\n❌ No recent activity found in any key table")
            sys.exit(2)

    print("\n✅ Health check passed")
    sys.exit(0)


if __name__ == "__main__":
    main()
