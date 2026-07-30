#!/usr/bin/env bash
#
# Phase 0 dry-run-phase0.sh
# Prints the exact shadow-mode command that exercises the full Phase 0
# safety surface (DMS, reconciler, FuturesRiskCheck with caps, depth stream,
# persist) without any live orders. Run this before the first real session
# to validate the build + QuestDB + wiring.
#
# Usage:
#   ./scripts/phase0/dry-run-phase0.sh

set -euo pipefail

DATE_UTC=$(date -u +%Y%m%d_%H%M)
RUN_TAG="phase0_dryrun_${DATE_UTC}"

cat <<EOF
==================================================================
PHASE 0 — DRY-RUN / SHADOW VALIDATION COMMAND
==================================================================

This command uses engine_shadow (never engine_live) so it cannot place
real orders. It exercises the identical provider open() path, DMS
heartbeat thread, venue risk caps, reconciler, depth parsing, and
QuestDB wiring that the real Phase 0 live sessions will use.

Prerequisites before running:
  1. cmake -B build -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON
  2. cmake --build build -j\$(nproc)
  3. QuestDB daemon running and reachable on 127.0.0.1:9000 / 9009
  4. You have a Binance futures API key pair (read-only is sufficient for shadow)

Copy-paste (replace the two export lines if you want real credentials for the
user-data stream; otherwise the provider will still arm DMS and risk checks):

export BINANCE_FUTURES_KEY=...
export BINANCE_FUTURES_SECRET=...

./build/engine_shadow \\
  --provider binance-futures \\
  --symbol BTCUSDT \\
  --stream trade \\
  --depth-stream depth20@100ms \\
  --api-key "\${BINANCE_FUTURES_KEY}" --api-secret "\${BINANCE_FUTURES_SECRET}" \\
  --persist --run-tag ${RUN_TAG} \\
  --reconcile-tolerance-bps 3 \\
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \\
  --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 0.07 \\
  --max-daily-loss 80 --risk-unwind 0.4 \\
  --status-format tui

Watch for in the TUI / logs:
  - "DMS heartbeat" or countdown timer advancing
  - Reconciler result (should pass or give clean [TESTNET-RESET] style advisory)
  - FuturesRiskCheck active with your caps
  - QuestDB "PERSISTENCE ENABLED" (or the expected soft-fail warning)
  - No worker drops over at least 30–60 minutes

When satisfied, stop the shadow run (Ctrl-C) and record the artifacts under
reports/phase0/ops/dry-run-${RUN_TAG}/ if desired.

This dry-run is a non-qualifying infrastructure rehearsal before scheduling
real Session #1 with engine_live + math-captcha. Current Phase 0 gates live in
docs/governance/01-prod.md, docs/todos/01-P0-phase0.md, and reports/phase0/.

==================================================================
EOF
