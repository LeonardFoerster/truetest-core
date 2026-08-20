#!/usr/bin/env python3
"""Independent reference for the inventory-aware market-making strategy (R1).

This file re-derives the documented semantics from
docs/internal/r1-inventory-aware-market-making.md in exact rational
arithmetic. It deliberately shares no code, no headers and no build with the
C++ implementation: the only thing the two have in common is the
specification.

The C++ suite (tests/test_mm_strategy_golden.cpp) compares its decisions
against the expected file this script writes.

Exactness contract
------------------
The C++ engine evaluates in IEEE-754 double; this reference evaluates in
Fraction. The two therefore agree only if every rounding step lands
comfortably away from a boundary. Rather than assume that, generation
verifies it: every floor/ceil-to-tick and every nearest-atom rounding must sit
at least ``BOUNDARY_MARGIN`` away from its boundary, otherwise generation
fails and the fixture has to be changed. That is what licenses the exact
comparison on the C++ side.

Usage:
    python3 tests/reference/mm_strategy_reference.py --write
    python3 tests/reference/mm_strategy_reference.py --check
"""

from __future__ import annotations

import argparse
import json
import sys
from fractions import Fraction
from pathlib import Path

PRICE_SCALE = 10_000        # Price::SCALE
ATOM_SCALE = 100_000_000    # tt::quantity_scale::canonical_atoms
BPS = Fraction(10_000)

# Distance a value must keep from a rounding boundary for the exact/double
# comparison to be legitimate. 1e-6 raw units is ~1e-15 relative at a 60k
# price, i.e. several orders of magnitude above double round-off.
BOUNDARY_MARGIN = Fraction(1, 1_000_000)

HERE = Path(__file__).resolve().parent
GOLDEN_DIR = HERE.parent / "golden" / "mm"
CASES_PATH = GOLDEN_DIR / "cases.json"
CONFIG_PATH = GOLDEN_DIR / "reference_config.json"
EXPECTED_PATH = GOLDEN_DIR / "expected.json"


class FixtureError(RuntimeError):
    pass


# ── decimal-string conversion (exact, never via float) ──────────────────────


def decimal(value: str) -> Fraction:
    return Fraction(value)


def price_raw(value: str) -> int:
    scaled = decimal(value) * PRICE_SCALE
    if scaled.denominator != 1:
        raise FixtureError(f"price {value!r} is finer than the 1e-4 Price grid")
    return int(scaled)


def atoms(value: str) -> int:
    scaled = decimal(value) * ATOM_SCALE
    if scaled.denominator != 1:
        raise FixtureError(f"quantity {value!r} is finer than the 1e-8 atom grid")
    return int(scaled)


def price_text(raw: int) -> str:
    sign = "-" if raw < 0 else ""
    a = abs(raw)
    return f"{sign}{a // PRICE_SCALE}.{a % PRICE_SCALE:04d}"


def atom_text(value: int) -> str:
    sign = "-" if value < 0 else ""
    a = abs(value)
    return f"{sign}{a // ATOM_SCALE}.{a % ATOM_SCALE:08d}"


def cfg_num(value) -> Fraction:
    """Config doubles enter as the exact value the JSON literal denotes."""
    return Fraction(str(value))


# ── rounding with boundary verification ─────────────────────────────────────


def guard(value: Fraction, boundary: Fraction, what: str) -> None:
    if abs(value - boundary) < BOUNDARY_MARGIN:
        raise FixtureError(
            f"{what}: value {float(value)!r} sits within {float(BOUNDARY_MARGIN)} of a "
            "rounding boundary; the fixture must be moved away from it"
        )


def guard_integer_boundary(q: Fraction, what: str) -> int:
    """Both floor and ceil must be unambiguous, so q may not sit near either
    adjacent integer."""
    floor_q = q.numerator // q.denominator
    frac = q - floor_q
    if frac < BOUNDARY_MARGIN or frac > Fraction(1) - BOUNDARY_MARGIN:
        raise FixtureError(
            f"{what}: quotient {float(q)!r} sits within {float(BOUNDARY_MARGIN)} of an "
            "integer tick boundary; the fixture must be moved away from it"
        )
    return floor_q


def floor_to_tick(value: Fraction, tick: int, what: str) -> int:
    q = value / tick
    return guard_integer_boundary(q, what) * tick


def ceil_to_tick(value: Fraction, tick: int, what: str) -> int:
    q = value / tick
    return (guard_integer_boundary(q, what) + 1) * tick


def nearest_int(value: Fraction, what: str) -> int:
    """Round half away from zero, matching std::llround."""
    floor_v = value.numerator // value.denominator
    guard(value, Fraction(floor_v) + Fraction(1, 2), what)
    frac = value - floor_v
    return floor_v + (1 if frac > Fraction(1, 2) else 0)


def lot_floor(value: int, lot: int) -> int:
    if value <= 0 or lot <= 0:
        return 0
    return (value // lot) * lot


def clamp(value: Fraction, lo: Fraction, hi: Fraction) -> Fraction:
    return max(lo, min(hi, value))


# ── the reference model ─────────────────────────────────────────────────────


def evaluate(case: dict, cfg: dict) -> dict:
    market = case["market"]
    inv = case["inventory"]
    ins = case["instrument"]

    tick = price_raw(ins["tick_size"])
    lot = atoms(ins["lot_size"])
    min_qty = atoms(ins["min_qty"]) if "min_qty" in ins else 0
    maker_fee_bps = cfg_num(ins.get("maker_fee_bps", 0.0))

    decision_time = int(market["decision_time_ns"])
    event_time = int(market["event_time_ns"])
    receive_time = int(market["receive_time_ns"])
    age_ns = decision_time - receive_time

    result = {
        "name": case["name"],
        "state": "PAUSED",
        "market_snapshot_id": snapshot_id(market["snapshot_id"]),
        "market_age_ns": age_ns,
        "reason_codes": [],
        "quotes": [],
    }

    bid = price_raw(market["best_bid"]["price"])
    ask = price_raw(market["best_ask"]["price"])
    bid_qty = atoms(market["best_bid"]["qty"])
    ask_qty = atoms(market["best_ask"]["qty"])

    if (
        bid <= 0
        or ask <= 0
        or ask <= bid
        or bid_qty < 0
        or ask_qty < 0
        or (bid_qty == 0 and ask_qty == 0)
        or event_time > decision_time
        or receive_time > decision_time
    ):
        result["reason_codes"] = ["INVALID_MARKET_STATE"]
        return result

    must_pause = False
    reasons: list[str] = []

    if not market.get("sequence_valid", True):
        reasons.append("SEQUENCE_GAP")
        if cfg["safety"]["pause_on_sequence_gap"]:
            must_pause = True

    if age_ns > int(cfg["safety"]["max_market_data_age_ms"]) * 1_000_000:
        reasons.append("STALE_MARKET_DATA")
        must_pause = True

    if not inv.get("authoritative", True):
        reasons.append("UNKNOWN_INVENTORY")
        if cfg["safety"]["require_authoritative_inventory"]:
            must_pause = True

    if must_pause:
        result["reason_codes"] = reasons
        return result

    hard = atoms(cfg["inventory"]["hard_limit_base"])
    if "hard_limit" in inv:
        ledger_hard = atoms(inv["hard_limit"])
        if ledger_hard > 0:
            hard = min(hard, ledger_hard)

    # ── fair value ─────────────────────────────────────────────────────────
    mid = Fraction(bid + ask, 2)
    size_sum = bid_qty + ask_qty
    microprice = Fraction(ask * bid_qty + bid * ask_qty, size_sum)
    imbalance = Fraction(bid_qty - ask_qty, size_sum)
    book_half_spread = Fraction(ask - bid, 2)
    flow = cfg_num(market.get("short_flow_signal", 0.0))

    fair = (
        mid
        + cfg_num(cfg["fair_value"]["microprice_weight"]) * (microprice - mid)
        + cfg_num(cfg["fair_value"]["imbalance_weight"]) * imbalance * book_half_spread
        + cfg_num(cfg["fair_value"]["short_flow_weight"]) * flow * book_half_spread
    )

    # ── inventory ──────────────────────────────────────────────────────────
    position = atoms(inv["signed_base_position"])
    u = clamp(Fraction(position, hard), Fraction(-1), Fraction(1))
    abs_u = abs(u)

    soft = cfg_num(cfg["inventory"]["soft_limit_ratio"])
    reducing = cfg_num(cfg["inventory"]["reducing_bias_ratio"])
    boost_cfg = cfg_num(cfg["inventory"]["soft_limit_skew_boost"])

    boost = Fraction(1)
    if boost_cfg > 0 and abs_u > soft:
        boost += boost_cfg * ((abs_u - soft) / (Fraction(1) - soft))

    skew_bps = cfg_num(cfg["inventory"]["reservation_skew_bps_at_hard_limit"]) * u * boost
    reservation = fair * (Fraction(1) - skew_bps / BPS)

    result["fair_value"] = price_text(nearest_int(fair, "fair_value"))
    result["reservation_price"] = price_text(nearest_int(reservation, "reservation_price"))
    result["inventory_utilization"] = float(u)
    result["mid"] = price_text(nearest_int(mid, "mid"))
    result["microprice"] = price_text(nearest_int(microprice, "microprice"))

    state = "ACTIVE"
    if abs_u >= 1:
        state = "REDUCING_ONLY"
        reasons.append("INVENTORY_HARD_LIMIT")
    elif abs_u >= reducing:
        reasons.append("INVENTORY_REDUCING_BIAS")
    elif abs_u > 0 and abs_u >= soft:
        reasons.append("INVENTORY_SOFT_LIMIT")

    # ── spread ─────────────────────────────────────────────────────────────
    sp = cfg["spread"]
    fee_component = cfg_num(sp["fee_buffer_bps"]) + cfg_num(sp["maker_fee_multiplier"]) * maker_fee_bps
    raw_half_spread = (
        fee_component
        + cfg_num(sp["volatility_multiplier"]) * cfg_num(market.get("short_horizon_volatility_bps", 0.0))
        + cfg_num(sp["toxicity_multiplier"]) * cfg_num(market.get("toxicity_risk_bps", 0.0))
        + cfg_num(sp["latency_buffer_bps"])
        + cfg_num(sp["latency_multiplier"]) * cfg_num(market.get("latency_risk_bps", 0.0))
    )
    min_hs = cfg_num(sp["min_half_spread_bps"])
    max_hs = cfg_num(sp["max_half_spread_bps"])
    half_spread = clamp(max(min_hs, raw_half_spread), min_hs, max_hs)
    result["target_half_spread_bps"] = float(half_spread)

    # ── sizes ──────────────────────────────────────────────────────────────
    invcfg = cfg["inventory"]
    k = cfg_num(invcfg["size_skew_strength"])
    min_m = cfg_num(invcfg["min_size_multiplier"])
    max_m = cfg_num(invcfg["max_size_multiplier"])
    base = atoms(cfg["quotes"]["base_size"])

    bid_mult = clamp(Fraction(1) - k * u, min_m, max_m)
    ask_mult = clamp(Fraction(1) + k * u, min_m, max_m)
    bid_size = lot_floor(nearest_int(base * bid_mult, "bid_size"), lot)
    ask_size = lot_floor(nearest_int(base * ask_mult, "ask_size"), lot)

    if abs_u >= reducing:
        factor = cfg_num(invcfg["reducing_size_factor"])
        if u > 0:
            bid_size = lot_floor(nearest_int(Fraction(bid_size) * factor, "bid_size_reduced"), lot)
        elif u < 0:
            ask_size = lot_floor(nearest_int(Fraction(ask_size) * factor, "ask_size_reduced"), lot)

    # ── hard-limit headroom ────────────────────────────────────────────────
    worst_long = max(position, atoms(inv["worst_case_position_if_all_buys_fill"]))
    worst_short = min(position, atoms(inv["worst_case_position_if_all_sells_fill"]))
    buy_headroom = lot_floor(max(hard - worst_long, 0), lot)
    sell_headroom = lot_floor(max(worst_short + hard, 0), lot)
    allow_buy = buy_headroom > 0
    allow_sell = sell_headroom > 0

    if not allow_buy or not allow_sell:
        if "INVENTORY_HARD_LIMIT" not in reasons:
            reasons.append("INVENTORY_HARD_LIMIT")
        state = "REDUCING_ONLY"

    result["state"] = state
    result["bid_size"] = atom_text(bid_size if allow_buy else 0)
    result["ask_size"] = atom_text(ask_size if allow_sell else 0)

    if half_spread < fee_component:
        reasons.append("INSUFFICIENT_EDGE")
        result["reason_codes"] = reasons
        return result

    # ── ladder ─────────────────────────────────────────────────────────────
    post_only = bool(cfg["quotes"]["post_only"])
    spacing = cfg_num(cfg["quotes"]["level_spacing_bps"])
    quotes = []
    prev_bid = None
    prev_ask = None
    cross_prevented = False

    for level in range(int(cfg["levels"])):
        offset = half_spread + Fraction(level) * spacing
        bid_price = floor_to_tick(reservation * (Fraction(1) - offset / BPS), tick,
                                  f"bid_level_{level}")
        ask_price = ceil_to_tick(reservation * (Fraction(1) + offset / BPS), tick,
                                 f"ask_level_{level}")

        if prev_bid is not None:
            bid_price = min(bid_price, prev_bid - tick)
            ask_price = max(ask_price, prev_ask + tick)
        prev_bid, prev_ask = bid_price, ask_price

        if level == 0 and bid_price >= ask_price:
            quotes = []
            reasons.append("INSUFFICIENT_EDGE")
            break

        if allow_buy and bid_size > 0 and bid_price > 0 and buy_headroom > 0:
            if post_only and bid_price >= ask:
                cross_prevented = True
            else:
                q = lot_floor(min(bid_size, buy_headroom), lot)
                if q > 0 and (min_qty == 0 or q >= min_qty):
                    quotes.append({
                        "side": "BUY",
                        "price": price_text(bid_price),
                        "quantity": atom_text(q),
                        "level": level,
                        "post_only": post_only,
                    })
                    buy_headroom -= q

        if allow_sell and ask_size > 0 and sell_headroom > 0:
            if post_only and ask_price <= bid:
                cross_prevented = True
            else:
                q = lot_floor(min(ask_size, sell_headroom), lot)
                if q > 0 and (min_qty == 0 or q >= min_qty):
                    quotes.append({
                        "side": "SELL",
                        "price": price_text(ask_price),
                        "quantity": atom_text(q),
                        "level": level,
                        "post_only": post_only,
                    })
                    sell_headroom -= q

    if cross_prevented:
        reasons.append("POST_ONLY_CROSS_PREVENTED")

    if not reasons:
        reasons.append("NORMAL")

    result["quotes"] = quotes
    result["reason_codes"] = reasons
    return result


def snapshot_id(value) -> int:
    if isinstance(value, int):
        return value
    digits = ""
    for ch in reversed(str(value)):
        if ch.isdigit():
            digits = ch + digits
        else:
            break
    if not digits:
        raise FixtureError(f"snapshot_id {value!r} has no numeric suffix")
    return int(digits)


def merge(base: dict, override: dict) -> dict:
    out = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = merge(out[key], value)
        else:
            out[key] = value
    return out


def build() -> dict:
    base_cfg = json.loads(CONFIG_PATH.read_text())
    cases = json.loads(CASES_PATH.read_text())["cases"]

    results = []
    for case in cases:
        cfg = merge(base_cfg, case.get("config_overrides", {}))
        try:
            results.append(evaluate(case, cfg))
        except FixtureError as exc:
            raise FixtureError(f"case {case['name']!r}: {exc}") from None
    return {
        "generator": "tests/reference/mm_strategy_reference.py",
        "note": "regenerate with --write after an intentional semantic change",
        "cases": results,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true", help="write expected.json")
    group.add_argument("--check", action="store_true", help="verify expected.json is current")
    args = parser.parse_args()

    produced = build()
    text = json.dumps(produced, indent=2, sort_keys=True) + "\n"

    if args.write:
        EXPECTED_PATH.write_text(text)
        print(f"wrote {EXPECTED_PATH} ({len(produced['cases'])} cases)")
        return 0

    if not EXPECTED_PATH.exists():
        print(f"missing {EXPECTED_PATH}", file=sys.stderr)
        return 1
    if EXPECTED_PATH.read_text() != text:
        print(f"{EXPECTED_PATH} is stale; rerun with --write", file=sys.stderr)
        return 1
    print("reference expectations are current")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FixtureError as exc:
        print(f"fixture error: {exc}", file=sys.stderr)
        sys.exit(2)
