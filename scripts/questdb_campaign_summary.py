#!/usr/bin/env python3
"""
Phase 4: Simple golden query helper for long-running QuestDB campaigns.

Usage:
    python scripts/questdb_campaign_summary.py --run-tag myrun_2026xxxx --host localhost

Prints a compact campaign summary using the richer runs_meta (Phase 4)
and basic per-run table counts.
"""

import argparse
import sys

from questdb_common import validate_run_tag

try:
    import requests
except ImportError:
    print("This helper requires 'requests'. Install with: pip install requests")
    sys.exit(1)


def query_questdb(host: str, port: int, sql: str):
    url = f"http://{host}:{port}/exec"
    resp = requests.get(url, params={"query": sql}, timeout=30)
    resp.raise_for_status()
    data = resp.json()
    if data.get("error"):
        raise RuntimeError(data["error"])
    return data.get("dataset", [])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-tag", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    args = parser.parse_args()

    rt = validate_run_tag(args.run_tag)

    print(f"=== QuestDB Campaign Summary: {rt} ===")

    # Rich summary from runs_meta (Phase 4 columns)
    summary_sql = f"""
        SELECT 
            strategy,
            final_equity - initial_equity as pnl,
            max_drawdown,
            sharpe_ratio,
            profit_factor,
            win_rate,
            total_trades
        FROM runs_meta
        WHERE run_tag = '{rt}'
          AND ended_at IS NOT NULL
        LIMIT 1
    """
    rows = query_questdb(args.host, args.port, summary_sql)
    if rows:
        print("Performance (from richer runs_meta):")
        for r in rows:
            print(f"  Strategy: {r[0]}")
            print(f"  PnL: {r[1]:.2f}")
            print(f"  Max DD: {r[2]:.2f}%")
            print(f"  Sharpe: {r[3]:.3f}")
            print(f"  Profit Factor: {r[4]:.3f}")
            print(f"  Win Rate: {r[5]:.1%}")
            print(f"  Total Trades: {r[6]}")
    else:
        print("No completed run found in runs_meta.")

    # Basic table row counts
    tables = ["orders", "fills", "events", "rejections"]
    print("\nRow counts:")
    for t in tables:
        sql = f"SELECT COUNT(*) FROM {rt}_{t}"
        try:
            cnt = query_questdb(args.host, args.port, sql)[0][0]
            print(f"  {t}: {cnt}")
        except Exception as e:
            print(f"  {t}: (table not found or error: {e})")

    print("\nDone.")


if __name__ == "__main__":
    main()
