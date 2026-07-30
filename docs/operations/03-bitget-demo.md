# Bitget Futures Demo Drill & Operator Preconditions

**Status**: Active operator SOP for Bitget UTA futures (Phases 0–4). Demo drills only — **not** mainnet readiness.

**Authoritative gates**: `docs/governance/01-prod.md`, root/core `AGENTS.md` safety red lines, plan `upcoming_plattform/bitget.md` §12–§14.

**Last updated**: 2026-07-30 (Phase 4: backfill, brackets, advisories, funding account channel).

---

## Purpose

Rehearse Bitget UTA wiring before any human CCB discussion of mainnet:

- Credential / passphrase / env routing (`TRUETEST_BITGET_*`).
- Demo (`--demo` / paptrading) place-cancel path.
- Reconciler refusal on cash/position drift.
- Kill-switch cancel → close-positions.
- Dead-man's switch arm / heartbeat / disarm (account-wide).
- One-way position mode gate (hedge refused at open).

This document is an **operator precondition + demo drill**. It does **not** claim mainnet readiness. Mainnet requires human CCB, Phase 0 evidence culture, and governance ritual — agents must not self-declare mainnet go-live.

---

## Geo precondition (binding)

Bitget restricts or blocks accounts / access in certain jurisdictions (notably **DE** and **FR** among others). TrueTest **does not** perform Geo-IP or KYC checks.

| Rule | Detail |
|------|--------|
| Operator responsibility | You must be legally allowed to use Bitget in your jurisdiction. |
| Engine behavior | No geo gate in code; connectivity alone is not legal clearance. |
| Restricted accounts | Do not operate with DE/FR-restricted accounts against this provider. |
| SOP block | If jurisdiction is unclear or restricted → **do not run** live/demo with real credentials. |

Checklist:

- [ ] Operator jurisdiction allows Bitget use (NOT DE/FR restricted accounts)
- [ ] Legal/compliance self-check completed before any API key is loaded
- [ ] Demo key for drills; mainnet keys never in argv if avoidable (prefer env)

---

## Dual surface (classic vs UTA)

| Surface | Support in this provider | Notes |
|---------|--------------------------|-------|
| **UTA v3** (default) | Full Phase 0–4 path | Public WS + private WS + REST trade + DMS + brackets + backfill |
| Classic mix/v2 | **Refused** | No classic countdown DMS; `api_surface=classic` fails open |

Registry names (both map to the same UTA factory):

- `bitget-futures` (canonical)
- `bitget` (alias)

Build:

```bash
cmake -B build -DENABLE_BITGET=ON -DENABLE_BINANCE=ON -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)" --target engine_backtest engine_shadow engine_live
```

---

## Credentials & env

Prefer environment variables over CLI secrets:

```bash
export TRUETEST_BITGET_API_KEY=...
export TRUETEST_BITGET_API_SECRET=...
export TRUETEST_BITGET_API_PASSPHRASE=...
```

| Item | Requirement |
|------|-------------|
| Passphrase | Required for Bitget private REST/WS (third secret). |
| Demo keys | Use **demo** API keys with `--demo`. Mainnet keys on `wspap` fail. |
| CLI | `--api-key` / `--api-secret` / `--api-passphrase` work but emit argv warnings. |
| Sandbox flags | `--demo` or `--testnet` → Bitget demo/paptrading (REST header `paptrading:1`, WS `wspap`). |

---

## One-way position mode

- Account must be in **one-way** (not hedge / dual long-short).
- Provider refuses live open if hold mode is hedge or missing.
- Confirm one-way in the Bitget UI **before** demo live.

- [ ] One-way position mode confirmed in UI
- [ ] Hedge mode never used with this provider

---

## Shadow (public only, no keys)

Smoke connectivity against mainnet public WS (no orders, no credentials):

```bash
./build/engine_shadow \
  --provider bitget-futures \
  --symbol BTCUSDT \
  --stream trade \
  --depth-stream books5 \
  --no-pin --status-format off --no-tui
```

Expect `BitgetFuturesProvider: ws=ws.bitget.com:443/v3/ws/public` and eventual `[LIVE] events>0`.

---

## Demo live drill (tiny size)

Use `engine_live` only when intentionally testing signed demo order paths. Tiny notional, attended, demo keys only.

**Phase-0 CLI gates (parity with Binance futures):** mainnet Bitget live **refuses** without at least one of `--max-notional` / `--max-leverage` / `--min-liq-distance-pct` **and** a positive `--max-daily-loss`. `--demo` / `--testnet` (paptrading) may warn only.

```bash
export TRUETEST_BITGET_API_KEY=...
export TRUETEST_BITGET_API_SECRET=...
export TRUETEST_BITGET_API_PASSPHRASE=...

./build/engine_live \
  --provider bitget-futures \
  --symbol BTCUSDT \
  --stream trade \
  --depth-stream books5 \
  --demo \
  --live \
  --api-key "$TRUETEST_BITGET_API_KEY" \
  --api-secret "$TRUETEST_BITGET_API_SECRET" \
  --api-passphrase "$TRUETEST_BITGET_API_PASSPHRASE" \
  --reconcile-tolerance-bps 5 \
  --dead-man-countdown-ms 30000 \
  --dead-man-heartbeat-ms 8000 \
  --max-notional 50 --max-leverage 2 --min-liq-distance-pct 7 \
  --max-daily-loss 5
```

Suggested drill sequence:

| Step | Action | Pass criteria |
|------|--------|---------------|
| 1. Open refuse path | Start without one-way / with hedge | Loud refuse; no silent open |
| 2. Clean open | Correct demo keys + one-way + DMS | Reconciler match; DMS arm log; HB advances |
| 3. Place / cancel | Tiny limit away from market; cancel | Ack + cancel events; no leftover open order |
| 4. Reconciler | Drift cash/pos (if injectable) or mismatch caps | Fail-closed refuse or halt; no silent continue |
| 5. Kill-switch | SIGINT / orderly shutdown | cancel-symbol-order then close-positions; no unexpected leftover |
| 6. DMS venue | `kill -9` or network block after arm | Venue cancels **all UTA open orders** after countdown; positions reviewed manually |

Network demo live tests remain **opt-in** (env-gated unit suite); CI does not place demo orders by default.

---

## DMS BD enablement & account-wide cancel

### BD enablement

Bitget may require **BD (business development) enablement** before `POST /api/v3/trade/countdown-cancel-all` works. Permission-style failures refuse live open and log a BD enablement hint.

- [ ] DMS BD enablement confirmed with Bitget if countdown is required
- [ ] If DMS cannot arm → do not run live; fix permissions first

### Account-wide cancel caveat

Unlike Binance futures countdown (symbol-scoped), Bitget UTA **countdown-cancel-all is account-wide**: when the timer expires it cancels **all open UTA orders on the account**, not only the provider symbol.

Implications:

- v1 assumes a **single-symbol process**.
- Multi-strategy / multi-symbol operators must treat DMS expiry as process- and account-wide.
- Positions are **not** automatically flattened by venue countdown alone (orders only), unless `dms_attempt_position_close` / close-positions path runs while the process is still alive.

Countdown bounds (engine): operator ms → seconds clamped **[5, 60]**; heartbeat floor **1000 ms** (venue 1/s rate limit). Heartbeat is always kept **strictly below** clamped countdown.

---

## Dual-channel fills (order vs fill WS)

Bitget private WS uses separate **order** and **fill** channels:

| Channel | Role in TrueTest |
|---------|------------------|
| `order` | Lifecycle (ack / cancel / reject). Status `filled` / `partially_filled` with no last-fill qty is demoted so the bridge does **not** invent fills. |
| `fill` / `fast-fill` | Source of truth for `last_fill_qty` / price. Bridge untracks when cumulative fill qty covers the order. |

**Residual (ops):** if the fill channel is silent while the order channel reports filled, the engine will not invent inventory from order status alone (anti-double-count). Mitigations:

- Private WS disconnect with halt callback → process halt (after engine wires halt_cb).
- DMS cancels **open orders** account-wide when countdown expires; positions need kill-switch / `dms_attempt_position_close` / manual review.
- Operator: after any disconnect or ambiguous fill state, reconcile positions in the Bitget UI and do not assume engine lots match venue until a clean re-open/reconcile.

- [ ] After demo drills with kills/disconnects, confirm venue positions flat or expected
- [ ] Do not restart live on mainnet with unknown residual inventory

---

## Kill-switch drill (demo before mainnet)

Always complete a kill-switch drill on **demo** before any mainnet discussion:

1. Arm demo live with tiny risk caps.
2. Place a resting order (or hold a flat session with open path exercised).
3. Trigger orderly kill (SIGINT / engine halt path).
4. Verify: cancel-symbol-order succeeded (or documented noop), close-positions succeeded (or empty-position noop), no unexpected open orders.

| Scenario | Action | Expected |
|----------|--------|----------|
| A. Clean shutdown | SIGINT | Kill-switch cancel+flatten; DMS disarm |
| B. SIGKILL | `kill -9` | Venue DMS cancels all UTA open orders after countdown |
| C. Network loss | Block egress | Loud halt / HB fail path; venue timer still protects orders |
| D. Reconciler refuse | Cash/pos drift | Startup refuse; no silent open |

---

## Pre-flight checklist (demo)

- [ ] Geo/legal precondition satisfied (not DE/FR restricted)
- [ ] `ENABLE_BITGET=ON` binaries built (`engine_shadow`, `engine_live`)
- [ ] Demo API key + secret + passphrase in `TRUETEST_BITGET_*`
- [ ] One-way mode in UI
- [ ] DMS BD enablement if countdown used
- [ ] Tiny caps (`--max-notional` etc.)
- [ ] `--demo` set (never mix mainnet keys with wspap)
- [ ] Kill-switch + DMS plan written before first order
- [ ] **No mainnet readiness claim** without human CCB

---

## Phase 4 capabilities (ops)

| Feature | How to use | Notes |
|---------|------------|-------|
| **Kline backfill** | `--stream kline1m --backfill 500` | REST `/api/v3/market/candles` → PrependTransport; intervals normalized (`4h`→`4H`) |
| **Position advisories** | `--margin-type crossed` / `--liquidation-warn-pct 0.05` | Startup `[ADVISORY]` logs; strict margin still via settings gate |
| **Venue brackets** | Engine ExitManager + `get_bracket_adapter()` | UTA `place-strategy-order` tpsl full; partial fractions decline |
| **Funding** | Private WS `account` topic | Logs `[FUNDING]` / publishes `funding_event` when balance delta present |
| **Classic surface** | Do not set `api_surface=classic` | Open refuses; UTA only |

Backfill smoke (public REST, no keys):

```bash
./build/engine_shadow \
  --provider bitget-futures \
  --symbol BTCUSDT \
  --stream kline1m \
  --backfill 100 \
  --no-pin --status-format off --no-tui
```

Expect `backfill loaded N bars` then live kline stream. First open candle after backfill may be held by closed-bar gate until the next interval starts.

---

## Mainnet readiness (explicit non-claim)

| Statement | Status |
|-----------|--------|
| Phases 0–4 code + unit tests + gate scripts | Implementation complete when tests green |
| Demo drills documented | This SOP |
| Mainnet tiny-size live | **Not authorized by this doc** |
| Human CCB / Phase 0 evidence | Required before any mainnet capital |

Do **not** treat a green CI build or a successful demo session as mainnet go-live.

---

## Recording

Store demo drill notes under `reports/` (or a dated ops subdirectory). Include:

- Command line and run tag (if `--persist`).
- Open-order / position snapshots before and after each drill.
- DMS arm/disarm and kill-switch log excerpts.
- Mandatory grep:

```bash
grep -i "POSITION-SNAPSHOT\|funding\|drift\|reconcile\|halt\|kill\|DMS\|Bitget" <log> | tail -40
```

---

## Related

- `docs/operations/01-futures-phase0-operator-sop.md` — Binance Phase 0 printable SOP
- `docs/operations/02-futures-testnet.md` — Binance testnet drills
- `upcoming_plattform/bitget.md` — implementation plan (§12 examples, §14 DoD)
- `src/providers/bitget/` — provider surface (UTA only)
