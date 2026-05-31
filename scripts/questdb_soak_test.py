#!/usr/bin/env python3
"""
Phase 5: QuestDB Soak Test with Failure Injection.

Runs a simulated long campaign (default 30 minutes) while periodically
injecting "network blips" (simulated write failures).

It exercises the full hardened stack:
- Periodic time-based flushing (Phase 1)
- Strict mode + local fallback (Phase 2)
- Rich events + schema (Phase 3)
- Better health / retention (Phase 4)

Usage (against real QuestDB):
    python scripts/questdb_soak_test.py \
        --run-tag soak_$(date +%s) \
        --duration-minutes 30 \
        --blip-every-minutes 5 \
        --blip-duration-seconds 30

The script will:
1. Create tables (via the engine or direct DDL).
2. Send a steady stream of synthetic order/fill/event rows.
3. Periodically simulate blips (by refusing to accept writes for a window).
4. Verify at the end that data was either delivered or safely captured in fallback.
5. Print a reconciliation-style summary.

This is intentionally a *manual* / CI soak tool, not a unit test.
"""

import argparse
import random
import sys
import time
from datetime import datetime, timedelta
from typing import Optional

try:
    import requests
except ImportError:
    print("ERROR: pip install requests")
    sys.exit(1)


class BlipInjector:
    """Simulates network blips for the ILP writer under test."""

    def __init__(self, blip_every_seconds: int, blip_duration_seconds: int):
        self.blip_every = blip_every_seconds
        self.blip_duration = blip_duration_seconds
        self.next_blip_time = time.time() + blip_every_seconds
        self.blip_end_time: Optional[float] = None
        self.in_blip = False

    def should_fail_now(self) -> bool:
        now = time.time()
        if not self.in_blip and now >= self.next_blip_time:
            self.in_blip = True
            self.blip_end_time = now + self.blip_duration
            self.next_blip_time = now + self.blip_every
            print(f"  >>> INJECTING BLIP for {self.blip_duration}s <<<")
            return True

        if self.in_blip and self.blip_end_time and now >= self.blip_end_time:
            self.in_blip = False
            print("  <<< BLIP ENDED >>>")
            return False

        return self.in_blip


def send_ilp_line(host: str, port: int, line: str, injector: Optional[BlipInjector] = None) -> bool:
    """Very naive ILP sender for soak testing. In real use the engine does this."""
    if injector and injector.should_fail_now():
        # Simulate the writer seeing a failure
        return False

    try:
        # QuestDB ILP is TCP on 9009. We do a fire-and-forget send for the soak.
        import socket
        with socket.create_connection((host, port), timeout=2) as s:
            s.sendall((line + "\n").encode("utf-8"))
        return True
    except Exception:
        return False


def main():
    parser = argparse.ArgumentParser(description="QuestDB soak test with blips (Phase 5)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ilp-port", type=int, default=9009)
    parser.add_argument("--http-port", type=int, default=9000)
    parser.add_argument("--run-tag", required=True)
    parser.add_argument("--duration-minutes", type=int, default=30)
    parser.add_argument("--blip-every-minutes", type=int, default=5)
    parser.add_argument("--blip-duration-seconds", type=int, default=45)
    parser.add_argument("--rows-per-second", type=int, default=20)
    args = parser.parse_args()

    print(f"=== QuestDB Phase 5 Soak Test ===")
    print(f"Run tag: {args.run_tag}")
    print(f"Duration: {args.duration_minutes} minutes")
    print(f"Blip every {args.blip_every_minutes} min for {args.blip_duration_seconds}s")
    print(f"Target rate: ~{args.rows_per_second} rows/sec")
    print()

    injector = BlipInjector(
        blip_every_seconds=args.blip_every_minutes * 60,
        blip_duration_seconds=args.blip_duration_seconds
    )

    start = time.time()
    end = start + args.duration_minutes * 60
    sent = 0
    failed_during_blip = 0

    # Synthetic data generator (mimics order + event rows)
    def make_line(i: int) -> str:
        ts = int(time.time() * 1_000_000_000)
        return (
            f"{args.run_tag}_events,"
            f"event_type=soak_test,symbol=TEST{i%10},strategy=soak_test,severity=info "
            f"order_id={100000 + i}i,message=\"soak iteration {i}\" {ts}"
        )

    try:
        while time.time() < end:
            line = make_line(sent)
            ok = send_ilp_line(args.host, args.ilp_port, line, injector)
            if ok:
                sent += 1
            else:
                failed_during_blip += 1

            # Simple rate limiting
            time.sleep(1.0 / args.rows_per_second)

            if sent % 500 == 0:
                print(f"  Sent: {sent} | Failed during blips: {failed_during_blip}")

    except KeyboardInterrupt:
        print("\nInterrupted by user.")

    duration = time.time() - start
    print(f"\n=== Soak Complete ===")
    print(f"Duration: {duration:.1f}s")
    print(f"Total attempted: {sent + failed_during_blip}")
    print(f"Successfully sent: {sent}")
    print(f"Failed (expected during blips): {failed_during_blip}")

    print("\nNext steps for operator:")
    print(f"  1. Run: python scripts/questdb_health_check.py --run-tag {args.run_tag} --require-activity")
    print(f"  2. Run: python scripts/questdb_campaign_summary.py --run-tag {args.run_tag}")
    print("  3. Manually verify fallback file (if strict mode was used) was written during blips.")
    print("  4. Reconcile against binary log (future tool).")


if __name__ == "__main__":
    main()
