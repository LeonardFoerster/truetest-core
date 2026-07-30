# Bybit Futures Testnet / Demo Drill

**Status**: Active operator SOP for Bybit V5 linear USDT perpetuals (Phases 0–4).  
**Not** mainnet readiness. Mainnet requires human CCB + Phase 0 evidence culture.

**Last updated**: 2026-07-30

---

## Purpose

Rehearse Bybit wiring before any mainnet discussion:

- Env credentials (`TRUETEST_BYBIT_*`) — never argv secrets in shared shells
- Testnet vs Demo Trading (distinct hosts)
- One-way position mode (hedge refused at `open()`)
- Reconciler refuse on cash/position drift
- Kill-switch: cancel-all → reduceOnly MARKET flatten
- Local DMS (DCP is institutional-only; no silent pretend-DCP)
- Conditional SL/TP brackets (two legs, non-atomic)

---

## Environments

| Flag | REST | Public WS | Private WS |
|------|------|-----------|------------|
| `--testnet` | `api-testnet.bybit.com` | `stream-testnet.bybit.com` | same |
| `--demo` | `api-demo.bybit.com` | public: mainnet streams | `stream-demo.bybit.com` |
| (none) | mainnet — **attended + CCB only** | | |

Demo takes precedence over testnet in the provider factory.

---

## Credentials

```bash
export TRUETEST_BYBIT_API_KEY=...
export TRUETEST_BYBIT_API_SECRET=...
# Optional gated unit test:
export TRUETEST_BYBIT_TESTNET_LIVE=1
```

Prefer env over `--api-key` / `--api-secret` (argv is visible in `ps`).

---

## Build

```bash
cmake --preset linux-bybit
cmake --build --preset linux-bybit -j
# or: cmake -B build -DENABLE_BYBIT=ON -DBUILD_TESTS=ON && cmake --build build -j
```

---

## Shadow (MD only, no live orders)

```bash
./out/build/linux-bybit/engine_shadow \
  --provider bybit-futures --symbol BTCUSDT \
  --stream trade --depth-stream orderbook.50 \
  --testnet \
  --strategy sma --no-pin --status-format off --no-tui
```

Expect continuous ticks without crash for several minutes.

---

## Live testnet ritual (tiny, attended)

Use `engine_live` only. Tiny notional. Operator present.

```bash
./out/build/linux-bybit/engine_live \
  --provider bybit-futures --symbol BTCUSDT \
  --stream trade --testnet --live \
  --max-notional-usdt 50 --max-leverage 5 \
  --max-daily-loss 25 \
  --dead-man-countdown-ms 30000 \
  --no-pin --status-format off --no-tui
```

Checklist:

- [ ] Account is **one-way** (not hedge)
- [ ] Keys from env, not argv
- [ ] Place tiny limit far from market → private WS ack → cancel
- [ ] Kill path: process SIGINT → cancel-all + flatten (no leftover position)
- [ ] DMS: local heartbeat log; on SIGKILL orders are **not** venue-auto-cancelled (DCP institutional only) — operator closes manually if needed
- [ ] Reconciler: intentional cash drift fixture refuses start

Gated unit test (same keys):

```bash
TRUETEST_BYBIT_TESTNET_LIVE=1 ctest --test-dir out/build/linux-bybit \
  -R BybitFuturesTestnetLive --output-on-failure
```

---

## Safety notes (binding)

| Topic | Behavior |
|-------|----------|
| Halt | Write-once terminal; process restart only |
| Kill-switch | No retry loop; deadline-bounded |
| DMS | Local by default; logs DCP institutional caveat |
| Brackets | Two conditional Market legs; `oco=false`; partial fractions declined |
| Risk | `FuturesRiskCheck` from provider caps (`max_notional_usdt`, …) |

---

## Related

- Guide: `docs/upcoming_platform/bybit.md`
- Binance testnet drills: `docs/operations/02-futures-testnet.md`
- Bitget demo: `docs/operations/03-bitget-demo.md`
