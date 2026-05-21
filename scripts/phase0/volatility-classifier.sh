#!/usr/bin/env bash
#
# Phase 0 volatility-classifier.sh
# Prints the current recommended method + thresholds for classifying a session
# into High / Medium / Low volatility regime (required for Phase 0 exit: 3 regimes).
#
# For now this is primarily documentation + a place to record the exact rule used.
# In a later iteration it can fetch or compute 7-day realized vol automatically.
#
# Usage:
#   ./scripts/phase0/volatility-classifier.sh
#   ./scripts/phase0/volatility-classifier.sh --for-date 2026-05-22

set -euo pipefail

echo "=================================================================="
echo "PHASE 0 — VOLATILITY REGIME CLASSIFIER"
echo "=================================================================="
echo ""
echo "Recommended external source (auditable):"
echo "  TradingView BTCUSDT chart → 'Realized Vol' indicator (7-day or 14-day annualized)"
echo "  or Coinglass / Binance 'Realized Volatility' widget for BTC."
echo ""
echo "Current thresholds (record the exact value + source for every session):"
echo ""
echo "  High   : > 60% annualized realized vol   OR  clear event-driven spike"
echo "           (FOMC, major liquidation cascade, geopolitical news, etc.)"
echo "  Medium : 35 – 60%"
echo "  Low    : < 35% (quiet ranging days, weekends, holidays, low-volume Asia session)"
echo ""
echo "For every Phase 0 session you MUST record in:"
echo "  reports/phase0/ops/volatility-log.md"
echo "the 7d/14d realized vol value, the source, and the assigned regime."
echo ""
echo "If a day is borderline, note the discussion and the final agreed label."
echo ""
echo "Example entry:"
echo "  2026-05-22 | p0_20260522_0945 | BTCUSDT | High | 72% (7d) | TradingView BTCUSDT RV | FOMC + liquidation cascade"
echo ""
echo "After 15 qualifying sessions you must be able to show at least one (ideally 5)"
echo "in each of the three buckets."
echo "=================================================================="

# Future enhancement: curl a public realized vol endpoint or parse a local file.
# For the campaign the manual + documented external reference is sufficient and auditable.