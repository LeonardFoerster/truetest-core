# Binance USDT-M futures testnet

The futures testnet (`testnet.binancefuture.com`) is a Binance-operated
sandbox for USDT-margined perpetuals. Real-time mark prices, real signed
`/fapi/v1/...` REST + `fstream`-style WebSocket protocols, and a demo
account funded with fake USDT. Same caveat as the spot testnet doc:
this catches **wire/protocol bugs** (auth, signing, listenKey, position
mode, reduce-only, position-close), not slippage / impact / fill realism.

This is a separate stack from spot. Different host, different signup,
different keys, different engine provider (`binance-futures`). It is not
a flag on the spot provider.

## What you need

- An `engine_live` binary built with `-DENABLE_BINANCE=ON`. Same compile-
  time gate as spot: `engine_backtest` / `engine_shadow` refuse
  `--mode live` even against testnet.
- An API key + secret minted at <https://testnet.binancefuture.com>.
  **Email signup**, not GitHub — the futures testnet portal is unrelated
  to the spot testnet portal even though the docs make them look like
  siblings. Different login, different keys.
- An account configured for **one-way mode**, not hedge mode. Margin
  type and leverage are operator concerns; this engine reads them but
  does not flip them on your behalf. See "Account setup" below.

## Account setup (one-time, in the Binance UI)

Do these in the testnet UI before the first run. The provider refuses
to start if any of them are wrong, but it won't fix them — fixing
silently is exactly the kind of helpful behaviour that has caused
production incidents elsewhere.

1. **Position mode → One-way.**
   *Settings → Position Mode → One-way.* The provider issues a signed
   `GET /fapi/v1/positionSide/dual` at startup; if the response says
   `"dualSidePosition":true`, it refuses with:
   ```
   BinanceFuturesProvider: refusing to go live — account is in hedge
   mode (dualSidePosition=true). Switch to one-way mode in the Binance UI.
   ```
   The encoder doesn't emit `positionSide`, and the engine's lot
   bookkeeping is built around netted positions. Hedge mode would
   require both, and v1 supports neither.

2. **Margin type per symbol.** Either ISOLATED or CROSSED works; pick
   one and don't change it mid-run. The reconciler reads it but doesn't
   warn yet (planned).

3. **Leverage per symbol.** Set conservatively for testnet too — high
   leverage on the testnet trains bad muscle memory. The provider does
   not auto-set leverage; what's in the UI is what's used.

## Run it

```bash
export TRUETEST_BINANCE_API_KEY=...
export TRUETEST_BINANCE_API_SECRET=...

./engine_live \
    --provider binance-futures \
    --symbol btcusdt \
    --stream kline_1m \
    --mode live --live \
    --testnet
```

`--testnet` selects `binance::usdm_testnet()`:
- WebSocket: `stream.binancefuture.com:9443`
- REST: `testnet.binancefuture.com:443`

Note: the testnet WebSocket host has no `testnet` substring in it. The
endpoint registry catches this on the `binancefuture` token instead, so
explicitly setting `host=stream.binancefuture.com` in `provider_config`
also flips the endpoint set automatically.

The engine logs the resolved endpoints at startup:

```
  BinanceFuturesProvider: [TESTNET] ws=stream.binancefuture.com:9443 rest=testnet.binancefuture.com:443
```

The math-captcha confirmation prompt is auto-skipped when `--testnet`
is set, same as spot.

### Optional advisory inputs

Two startup-time *advisories* (warnings, not refusals — startup proceeds
either way) sit alongside the refusal pipeline. They surface conditions
the engine cannot fix on its own.

```bash
./engine_live --provider binance-futures \
    --symbol btcusdt --stream kline_1m \
    --mode live --live --testnet \
    --margin-type ISOLATED \
    --liquidation-warn-pct 0.05
```

| Flag | Default | What it does |
|---|---|---|
| `--margin-type ISOLATED\|CROSSED` | empty (no check) | Reads `marginType` from `/fapi/v2/positionRisk` per open position. If venue says one thing and you said another, prints `[ADVISORY] BTCUSDT margin mode is CROSSED, operator configured ISOLATED`. Both spellings match (`isolated`/`ISOLATED`, `cross`/`CROSSED`). |
| `--margin-type-strict` | off | Escalates margin-mode mismatch from advisory (warning) to refusal — `open()` returns false instead of logging and proceeding. Has no effect unless `--margin-type` is also set. Use this when the margin-type choice is a deliberate config invariant, not a preference. |
| `--liquidation-warn-pct 0.05` | 0.05 (5%) | For each open position with a non-zero `liquidationPrice`, computes distance from `markPrice`. If smaller than the threshold, prints `[ADVISORY] BTCUSDT position is N.NN% from liquidation (mark=… liq=… threshold=5.00%)`. Set to 0 to disable. **Always advisory** — there's no `--strict` companion because liquidation distance shrinks under price action the operator can't fix by restarting. |

Both advisories are filtered to the provider's symbol — multi-symbol
cross-margin awareness is out of scope for v1. Both tolerate
`liquidationPrice == 0` and `markPrice == 0` silently (unfunded testnet
accounts and just-opened positions both legitimately surface zeros).

### Pre-trade risk caps

In addition to advisories (which run once at startup), the futures
provider can refuse individual orders that would exceed operator-set
notional, leverage, or projected liquidation-distance caps. These run
on every order, before the venue-agnostic `RiskManager`. All three
caps are independent — disabling all of them (the default) means the
engine queries `provider->get_risk_check()`, gets `nullptr`, and
applies no extra check.

```bash
./engine_live --provider binance-futures \
    --symbol btcusdt --stream kline_1m --mode live --live --testnet \
    --max-notional 5000 \
    --max-leverage 5 \
    --min-liq-distance-pct 0.10
```

| Flag | Default | What it does |
|---|---|---|
| `--max-notional <usdt>` | 0 (disabled) | Refuses if `\|post_qty\| × mark_price` exceeds the cap. Magnitude-based, so flipping long↔short is captured naturally. |
| `--max-leverage <multiplier>` | 0 (disabled) | Refuses if `post_notional / cash` exceeds the cap. Skipped cleanly when local cash is zero. |
| `--min-liq-distance-pct <fraction>` | 0 (disabled) | Refuses if the projected post-trade buffer to liquidation, expressed as a fraction of mark, is smaller than this. `0.05` = require ≥ 5% room. |

#### How the liquidation projection works

The buffer is approximated as `cash / post_notional − maintenance_margin`.
That's `1/leverage − mm`. At a maintenance margin of 0.5% (Binance
BTCUSDT first tier):

| Implied leverage | Projected buffer |
|---:|---:|
| 5× | 19.5% |
| 10× | 9.5% |
| 20× | 4.5% |
| 50× | 1.5% |
| 100× | 0.5% |

Setting `--min-liq-distance-pct 0.05` therefore caps effective
leverage around 18–19× under the default maintenance margin. Setting
it to `0.10` caps around 9–10×.

Three things to know before you tune this number:

1. **Cash is a coarse proxy for available margin.** Under cross-margin
   or multi-symbol setups, the venue's `availableBalance` may be very
   different from the engine's `portfolio.get_cash()`. The check uses
   the local figure. Conservative if local < venue, optimistic if
   higher. Operators with cross-margin should set caps tighter than
   the strictly-necessary venue limits.
2. **Maintenance margin is flat.** Binance USDT-M uses notional-tiered
   MM rates (BTCUSDT: 0.5% up to 1M USDT, then 0.65%, then …). This
   impl uses a flat default; raise via the `maintenance_margin_pct`
   provider config key for high-notional accounts where tiers bite.
3. **Pre-trade entry approximation.** We don't know the entry price of
   the new position, so the projection assumes entry ≈ mark. Small
   error for ack-fast venues (mark moves under microseconds, fill
   happens within seconds), but still an approximation: don't tune
   the threshold below `maintenance_margin + safety_factor`.

Refusals are pure rejections — they emit a `rejection_event` reason
`venue_risk_reject` and the engine continues. They are **not** halts:
the cap describes the operator's prudent-trading envelope, not a
market-wide risk-of-ruin condition that should stop everything. The
existing `--max-daily-loss` and `--risk-unwind` flags handle that
distinction.

## Refusal modes (what the messages mean)

The live `open()` runs a strict refusal pipeline. Each gate exists
because we have seen the failure mode in practice on spot or futures
mainnet — they are not paranoia for its own sake.

| Message | What it means | What to do |
|--------|---------------|------------|
| `refusing to go live — clock skew: drift N ms exceeds tolerance` | Local clock is more than 2 s off from `/fapi/v1/time` | Run `chronyd` / `systemd-timesyncd`; do not bypass |
| `refusing to go live — symbol 'X' not found on testnet exchangeInfo (HTTP 4xx)` | The symbol does not exist on testnet (rotating subset) or you typo'd it | Pick a symbol that's actually listed; the testnet Markets page is authoritative |
| `refusing to go live — /fapi/v1/positionSide/dual HTTP N` | API key lacks read permission, or signature failure | Check key permissions; check that you copied the secret correctly |
| `refusing to go live — account is in hedge mode (dualSidePosition=true)` | Account is not in one-way mode | Flip in UI per "Account setup" above |
| `ExecutionBridge open failed: ...` | listenKey creation or user-data WS handshake failed | Usually transient testnet flap; retry. If persistent, check key state |

## What the engine does on your behalf

- **Position-based reconciliation.** The startup reconciler reads
  `availableBalance` from `/fapi/v2/account` (top-level field, not the
  per-asset `assets[]` walk that spot uses) and signed `positionAmt`
  from `/fapi/v2/positionRisk?symbol=...`. Position drift is checked
  against your local `position.qty` (signed: long >0, short <0). No
  testnet-reset shortcut: futures testnet doesn't wipe on the same
  cadence as spot, so the spot heuristic would mask real drift. If the
  testnet wipes you out and you want to keep going, clear local
  checkpoint files explicitly.
- **Kill switch closes positions; it does not sweep balances.** On
  shutdown (or `--kill-switch` trigger) the engine cancels all open
  orders via `DELETE /fapi/v1/allOpenOrders`, then reads
  `/fapi/v2/positionRisk` and submits a `reduceOnly=true MARKET` on
  the opposite side sized to `|positionAmt|`. Long → SELL, short → BUY.
  `reduceOnly=true` is mandatory: it stops the close from accidentally
  opening an oversized opposite position if Binance partially
  liquidated us mid-shutdown.
- **Bracket adapter places SL+TP as two separate conditional orders**
  (`STOP_MARKET` + `TAKE_PROFIT_MARKET`) with `closePosition=true`
  and `reduceOnly=true`. There is no `/fapi/v1/order/oco` — placement
  is two non-atomic POSTs. The cancel-other-when-fires guarantee comes
  from `closePosition=true` semantics: when one leg triggers and the
  position becomes zero, Binance auto-cancels every other
  `closePosition=true` order on the symbol. Functionally equivalent
  to spot OCO, just with a small atomicity gap on placement that the
  adapter logs and the engine-side ExitManager covers in fallback.
- **Engine-binary hint.** Running `--mode live --testnet` on
  `engine_shadow` / `engine_backtest` prints a build-and-run hint for
  `engine_live` instead of a generic gate error.

## Gotchas

- **Liquidity is thin.** Testnet's matching engine has none of
  production's depth — quotes look reasonable, fills look weird. Do
  **not** calibrate slippage / spread / impact assumptions here.
  Production tape is the source of truth (see
  [`docs/realism.md`](realism.md)).
- **Funding rate is fictional.** Testnet computes a funding rate but
  it doesn't reflect real perpetual carry. Strategies that key off
  funding will mis-train on testnet.
- **Account state can persist longer than you'd like.** Testnet doesn't
  wipe with the same cadence as spot, but periodic resets do happen
  without warning. After any reset your local checkpoint is stale; clear
  it and re-anchor.
- **Liquidation behavior on testnet has been inconsistent.** Some
  accounts get liquidated normally, others see the position sit
  through what should be a margin call. The kill switch path assumes
  Binance will accept the reduceOnly close — if it doesn't, the
  shutdown bails with a stderr warning and operator intervention is
  required.
- **`marginRatio` may report 0 on unfunded testnet accounts** even with
  open positions. The liquidation-distance heuristic (planned) must
  tolerate this; for now the reconciler doesn't compute it.
- **`positionSide` is implicit.** In one-way mode every order Binance
  emits in execution reports has `"ps":"BOTH"`. The user-data parser
  doesn't read it, so this is informational only.
- **Bracket adapter declines partial-fraction intents** (`qty_fraction
  != 1.0`). `closePosition=true` always closes the entire position;
  there's no way to express "close 50% via venue-resting bracket"
  without falling back to `quantity` + `closePosition=false`, which
  loses the auto-cancel-other guarantee. v1 routes those intents
  through engine-side ExitManager only.

## Out of scope (for now)

- **COIN-M (inverse) futures** at `dapi.binance.com` /
  `dstream.binance.com`. Different contract math (settled in the base
  asset, not USDT). Will land as a sibling provider, not a flag.
- **Hedge mode.** The position-mode gate refuses it cleanly. Adding
  support means the encoder emits `positionSide=LONG/SHORT` and the
  engine grows a second per-symbol position bucket.
- **Cross-collateral / portfolio margin.** Account-level features that
  v1 doesn't model.
- **Position-based risk limits.** The engine's existing `RiskManager`
  is balance-based (cash). A futures-aware `notional / leverage /
  margin-call distance` check is the obvious next gap before relying
  on this path against real money. Don't flip mainnet on without it.

## Integration smoke test

`tests/test_binance_futures_testnet_live.cpp` exercises the full live
path end-to-end against the real testnet — clock resync, exchangeInfo
probe, position-mode readback, place + cancel a LIMIT BUY at half mark.
It's gated on `TRUETEST_FUTURES_TESTNET_KEY` / `TRUETEST_FUTURES_TESTNET_SECRET`
and silently no-ops without them, so the standard CI run stays clean.

```bash
TRUETEST_FUTURES_TESTNET_KEY=... TRUETEST_FUTURES_TESTNET_SECRET=... \
    ctest --test-dir build -R BinanceFuturesTestnetLive --output-on-failure
```
