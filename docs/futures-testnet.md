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

> **If you are trading USDT-M futures only**, this document + the realistic
> demo workflow in [`docs/demo-trading-workflow.md`](demo-trading-workflow.md)
> (with the futures adaptations) + [`docs/futures-testnet.md`](futures-testnet.md)
> form your complete pre-real-money path.

## Recommended pre-mainnet validation path for futures traders

For a clear picture of what actually happens to an order once it leaves your strategy,
read [`docs/futures-order-lifecycle.md`](futures-order-lifecycle.md).

1. **Backtest + realism** on recorded mainnet futures tape (see
   `demo-trading-workflow.md` "Futures example").
2. **Deterministic replay** of that tape.
3. **Live shadow** on mainnet futures feed (`--provider binance-futures` +
   `--depth-stream depth20@100ms`).
4. **Protocol + safety validation** on futures testnet (this document).
5. **Tiny-size mainnet Phase 0** (see `prod.md` Phase 0 section) with
   dead-man's switch armed, venue risk caps, daily loss limit, and the
   official Operator SOP (`docs/futures-phase0-operator-sop.md`).

Only after steps 1–3 show acceptable sim-vs-reality divergence should you
proceed to step 4 (testnet) and then step 5 (tiny mainnet Phase 0 under the SOP).

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

> **Heads up: the dead-man's switch is on by default.** Default countdown
> is 30 s; the engine refreshes it every 10 s by default. If the engine
> dies, hangs, or is suspended for longer than the countdown, Binance
> auto-cancels every open order on the symbol. Pass `--disarm-deadman`
> for a single run with the DMS off (debug, deploy, deliberate pause),
> or `--dead-man-countdown-ms 0` to disable. See "Dead-man's switch"
> below for tuning + foot-guns.

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

### Dead-man's switch

The dead-man's switch is the catastrophic-shutdown safety net. Where
the orderly kill-switch handles "engine exits cleanly via SIGINT or
risk-halt," the DMS handles "engine vanishes" — SIGKILL, OOM,
kernel panic, network gone. It posts to `/fapi/v1/countdownCancelAll`
on startup, and a heartbeat thread refreshes the timer periodically.
If the engine fails to refresh, Binance cancels every open order on
the symbol within `countdown_ms` of the last successful heartbeat.

```bash
./engine_live --provider binance-futures \
    --symbol btcusdt --stream kline_1m --mode live --live --testnet \
    --dead-man-countdown-ms 30000 \
    --dead-man-heartbeat-ms  10000
```

| Flag | Default | What it does |
|---|---|---|
| `--dead-man-countdown-ms <ms>` | 30000 | Server-side countdown. After this many ms without a heartbeat refresh, Binance cancels open orders. 0 disables. |
| `--dead-man-heartbeat-ms <ms>` | 0 (= countdown / 3) | How often the engine refreshes the timer. With the default ratio, two consecutive missed cycles trigger auto-cancel; one missed cycle is tolerated. |
| `--disarm-deadman` | off | Single-run kill switch for the DMS. Equivalent to `countdown_ms=0` but intent-revealing for debug / deploy workflows. |

The engine also runs an internal liveness watchdog (see
`src/threading/worker_watchdog.h`): if the heartbeat thread itself
hangs (stuck socket, deadlock, scheduler pause) for longer than
`3 × heartbeat_ms`, it sets `halt_flag_` so the orderly kill-switch
runs *before* the venue-side countdown fires mid-quote. Belt and
braces.

#### Foot-guns to internalize before relying on this

1. **The DMS does NOT close positions.** Only cancels orders. Open
   futures positions stay open after auto-cancel; an operator must
   close them out. The DMS is half a safety net; the kill-switch's
   flatten step is the other half.
2. **Process suspension (SIGSTOP) cancels your book.** If you `kill
   -STOP` the engine for debugging, the heartbeat thread can't run
   while paused → countdown fires → orders cancelled. Resume picks
   up an empty book. Pass `--disarm-deadman` before deliberate-pause
   workflows, or stop and restart cleanly.
3. **Network flaps inside the countdown window can trigger spurious
   cancels.** A 10s outage on a 30s countdown plus a heartbeat that
   was already 25s old at the start of the outage = countdown
   expires. This is why heartbeat-interval defaults to 1/3 the
   countdown, not 1/2 — gives slack for one missed cycle without
   losing the book.
4. **Heartbeat thread bugs are catastrophic in the wrong direction.**
   A heartbeat that silently hangs (stuck socket, mutex deadlock)
   while the engine is otherwise alive cancels orders mid-trade.
   The internal watchdog is the mitigation, but it depends on
   `halt_flag_` being honored quickly. Run the operator validation
   playbook (below) on every change to the heartbeat thread.

If the foot-guns above are net-negative for your use case, the
escape hatch is `--disarm-deadman` per run, or `--dead-man-countdown-ms
0` to keep the DMS disarmed across the run. Both are loud and
intent-revealing in operator dashboards / process listings.

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
probe, position-mode readback, place + cancel a LIMIT BUY at half mark,
listenKey lifecycle, bracket adapter round-trip, reconciler call, and
dead-man's-switch arm + heartbeat + disarm round-trip.
It's gated on `TRUETEST_FUTURES_TESTNET_KEY` / `TRUETEST_FUTURES_TESTNET_SECRET`
and silently no-ops without them, so the standard CI run stays clean.

```bash
TRUETEST_FUTURES_TESTNET_KEY=... TRUETEST_FUTURES_TESTNET_SECRET=... \
    ctest --test-dir build -R BinanceFuturesTestnetLive --output-on-failure
```

## Dead-man's-switch operator validation playbook

The smoke test above asserts the wire format is right. It does **not**
test the actual fail-safe behavior — that requires *killing the engine*
and watching what Binance does next. Run this playbook before turning
`--dead-man-countdown-ms` on by default, and re-run after any change
that touches the heartbeat thread or `BinanceFuturesProvider::open()/close()`.

### Setup (one-time)

1. **Funded testnet account** at <https://testnet.binancefuture.com>. Fund
   your USDT-M account from the in-UI faucet (not the spot testnet). One-way
   position mode set in the UI (the provider refuses to start in hedge mode).
2. **Engine build** with `-DENABLE_BINANCE=ON` — see the Build section above.
3. **Two terminals**: one for `engine_live`, one for monitoring Binance state
   via signed REST.
4. **Useful one-liners** (in the monitoring terminal):
   ```bash
   # Live snapshot of open orders for BTCUSDT
   alias bf-orders='./scripts/bf-signed-get /fapi/v1/openOrders symbol=BTCUSDT'

   # Live snapshot of position
   alias bf-position='./scripts/bf-signed-get /fapi/v2/positionRisk symbol=BTCUSDT'
   ```
   (If `scripts/bf-signed-get` doesn't exist, use `curl` against the testnet
   REST host with HMAC-SHA256 over the params; the `BinanceRestClient`
   header has the canonical signing flow if you need a reference.)

### Run command (the engine half)

Use **conservative caps** so accidental fills don't burn the testnet balance:

```bash
export TRUETEST_BINANCE_API_KEY=...
export TRUETEST_BINANCE_API_SECRET=...

./build/engine_live \
    --provider binance-futures \
    --symbol btcusdt --stream kline_1m \
    --testnet --live \
    --strategy sma --sma-period 14 \
    --balance 100 \
    --max-notional 50 --max-leverage 3 --min-liq-distance-pct 0.10 \
    --margin-type ISOLATED \
    --dead-man-countdown-ms 30000 \
    --dead-man-heartbeat-ms 10000 \
    --status-format ndjson
```

You should see startup logs ending with:
```
  BinanceFuturesProvider: dead-man's switch armed (countdown=30000ms, heartbeat=10000ms)
```

If that line is missing, the DMS isn't actually running — abort the test.

### Scenario A — clean shutdown (Ctrl-C)

**Goal:** verify the orderly path doesn't leak an armed timer.

| Step | Action | Expected |
|---|---|---|
| 1 | Run the engine command above. Wait for SMA to emit at least one resting order (~30-60s). | `bf-orders` shows at least one open order for BTCUSDT. |
| 2 | Send SIGINT (Ctrl-C in the engine terminal). | Engine prints kill-switch progress, then `dead-man's-switch disarm` confirmation, then exits. |
| 3 | Within 5s of exit, run `bf-orders`. | Returns `[]` — orders cancelled by kill-switch. |
| 4 | Wait 35s. Run `bf-orders` again. | Still `[]`. The disarm worked; no late server-side cancel. |
| 5 | Re-launch the engine immediately after exit. | Provider arms a fresh DMS without "already-set countdown" complaint. |

**Pass criteria:** all five rows green. **Fail signature:** late cancellation
at step 4 (~30s after exit) means the disarm POST didn't land.

### Scenario B — SIGKILL (catastrophic)

**Goal:** verify the DMS auto-cancels when the engine can't run its kill-switch.

| Step | Action | Expected |
|---|---|---|
| 1 | Run the engine. Wait for resting orders. | `bf-orders` non-empty. |
| 2 | In another terminal: `kill -9 <engine-pid>`. | Engine vanishes immediately. No kill-switch logs. |
| 3 | Note wall-clock time **T** of the kill. | — |
| 4 | Run `bf-orders` every 2s starting at T+25s. | Orders **still open** until ≈ T + (30s − age_of_last_heartbeat). |
| 5 | After T+45s at the latest, `bf-orders` returns `[]`. | Server-side countdown fired; orders cancelled by Binance. |
| 6 | Run `bf-position`. | If the strategy filled a position before the kill, position is **still open**. Manual close required (DMS does not flatten). |

**Pass criteria:** rows 4 and 5 align with the countdown window. **Fail
signature:** orders never cancel → DMS arm didn't actually take effect on
the venue side, OR heartbeat was racing the countdown such that the venue
view of "last refresh" was older than expected.

### Scenario C — OOM-killed simulation

**Goal:** same as B but reachable via mainstream test-harness tooling.

| Step | Action | Expected |
|---|---|---|
| 1 | Run the engine. Wait for resting orders. | `bf-orders` non-empty. |
| 2 | `kill -9 <pid>` (POSIX equivalent of OOM-kill from a verification standpoint — both deliver SIGKILL with no opportunity to clean up). | Engine vanishes. |
| 3 | Same as Scenario B from step 4 onward. | Same results. |

If on Linux and you want a higher-fidelity OOM repro, run under `systemd-run
--scope -p MemoryMax=200M` and let an allocation push it over. The kill
signal from OOM is identical to `kill -9` for our purposes; the value of
this scenario is mostly that operators don't all run with the same shell.

### Scenario D — network unplug

**Goal:** verify the DMS protects against silent network failure.

| Step | Action | Expected |
|---|---|---|
| 1 | Run the engine. Wait for resting orders. | `bf-orders` non-empty. |
| 2 | Block all egress to `testnet.binancefuture.com` (`sudo iptables -A OUTPUT -d testnet.binancefuture.com -j DROP`, or unplug ethernet, or `nmcli device disconnect`). | Engine's heartbeat starts failing. |
| 3 | Wait. Within `3 × heartbeat_ms` (= 30s default) the WorkerWatchdog should fire `halt_flag_`, and the engine should attempt orderly shutdown. The kill-switch will also fail (network is down), so eventually the engine exits with a stderr WARNING. | `[ADVISORY]` then watchdog stderr line then kill-switch WARNING then exit. |
| 4 | Restore network. | — |
| 5 | Run `bf-orders` from the monitoring terminal. | At ≈ countdown_ms after the last successful heartbeat, orders should be `[]`. |

**Pass criteria:** rows 3 and 5 both fire. **Fail signature:** engine
hangs forever (watchdog didn't fire) → step 1's framework regressed.

### Scenario E — process suspension (SIGSTOP)

**Goal:** verify the foot-gun documented above (suspended-process false
positive) actually behaves as documented, so operators are warned.

| Step | Action | Expected |
|---|---|---|
| 1 | Run the engine. Wait for resting orders. | `bf-orders` non-empty. |
| 2 | `kill -STOP <pid>`. | Engine pauses entirely. Heartbeat thread can't run (process is suspended). |
| 3 | Wait `countdown_ms` + 5s (35s default). Run `bf-orders`. | `[]` — Binance auto-cancelled while we slept. |
| 4 | `kill -CONT <pid>`. | Engine resumes. Heartbeat tries to refresh, gets a 2xx, but the previous countdown already fired → the orders are gone. |
| 5 | Engine state vs venue state: engine still thinks the orders are resting. Reconciler at next startup would catch this. | Live reconciliation gap until next restart. |

**Pass criteria:** matches the documented foot-gun. **Action item if
unexpected:** if the cancel doesn't fire at step 3, the heartbeat is
somehow still running while suspended — investigate. If it fires earlier
than the countdown, the countdown wasn't what we set.

### What to record

For each run, capture:
- **Scenario** (A–E)
- **Engine version** (commit SHA)
- **Timestamps**: T_start, T_kill, T_first_observe_cancel, T_full_cancel
- **Observed countdown window**: `T_full_cancel - T_kill` should be `≤ countdown_ms`
- **Anomalies**: anything not in the "Expected" column
- **Position state** at the end (DMS doesn't close positions)

A single completed pass of all five scenarios constitutes a green
validation run. Re-run on every change to the heartbeat thread or
the provider lifecycle. Don't flip the default ON until at least one
clean week of green runs in production hours (when network conditions
are most realistic).

### What this playbook does NOT cover

- **Mainnet validation.** Same procedure, real money. Do not run until
  testnet is solidly green and an operator-ratified change-control review
  has signed off.
- **Multi-symbol DMS.** The current impl arms one countdown per provider
  (one symbol). Multi-symbol setups would arm multiple, and the failure
  modes interact. Out of scope until multi-symbol providers exist.
- **DMS race with the orderly kill-switch.** Theoretical: kill-switch
  starts, watchdog fires (because heartbeat thread is mid-stop), halt_flag_
  set again, no harm done. Worth verifying once but not per-release.
