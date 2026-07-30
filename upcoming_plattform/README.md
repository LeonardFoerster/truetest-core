# Upcoming Platforms — Multi-Venue Provider Integration

**Status:** Research + Implementation Guides (Read-only design complete, code not started)  
**Date:** 2026-07-29  
**Audience:** Grok Build agents implementing new CEX providers for TrueTest `core/`

This folder contains **actionable build instructions** for integrating five additional centralized exchanges after the existing Binance Spot / Binance USDT-M Futures stack.

---

## Documents

| File | Venue | Registry names (planned) | Product focus (v1) |
|------|-------|--------------------------|--------------------|
| [bybit.md](./bybit.md) | **Bybit** | `bybit-futures`, `bybit` | Linear USDT perps (`BTCUSDT`) |
| [okx.md](./okx.md) | **OKX** | `okx-futures`, `okx` | USDT-M SWAP (`BTC-USDT-SWAP`) |
| [gateio.md](./gateio.md) | **Gate.io** | `gate-futures`, `gate` | USDT perps (`BTC_USDT`) |
| [bitget.md](./bitget.md) | **Bitget** | `bitget-futures`, `bitget` | UTA USDT-M (`BTCUSDT`) |
| [kraken.md](./kraken.md) | **Kraken Futures** | `kraken-futures`, `kraken` | Linear multi-collateral (`PF_XBTUSD`) |

Each guide was produced by a dedicated subagent that:

1. Studied the real engine architecture (`IProvider`, parsers, transports, Binance futures golden path).
2. Read official exchange API documentation.
3. Mapped venue APIs → TrueTest interfaces with phased delivery and DoD.

---

## Recommended implementation order

```
1. Bybit   — closest to Binance (symbols, V5 linear, testnet)
2. OKX     — best retail DMS (cancel-all-after) + deep liquidity + demo
3. Gate.io — countdown DMS + testnet + alt breadth
4. Bitget  — close-positions kill-switch gold; check DE/FR geo access first
5. Kraken  — regulated/EU fit; different auth (challenge + HMAC-SHA512)
```

Do **not** start all five in parallel on the same branch without isolation (worktrees).  
Shared scaffolding (HMAC helpers, depth-sync, CLI credential resolve) may be extracted after the first two venues land.

---

## Engine architecture (must understand before coding)

### Sole extension point

```
CLI/config → ProviderRegistry::create(name, provider_config)
                 │
                 ▼
            IProvider::open()
         ┌───────┴────────┐
         │                │
   IDataTransport    IExecutionAdapter
   (market WS)       (Hybrid | Shadow | ExecutionBridge)
         │                │
   IDataParser       REST + private user-data WS
         │                │
   DataBridge        poll_fills / async results
         └───────┬────────┘
                 ▼
              engine
    strategy → IRiskCheck → RiskManager → route
    startup: IReconciler | shutdown: IKillSwitch
    liveness: DMS / WorkerWatchdog → halt_flag_
```

### Four safety hooks (+ DMS)

| Hook | Interface | File |
|------|-----------|------|
| Reconciler | `IReconciler` | `src/execution/live_safety.h` |
| Kill-Switch | `IKillSwitch` | `src/execution/live_safety.h` |
| Risk Check | `IRiskCheck` | `src/risk/futures_risk_check.h` (reuse generic) |
| Brackets | `IBracketAdapter` | `src/exits/bracket_adapter.h` |
| DMS | venue-specific + `get_liveness_sources()` | pattern: `binance_futures_dead_mans_switch.h` |

### Golden reference (copy structure, not wire format)

```
src/providers/binance/
  binance_futures_provider.h      # composition root + open() mode dispatch
  binance_futures_register.cpp    # REGISTER_PROVIDER
  binance_parser.h                # hand-rolled JSON (no nlohmann)
  binance_combined_parser.h
  binance_combined_transport.h
  binance_futures_order_encoder.h
  binance_rest_order_transport.h
  binance_futures_user_data_parser.h
  binance_user_data_transport.h
  binance_futures_reconciler.h
  binance_futures_kill_switch.h
  binance_futures_dead_mans_switch.h
  binance_futures_bracket_adapter.h
  binance_auth.h
  binance_endpoints.h
```

### Non-negotiables (every venue)

| Rule | Meaning |
|------|---------|
| **R1/R6** | No `nlohmann/json` on hot path; hand `string_view` parsers |
| **S2** | Live orders only in `engine_live` (`TT_TARGET`) |
| **S3/S4** | Halt terminal; safety loud, non-retrying, fail-closed |
| **S9** | `HAS_<VENUE>` only under `src/providers/<venue>/` + CMake — never in `engine/`, `core/`, `threading/` |
| **Layer** | `./scripts/check-layer-deps.sh` must pass |
| **Freeze** | Do not casually edit Binance freeze files; new venue safety is separate |

After any `src/` edit:

```bash
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

---

## Shared CMake pattern (all venues)

```cmake
# CMakeLists.txt
option(ENABLE_<VENUE> "Build with <Venue> provider" OFF)

# cmake/Dependencies.cmake — inside tt_wire_optional_backends
if(ENABLE_<VENUE>)
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_sources(${target} PRIVATE
        ${_src}/providers/<venue>/<venue>_futures_register.cpp)
    target_link_libraries(${target} PUBLIC
        Boost::headers OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(${target} PUBLIC HAS_<VENUE>)
endif()
```

Tests go in `cmake/Sources.cmake` (explicit list, no GLOBs).

---

## Cross-venue comparison (safety-critical)

| Feature | Binance (done) | Bybit | OKX | Gate | Bitget | Kraken |
|---------|----------------|-------|-----|------|--------|--------|
| Symbol style | `BTCUSDT` | `BTCUSDT` | `BTC-USDT-SWAP` | `BTC_USDT` | `BTCUSDT` | `PF_XBTUSD` |
| Auth | HMAC-SHA256 hex query | HMAC-SHA256 headers | HMAC-SHA256 Base64 + passphrase | HMAC-SHA512 | HMAC-SHA256 Base64 + passphrase | SHA256+HMAC-SHA512 Authent |
| Private stream | listenKey | WS auth op | WS login | WS auth + user_id | WS login | Challenge-response |
| Cancel-all | ✓ | ✓ | batch / CAA | ✓ DELETE | ✓ | ✓ |
| Exchange DMS | countdown ms | DCP (often inst.) | **cancel-all-after** s | **countdown** s ≥5 | countdown s [5–60] | **cancelallordersafter** s |
| Flatten helper | reduceOnly MARKET | reduceOnly loop | **close-position** | reduceOnly signed size | **close-positions** | reduceOnly mkt |
| Testnet/Demo | ✓ | testnet+demo | demo | testnet | demo | demo |
| Port effort | — | lowest | low | medium | medium | high |

---

## Phase template (every venue guide follows this)

| Phase | Deliverable |
|-------|-------------|
| **0** | Scaffold + endpoints + auth unit tests + CMake + registry stub |
| **1** | Public market data WS + hand parsers + shadow/paper |
| **2** | REST orders + private fill stream + ExecutionBridge |
| **3** | Reconciler + kill-switch + DMS + risk caps |
| **4** | Brackets + demo/testnet ritual + ops note |

Never skip to live capital before Phase 3 unit tests and a demo/testnet kill+DMS drill.

---

## Operator / jurisdiction notes

- **Not legal advice.** Live derivatives access from EU/DE may require MiCA/MiFID-eligible entities and product gates.
- **Bitget:** DE/FR access restrictions reported — implement fully, but document operator precondition.
- **Market data multi-venue** is usually easier than **live execution** from a regulated EU account.
- Prefer env vars for secrets (`TRUETEST_<VENUE>_API_KEY`, …); never commit keys.

---

## How Grok Build should use these docs

1. Pick **one** venue file (e.g. `bybit.md`).
2. Open the Binance futures golden files listed above side-by-side.
3. Execute **Phase 0 only** first; stop at DoD checklist.
4. Run the three check scripts + focused `ctest -R <venue>`.
5. Proceed phase-by-phase; do not invent API fields — re-read official docs linked in each guide.
6. Commit thematically (provider scaffold ≠ safety ≠ docs).

### Suggested first commands (after Phase 0 for a venue)

```bash
cmake -B build -DENABLE_BYBIT=ON -DENABLE_BINANCE=ON -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build -R 'bybit|Bybit' --output-on-failure
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

---

## Out of scope for this folder

- Hyperliquid (DEX / wallet signing — separate architecture track)
- Coinbase Advanced (JWT + product fragmentation)
- MEXC / HTX (testnet / trust / DMS gaps)
- Spot multi-venue (futures-first; spot can follow per venue later)
- Actual C++ implementation (guides only)

---

## Source research

Guides were cross-checked against:

- TrueTest `core/` sources (2026-07)
- Official API docs: Bybit V5, OKX V5, Gate APIv4/Futures WS, Bitget UTA, Kraken Futures
- CMC/CoinGecko derivatives rankings (liquidity context only)

If an official doc and a guide disagree, **trust the live official docs** and update the guide.

---

*Last updated: 2026-07-29 — multi-agent research pack for Grok Build.*
