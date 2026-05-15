# Binance spot testnet

The spot testnet (`testnet.binance.vision`) is a Binance-operated sandbox:
real-time market data, real signed REST/WebSocket protocols, and a demo
account funded with fake balances. The engine routes through the same
live order code path as production — it's the right tool for catching
auth, signing, OCO, listenKey, reconciliation, and rate-limit bugs
before any real money is at stake. It is the wrong tool for tuning
slippage, impact, or fill realism (see [Gotchas](#gotchas) below).

## What you need

- An `engine_live` binary. The live order path is gated at compile time
  by `TT_TARGET`; `engine_backtest` and `engine_shadow` will refuse
  `--mode live` even with `--testnet`.
- An API key + secret minted at <https://testnet.binance.vision>. Login
  is GitHub OAuth — no KYC, no real account. Same HMAC-SHA256 wire
  format as production; no code change needed for signing.

## Run it

```bash
export TRUETEST_BINANCE_API_KEY=...
export TRUETEST_BINANCE_API_SECRET=...

./engine_live \
    --provider binance \
    --symbol btcusdt \
    --stream trade \
    --mode live --live \
    --testnet
```

`--testnet` flips the provider's REST and WebSocket hosts to
`testnet.binance.vision` / `stream.testnet.binance.vision`. Without
`--mode live`, the same flag still routes market data through the
testnet feed — useful for shadow/paper runs against testnet data.

The "real money" confirmation prompt (`confirm_real_money_math`) is
auto-skipped when `--testnet` is set. The provider logs the resolved
endpoints at startup so you can see which sandbox you hit:

```
  BinanceProvider: [TESTNET] ws=stream.testnet.binance.vision:9443 rest=testnet.binance.vision:443
```

You can also drive testnet by setting `host=stream.testnet.binance.vision`
in `provider_config` directly — `looks_like_testnet_host()` flips the
endpoint set automatically.

## What the engine does on your behalf

- **Account-reset tolerance.** Testnet wipes balances and open orders
  ~monthly. The reconciler logs a `[TESTNET-RESET]` warning and lets
  startup proceed when the venue returns near-zero balances against
  non-trivial local state, instead of treating it as drift. Production
  still treats the same condition as a halt-worthy invariant violation.
- **WAF guard for client_order_id.** Binance's testnet rejects any
  request whose params contain SQL keywords (`OR`, `AND`, `SELECT`,
  `DROP`, `UNION`, `--`). The provider checks the minted prefix at
  open() and refuses to go live if it would trip the filter.
- **Symbol existence check.** Testnet's symbol set is a rotating subset
  of prod. Before opening the WebSocket the provider hits
  `GET /api/v3/exchangeInfo?symbol=...` once and fails fast on a typo,
  rather than letting it surface as `-1121 unknown symbol` mid-stream.
- **Engine-binary hint.** Running `--mode live --testnet` on
  `engine_shadow` or `engine_backtest` prints a build-and-run hint for
  `engine_live` rather than a generic gate error.

## Gotchas

- **Synthetic liquidity.** The book is fed by a simulated matching
  engine, not real prod flow. Fills behave unrealistically (often
  instant top-of-book regardless of size). Do **not** use testnet to
  calibrate `realistic_fills`, `impact_model`, `bar_spread_bps`, or any
  slippage assumption — those calibrations live in
  [`docs/realism.md`](realism.md) and want recorded prod tape.
- **Monthly account reset.** Treat every restart as if positions and
  orders may have vanished. The reconciler tolerates this; your
  strategy state may not.
- **Feature drift.** Testnet typically receives new endpoints (new OCO
  variants, new order types) weeks before production. The Binance
  changelog is the authoritative source. Code defensively against
  divergent rejection codes between the two.
- **listClientOrderId is still 32 chars.** Same as prod — strategy
  attribution can't round-trip through the OCO list id.
- **WebSocket disconnect cap.** 24h max, same as prod, plus more
  spurious disconnects in practice. Auto-reconnect is required, not
  optional.
- **No SAPI.** Wallet, savings, margin transfers — only the `/api`
  surface is mounted on testnet.

## Out of scope for this document

- **Futures testnet** — see the dedicated guide
  [`docs/futures-testnet.md`](futures-testnet.md). It uses a completely
  separate provider (`binance-futures`), different keys, different
  endpoints (`testnet.binancefuture.com` + `stream.binancefuture.com`),
  one-way mode requirement, dead-man's switch, position-based
  reconciliation, and `reduceOnly` close logic.
- **`/api/v3/orderList/oco` migration.** The deprecated
  `/api/v3/order/oco` path still works on both prod and testnet today.
  Migrating is tracked separately and lands with the OCO bracket
  adapter rather than as a testnet-only change.
