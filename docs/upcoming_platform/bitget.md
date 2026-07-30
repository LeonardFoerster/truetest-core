# Bitget Futures Provider — Grok Build Implementation Guide

**Ziel-Datei:** `docs/upcoming_platform/bitget.md`  
**Workspace:** `/home/leonard/work/projects/truetest/core`  
**Golden Path (Vorbild):** `src/providers/binance/*` — insbesondere `binance_futures_*`  
**Stand API-Recherche:** 2026-07 (UTA v3 primär; Classic mix/v2 als Fallback-Oberfläche dokumentiert)

### Status (2026-07-30) — implemented

| Surface | Location |
|---------|----------|
| Code | `src/providers/bitget/` (UTA v3 USDT-M; classic refused) |
| Build | `-DENABLE_BITGET=ON`, presets `linux-bitget`, `linux-providers-questdb` |
| Freeze | `bitget_futures_{provider,dead_mans_switch,kill_switch,reconciler}.h` on Phase 1 freeze list |
| Ops SOP | [`docs/operations/03-bitget-demo.md`](../operations/03-bitget-demo.md) (demo drills; **not** mainnet authorization) |
| CLI | `--provider bitget\|bitget-futures`, `--demo`, `--api-passphrase`, env `TRUETEST_BITGET_*` |

This file remains a **design / DoD / API map** reference. Prefer code + the ops SOP for current behaviour. Unchecked checkboxes below are historical scaffold language unless re-opened as follow-ups. **Mainnet capital** still requires human CCB + Phase 0 evidence (`docs/governance/01-prod.md`).

---

## 1. Ziel

Einen **Bitget USDT-M Futures Provider** in die TrueTest-C++-Engine einhängen, der denselben Contract erfüllt wie `binance-futures`:

| Capability | Interface | Wann |
|---|---|---|
| Marktdaten (Trades, optional L2) | `IDataTransport` + Parser → `provider::event` / Tick/Bar | immer |
| Paper/Shadow-Execution | `HybridExecutor` / `TradeTapeShadowAdapter` | backtest/paper/shadow |
| Live-Orders | `ExecutionBridge` + `IOrderTransport` + `IOrderEncoder` + `IFillTransport` + `IFillParser` | nur `engine_live` + Keys |
| Startup-Gate Cash/Position | `IReconciler` | live |
| Not-Aus Cancel + Flatten | `IKillSwitch` | live |
| Dead-Man’s Switch | Venue countdown + Heartbeat-Thread + Watchdog-Liveness | live, opt-in |
| Pre-Trade Caps | `IRiskCheck` → wiederverwenden `FuturesRiskCheck` | shadow/live wenn Caps > 0 |
| Brackets (später) | `IBracketAdapter` | Phase 4 optional |

**Primäre API-Oberfläche: UTA v3** (`/api/v3/...`, WS `wss://ws.bitget.com/v3/ws/...`).  
Classic mix/v2 bleibt als **Dual-Surface-Notiz** und optionaler Fallback, nicht als Default.

**Provider-Namen (Registry):**
- `"bitget-futures"` — kanonisch (wie `"binance-futures"`)
- `"bitget"` — Alias, der auf denselben Factory zeigt (Spot ist **nicht** Scope v1)

---

## 2. Non-Negotiables (AGENTS.md + Safety)

Agents müssen diese Regeln **wörtlich** einhalten. Verstöße = Reject, kein Merge.

### 2.1 Architektur

1. **Provider ist der einzige Venue-Extension-Point.** Kein `HAS_BITGET` / Bitget-Header in `engine/`, `risk/`, `threading/`, `core/`.  
2. **Layer graph:** `providers` darf `core types utils data orderbook execution engine exits risk simulation threading ui` — nichts erfinden, was `check-layer-deps.sh` bricht.  
3. **Compile-time Live-Gate:** `TT_TARGET` / `target_allows_live_orders()` bleibt unangetastet. Bitget ändert **nicht** `tt_target.h`.  
4. **Quellen:** keine GLOBs. Jede `.cpp` in `cmake/Dependencies.cmake` (via `ENABLE_BITGET`) bzw. Tests in `cmake/Sources.cmake`.  
5. **Header-only wo möglich** (wie Binance: Encoder, Kill-Switch, Reconciler, DMS sind oft header-only; nur Register-`.cpp` + ggf. Transport-`.cpp` als TUs).

### 2.2 Hot Path (R1–R10)

- Parser: **kein** `nlohmann::json` auf dem WS-Read-Pfad. Needle-Scan wie `binance_parser.h` (`find_key` / `extract_sv_*`).
- Kein Heap pro Tick außer dem, was der bestehende Pool/Bridge-Pfad ohnehin macht.
- Order-Encoder: Prefixe cachen, `snprintf` für Zahlen, JSON-Body vorallokieren.
- Kein sync Logging im tight loop; Advisory/Startup-Logs auf `stderr` sind cold path.

### 2.3 Safety (S1–S10)

- Halt ist **write-once terminal** — kein Auto-Resume.
- Kill / DMS / Reconciler: **laut, non-retrying, fail-closed**.
- Reconciler **default-refuse** bei Drift/HTTP-Fehler/malformed.
- DMS-Countdown **nicht** adaptiv verlängern unter Load.
- Kill-Switch, DMS und Freeze **nicht** zu einem vagen „cancel everything“ vermischen.
- `FuturesRiskCheck` vor `RiskManager` bleibt Engine-Verantwortung; Provider liefert nur `get_risk_check()`.
- Bitget Safety-Header (`bitget_futures_{provider,dead_mans_switch,kill_switch,reconciler}.h`) sind **auf der Freeze-Liste** (14 Dateien gesamt). Edits brauchen `LIVE_SAFETY_CCB_APPROVED` + CCB + Shadow-Evidence (`/safety`).

### 2.4 Geo / Operator-Precondition (kein Skip der Implementation)

Bitget listet **DE und FR** (u. a.) als Prohibited/Restricted Jurisdictions; API-Zugriff ist Teil der Platform-Services. Stand 2026:

- Neue Signups DE/FR blockiert (ab Jan 2026).
- FR: Service-Einstellung in Phasen (März 2026); Trading/API für betroffene Accounts deaktiviert.
- DE: Terms-Prohibited; bestehende Accounts unsicher/änderbar; IP/KYC-Enforcement möglich.
- VPN-Umgehung ist ToS-widrig und **kein** Engine-Feature.

**Für Grok Build:** Implementation **vollständig** bauen. In Docs/SOP als **Operator-Precondition** dokumentieren:

> Operator muss rechtlich und netzwerkseitig berechtigt sein, Bitget zu nutzen. Engine prüft **keine** Geo-IP. Failures aus Geo-Block (HTTP 403/empty TLS/account restricted) → laut fail, kein Silent-Retry-Loop.

---

## 3. Architecture Map (1:1 zu Binance Futures)

```
CLI / main.inc
  --provider bitget-futures | bitget
  --symbol BTCUSDT
  --stream trade | kline1m | ...
  --depth-stream books5 | books1 | ...
  --api-key / --api-secret / --api-passphrase   (+ env TRUETEST_BITGET_*)
  --demo / --testnet  → paptrading / wspap
       │
       ▼
ProviderRegistry::create("bitget-futures", provider_config)
       │
       ▼
BitgetFuturesProvider : IProvider
  open() mode-dispatch:
    ┌─ transport: BitgetWsTransport / BitgetCombinedTransport
    │    public: wss://ws.bitget.com/v3/ws/public
    │    demo:   wss://wspap.bitget.com/v3/ws/public
    ├─ parser:   BitgetTradeParser / BitgetCombinedParser
    ├─ backfill: BitgetBackfill (REST candles) → PrependTransport
    │
    ├─ paper/backtest: HybridExecutor (local book + fee/fill models)
    ├─ shadow:         TradeTapeShadowAdapter
    └─ live (+ keys):
         BitgetRestClient (HMAC + passphrase headers)
         clock skew + instruments probe + position mode gate
         BitgetFuturesReconciler
         BitgetFuturesKillSwitch   ← close-positions Vorteil
         BitgetFuturesDeadMansSwitch  ← countdown-cancel-all (UTA, enablement)
         ExecutionBridge {
           order_tx: BitgetRestOrderTransport  (POST JSON)
           fills_tx: BitgetPrivateWsTransport  (login + order/fill/position)
           encoder:  BitgetFuturesOrderEncoder
           parser:   BitgetFuturesUserDataParser
           rate_limiter, client_id_fn, position_snapshot_handler
         }
         FuturesRiskCheck (reuse)
         optional bracket adapter (Phase 4)
```

### 3.1 Interfaces (nicht neu erfinden)

| TrueTest | Datei | Bitget-Impl |
|---|---|---|
| `IProvider` | `src/providers/provider.h` | `BitgetFuturesProvider` |
| `IDataTransport` | `src/providers/transport.h` | Public WS |
| `IDataParser<T>` | `src/providers/parser.h` | Trade/L2/Combined |
| `IExecutionAdapter` | `execution_adapter.h` | Bridge / Hybrid / Shadow |
| `IOrderEncoder` | `execution/order_encoder.h` | JSON place/cancel |
| `IOrderTransport` | `execution/order_transport.h` | REST POST |
| `IFillTransport` | `execution/fill_transport.h` | Private WS |
| `IFillParser` | `execution/fill_parser.h` | order/fill/position → `parsed_exec` / snapshot |
| `IReconciler` / `IKillSwitch` | `execution/live_safety.h` | venue-specific |
| `IRiskCheck` | `risk/futures_risk_check.h` | **reuse** `FuturesRiskCheck` |
| `IBracketAdapter` | `exits/bracket_adapter.h` | Phase 4 |

Engine-Hooks (bereits vorhanden, nur liefern):

- `get_reconciler()` / `get_kill_switch()` / `get_risk_check()` / `get_bracket_adapter()`
- `get_liveness_sources()` → DMS heartbeat atomic
- `set_halt_callback()` → fatal WS disconnect → `engine::trigger_halt`
- `set_event_publisher()` / `set_funding_event_factory()` → funding/position snapshots

---

## 4. Dateibaum unter `src/providers/bitget/`

Spiegelung von `binance/`, klar getrennt, **kein** Shared-Code der Binance-Signaturen „hackt“.

```
src/providers/bitget/
  bitget_endpoints.h              # hosts, ports, demo/mainnet, category constants
  bitget_auth.h                   # HMAC-SHA256 → Base64, prehash builder, passphrase
  bitget_parser.h                 # generic JSON needle extractors (copy style from binance_parser)
  bitget_rest_client.h            # Boost.Beast TLS, signed GET/POST, demo header
  bitget_time_sync.h              # GET /api/v2/public/time + skew gate
  bitget_transport.h              # public WS (single topic stream)
  bitget_combined_transport.h     # multi-subscribe public WS
  bitget_private_ws_transport.h   # private WS: login + order/fill/position
  bitget_rest_order_transport.h   # IOrderTransport (POST place/cancel)
  bitget_futures_order_encoder.h  # IOrderEncoder → JSON bodies
  bitget_futures_user_data_parser.h
  bitget_futures_reconciler.h
  bitget_futures_kill_switch.h
  bitget_futures_dead_mans_switch.h
  bitget_futures_safety.h         # advisories (margin mode, liq distance)
  bitget_futures_bracket_adapter.h  # Phase 4
  bitget_backfill.h               # REST candles → prepend JSON frames
  bitget_futures_provider.h       # IProvider composition root
  bitget_futures_register.cpp     # REGISTER_PROVIDER "bitget-futures" + "bitget"
```

**Optional später (nicht Phase 0):**
- `bitget_classic_*.h` — mix/v2 Dual-Surface hinter Config-Flag `api_surface=classic|uta` (Default `uta`).

---

## 5. Build: `ENABLE_BITGET` / `HAS_BITGET`

### 5.1 `CMakeLists.txt`

```cmake
option(ENABLE_BITGET "Build with Bitget exchange provider" OFF)
```

### 5.2 `cmake/Dependencies.cmake` — parallel zu Binance

In `tt_wire_optional_backends`:

```cmake
if(ENABLE_BITGET)
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_sources(${target} PRIVATE
        ${_src}/providers/bitget/bitget_futures_register.cpp)
    target_link_libraries(${target} PUBLIC
        Boost::headers OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(${target} PUBLIC HAS_BITGET)
endif()
```

Alle Bitget-Header mit:

```cpp
#pragma once
#ifdef HAS_BITGET
// ...
#endif
```

Register-TU:

```cpp
#ifdef HAS_BITGET
#include "providers/provider_registry.h"
#include "providers/bitget/bitget_futures_provider.h"
// REGISTER_PROVIDER ...
#endif
```

### 5.3 Tests in `cmake/Sources.cmake`

Unter `TEST_SOURCES` (analog Binance-Block, runtime-gated mit `#ifdef HAS_BITGET`):

```
tests/providers/bitget/test_bitget_auth.cpp
tests/providers/bitget/test_bitget_endpoints.cpp
tests/providers/bitget/test_bitget_parser.cpp
tests/providers/bitget/test_bitget_futures_order_encoder.cpp
tests/providers/bitget/test_bitget_futures_user_data_parser.cpp
tests/providers/bitget/test_bitget_futures_register.cpp
tests/providers/bitget/test_bitget_futures_reconciler.cpp
tests/providers/bitget/test_bitget_futures_kill_switch.cpp
tests/providers/bitget/test_bitget_futures_dead_mans_switch.cpp
tests/providers/bitget/test_bitget_rest_client_time.cpp          # optional Phase 2
tests/providers/bitget/test_bitget_demo_live.cpp                 # opt-in network, default skip
```

### 5.4 Build-Kommandos

```bash
cmake -B build -DENABLE_BITGET=ON -DENABLE_BINANCE=ON -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build -R 'bitget|Bitget' --output-on-failure
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

---

## 6. Registry & CLI-Wiring

### 6.1 `bitget_futures_register.cpp`

```cpp
REGISTER_PROVIDER("bitget-futures", [](const provider_config& cfg) {
    // required: symbol
    // defaults: stream=trade, category=USDT-FUTURES, surface=uta
    // keys: api_key, api_secret, api_passphrase
    // demo: demo|testnet|paptrading ∈ {1,true}
    // risk/DMS keys: same names as binance-futures register
    // return make_shared<BitgetFuturesProvider>(...);
});

// Alias
REGISTER_PROVIDER("bitget", /* same factory */);
```

**Config-Keys (provider_config strings):**

| Key | Pflicht | Default | Bedeutung |
|---|---|---|---|
| `symbol` | ja | — | `BTCUSDT` (UPPER) |
| `stream` | nein | `trade` | public topic mapping |
| `depth_stream` | nein | empty | z.B. `books5` |
| `category` | nein | `USDT-FUTURES` | REST category |
| `api_surface` | nein | `uta` | `uta` \| `classic` (v1 nur uta implementieren) |
| `api_key` / `api_secret` / `api_passphrase` | live | — | Credentials |
| `demo` / `testnet` / `paptrading` | nein | off | Demo endpoints + header |
| `host` / `port` | nein | endpoints | Override |
| `margin_type` | nein | — | advisory expected `crossed`/`isolated` |
| `margin_type_strict` | nein | false | refuse on mismatch |
| `liquidation_warn_pct` | nein | `0.05` | advisory |
| `max_notional_usdt` / `max_leverage` / `min_liquidation_distance_pct` | nein | 0 | `FuturesRiskCheck` |
| `dead_man_countdown_ms` | nein | 0 | 0 = DMS off; Bitget API in **seconds** [5,60] |
| `dead_man_heartbeat_ms` | nein | countdown/3 | refresh interval |
| `dms_attempt_position_close` | nein | false | on HB fail → close-positions |

### 6.2 `main.inc` Erweiterungen (cold path)

1. **Passphrase:**
   - CLI: `--api-passphrase`
   - Env: `TRUETEST_BITGET_API_PASSPHRASE` (und Key/Secret analog)
2. **Credential resolve** generalisieren oder Bitget-Zweig:

```text
TRUETEST_BITGET_API_KEY
TRUETEST_BITGET_API_SECRET
TRUETEST_BITGET_API_PASSPHRASE
```

Fallback nur wenn Provider-Name mit `bitget` startet; Binance-Env unverändert lassen.

3. `pcfg["api_passphrase"] = ...`
4. Help-Text: Provider-Liste um `bitget`, `bitget-futures` erweitern.
5. `--demo` oder bestehendes `--testnet` für Bitget → `pcfg["demo"]="1"` (Semantik: **Demo/paptrading**, nicht Binance-Futures-Testnet-Domain).

---

## 7. Bitget API Surface (konkret)

### 7.1 Domains

| Rolle | Mainnet | Demo (paptrading) |
|---|---|---|
| REST | `https://api.bitget.com` | gleiche Host, Header `paptrading: 1` + **Demo API Key** |
| WS public | `wss://ws.bitget.com/v3/ws/public` | `wss://wspap.bitget.com/v3/ws/public` |
| WS private | `wss://ws.bitget.com/v3/ws/private` | `wss://wspap.bitget.com/v3/ws/private` |

`bitget_endpoints.h`:

```cpp
namespace bitget {
struct endpoints {
  std::string ws_public_host;   // ws.bitget.com
  std::string ws_private_host;  // same host, different path
  std::string ws_port;          // "443"
  std::string rest_host;        // api.bitget.com
  std::string rest_port;        // "443"
  std::string ws_public_path;   // /v3/ws/public
  std::string ws_private_path;  // /v3/ws/private
  bool is_demo = false;
};
endpoints uta_mainnet();
endpoints uta_demo(); // wspap hosts
}
```

### 7.2 Auth (REST + WS)

**Drei Secrets:** API Key + Secret + **Passphrase** (user-chosen bei Key-Erstellung, nicht rekonstruierbar).

**REST Headers (private):**

```
ACCESS-KEY: <apiKey>
ACCESS-SIGN: <base64(hmac_sha256(secret, prehash))>
ACCESS-TIMESTAMP: <ms>
ACCESS-PASSPHRASE: <passphrase>
Content-Type: application/json
locale: en-US
paptrading: 1          # nur Demo
```

**Prehash:**

```
timestamp + METHOD + requestPath + ["?" + queryString] + body
```

Beispiele:

```
1627366780545GET/api/v3/account/assets
1627366780545POST/api/v3/trade/place-order{"category":"USDT-FUTURES",...}
```

- Query-String: alphabetisch sortiert, **ohne** führendes `?` im Prehash-Teil nach `path` — der `?` gehört zwischen path und query laut Guide: `path + "?" + query`.
- Body: **exakter** JSON-String, der gesendet wird (kein Re-Serialize mit anderer Key-Order).
- Sign: HMAC-SHA256(raw bytes) → **Base64** (nicht Hex wie Binance!).

**WS Login:**

```json
{
  "op": "login",
  "args": [{
    "apiKey": "...",
    "passphrase": "...",
    "timestamp": "<ms>",
    "sign": "<base64 hmac of (timestamp + \"GET\" + \"/user/verify\")>"
  }]
}
```

Sign-Pfad für WS ist **immer** `GET/user/verify` (nicht der Channel-Path).

**Impl `bitget_auth.h`:**
- `HmacSha256Base64Signer` (OpenSSL EVP_MAC, analog Binance, aber Base64-Output).
- `sign_rest(ts, method, path, query, body)`.
- `sign_ws_login(ts)`.
- Unit-Tests mit golden vectors aus Bitget-Guide/SDK.

### 7.3 Rate Limits (konservativ einbauen)

| Resource | Limit (doku) | Engine-Policy |
|---|---|---|
| Overall IP | ~6000/min REST+WS | TokenBucket global optional |
| place-order | 10/s/UID | `TokenBucketRateLimiter(10, 10)` oder konservativ 5/s |
| cancel-order | 10/s/UID | shared oder eigen |
| cancel-symbol-order | 5/s/UID | kill-switch path |
| close-positions | 5/s/UID (UTA) | kill-switch / DMS close |
| countdown-cancel-all | **1/s/UID** | heartbeat ≥ 1s; prefer 2–5s |
| WS msgs | 10/s/conn inkl. ping | ping alle 30s, subscribe bursts drosseln |
| WS connections | 100/IP | 1 public + 1 private pro process |
| HTTP 429 | recovery ~minutes | fail loud; no tight retry on safety path |

### 7.4 productType / category / Symbol

| Context | Field | Value |
|---|---|---|
| REST UTA | `category` | `USDT-FUTURES` |
| REST Classic | `productType` | `USDT-FUTURES` (manche Dokus auch lowercase `usdt-futures` — **UTA REST: UPPER**) |
| WS public UTA | `instType` | **lowercase** `usdt-futures` |
| WS private UTA | `instType` | `UTA` |
| Symbol | `symbol` / `instId` | `BTCUSDT` (kein `BTCUSDT_UMCBL` in v2/v3 modern) |

Provider speichert intern immer UPPER `BTCUSDT` und mappt pro Oberfläche.

### 7.5 Marktdaten

#### Public WS Subscribe (UTA v3)

```json
{
  "op": "subscribe",
  "args": [
    {"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
    {"instType":"usdt-futures","topic":"books5","symbol":"BTCUSDT"},
    {"instType":"usdt-futures","topic":"kline","symbol":"BTCUSDT","interval":"1m"}
  ]
}
```

Heartbeat: raw text `"ping"` → `"pong"` alle ~30s; Idle-Disconnect ~2 min.

#### Stream-Mapping CLI → Topic

| `--stream` | Topic | Notes |
|---|---|---|
| `trade` | `publicTrade` | default |
| `ticker` | `ticker` | optional |
| `kline1m` / `candle1m` | `kline` + `interval=1m` | backfill-fähig |
| `kline5m` … | analog | |

| `--depth-stream` | Topic |
|---|---|
| `books1` | `books1` |
| `books5` | `books5` (empfohlen, snapshot) |
| `books50` | `books50` |
| `books` | full book snapshot+delta (komplexer) |

#### REST Market (Startup / Backfill)

| Zweck | Method | Path |
|---|---|---|
| Server time | GET | `/api/v2/public/time` |
| Instruments | GET | `/api/v3/market/instruments?category=USDT-FUTURES&symbol=BTCUSDT` |
| Candles backfill | GET | UTA market candles endpoint (docs: market/candles o.ä.) — **vor Merge gegen Live-Docs verifizieren**; Classic: `/api/v2/mix/market/candles` |

### 7.6 Private REST (UTA v3) — Order Lifecycle

| Action | Method | Path | Body essentials |
|---|---|---|---|
| Place | POST | `/api/v3/trade/place-order` | `category,symbol,side,orderType,qty[,price,timeInForce,clientOid,reduceOnly,posSide,marginMode]` |
| Cancel one | POST | `/api/v3/trade/cancel-order` | `orderId` **oder** `clientOid`, optional `category` |
| Cancel symbol / all | POST | `/api/v3/trade/cancel-symbol-order` | `category` required; `symbol` optional (omit = all in category) |
| Close positions | POST | `/api/v3/trade/close-positions` | `category`; optional `symbol`, `posSide` |
| Countdown DMS | POST | `/api/v3/trade/countdown-cancel-all` | `{"countdown":"40"}` seconds **[5,60]**; `"0"` disarm |
| Positions | GET | `/api/v3/position/current-position?category=USDT-FUTURES&symbol=BTCUSDT` | |
| Assets | GET | `/api/v3/account/assets` | |
| Unfilled orders | GET | `/api/v3/trade/unfilled-orders?...` | |
| Account settings | GET | `/api/v3/account/settings` | hold mode / margin |

**Success envelope:**

```json
{"code":"00000","msg":"success","requestTime":...,"data":{...}}
```

`BitgetRestClient` muss **HTTP 200 + code!=00000** als Fehler behandeln (nicht nur HTTP-Status).

### 7.7 Private WS (UTA)

Nach Login:

```json
{
  "op": "subscribe",
  "args": [
    {"instType":"UTA","topic":"order"},
    {"instType":"UTA","topic":"fill"},
    {"instType":"UTA","topic":"position"}
  ]
}
```

Optional: `fast-fill` für niedrigere Latenz (weniger Felder).  
**Best practice Bitget:** WS ist Source of Truth für Fills; REST place-ack heißt nur „accepted“.

### 7.8 Classic mix/v2 (Dual Surface — dokumentieren, v1 optional)

| Action | Path |
|---|---|
| Place | `POST /api/v2/mix/order/place-order` (`productType`, `size` statt `qty`, `marginCoin`) |
| Cancel one | `POST /api/v2/mix/order/cancel-order` |
| Cancel all | `POST /api/v2/mix/order/cancel-all-orders` |
| Flash close | `POST /api/v2/mix/order/close-positions` |
| WS public | `wss://ws.bitget.com/v2/ws/public` (`channel`/`instId`) |
| WS private | `wss://ws.bitget.com/v2/ws/private` (`orders`/`positions`) |
| **DMS countdown** | **nicht** auf Classic — nur UTA |

**Entscheidung v1:** nur UTA. Classic hinter Feature-Flag erst wenn Operator-Account nicht UTA-fähig.

### 7.9 DMS Enablement

UTA `countdown-cancel-all` Docs: **Zugang muss ggf. über Bitget BD freigeschaltet werden**.  
Provider-Verhalten:

- `dead_man_countdown_ms == 0` → DMS off (ok).
- `> 0` → convert to seconds, clamp **[5,60]**; arm; bei initial fail → **refuse live open()** (wie Binance).
- Log: „DMS may require BD enablement if venue returns permission error“.

### 7.10 One-way vs Hedge

Wie Binance: Engine-Lot-Bookkeeping = **one-way / netted**.  
Startup:

- Settings/position mode abfragen.
- Hedge → **refuse open()** mit klarer stderr-Message: Operator muss one-way setzen.
- Encoder emittiert **kein** `posSide` im one-way Default; `reduceOnly: "yes"` für Closes vom Kill-Switch/DMS.

---

## 8. Phases 0–4 (konkrete Deliverables)

### Phase 0 — Skeleton + Public Market Data (Shadow/Paper)

**Done when:**

1. `ENABLE_BITGET=ON` kompiliert, Registry listet `bitget` / `bitget-futures`.
2. `engine_shadow --provider bitget-futures --symbol BTCUSDT --stream trade` verbindet Public WS, parst Trades, Engine läuft ohne Crash.
3. Optional `--depth-stream books5` → `supports_event_stream()==true`, L2 snapshots in Orderbook.
4. Unit tests: endpoints, auth golden, trade parser fixtures (canned JSON, no network).
5. Scripts: hotpath-json, layer-deps, freeze grün.
6. **Kein** Live-Order-Path; `has_execution()==true` aber live-Block verweigert ohne Keys bzw. baut nur Hybrid/Shadow.

**Dateien:** endpoints, auth, parser, transport, combined_transport, provider (non-live branches), register, tests parser/register/endpoints.

**open() non-live (Copy-Pattern aus Binance):**

```text
state=opening
build FuturesRiskCheck if caps
create public WS transport(s)
optional backfill + PrependTransport
Binance-style HybridExecutor OR TradeTapeShadowAdapter by mode
transport->open()
state=open
```

### Phase 1 — REST Client + Auth + Instruments Probe

**Done when:**

1. `BitgetRestClient` signed GET/POST, demo header, clock sync via `/api/v2/public/time`.
2. Clock skew gate (analog `binance::verify_clock_skew`) — refuse live if skew too large.
3. Instruments probe: symbol exists, status trading, extract tick size / lot size → `get_instrument()` optional.
4. Tests: prehash+sign vectors; time parse; instruments canned JSON.

### Phase 2 — Live Execution Path (Demo first)

**Done when:**

1. `BitgetFuturesOrderEncoder` place/cancel JSON.
2. `BitgetRestOrderTransport`: map `code==00000` → ok + `orderId` aus `data`.
3. `BitgetPrivateWsTransport`: connect → login → subscribe order+fill(+position); fatal disconnect → halt_cb.
4. `BitgetFuturesUserDataParser`: order/fill → `parsed_exec`; position → `parsed_position_snapshot`.
5. Provider live branch: rest, minter (`ClientOrderIdMinter`, clientOid charset Bitget: `^[.A-Z:/a-z0-9_-]{1,32}$`), rate limiter, ExecutionBridge, open order.
6. Reconciler: assets available vs local cash; position size signed vs local qty; refuse on drift.
7. Demo live test (skipped unless env `TRUETEST_BITGET_DEMO_*=`): tiny place+cancel.

**Startup refuse checklist (live):**

- [ ] keys + passphrase non-empty  
- [ ] clock skew OK  
- [ ] instrument exists / trading  
- [ ] one-way mode  
- [ ] optional margin_type_strict  
- [ ] reconciler empty error  
- [ ] private WS open  
- [ ] DMS arm if configured  

### Phase 3 — Safety: Kill-Switch + DMS + close-positions Advantage

**Done when:**

1. **Kill-Switch** (gold path Bitget > Binance flatten half):
   1. `POST /api/v3/trade/cancel-symbol-order` mit `category=USDT-FUTURES`, `symbol=BTCUSDT` (scoped).
   2. `POST /api/v3/trade/close-positions` mit `category` + `symbol` (kein manuelles reduceOnly-MARKET basteln nötig — **das ist der Bitget-Vorteil**).
   3. Deadline-bounded per-call timeouts; HTTP/code fail → return false, loud log; **kein Retry-Loop**.
2. **DMS:**
   - Arm: countdown seconds clamped; heartbeat thread.
   - Disarm: `countdown=0` on orderly close (after stop thread).
   - Liveness atomic für `WorkerWatchdog` (deadline ~ 3× heartbeat).
   - Optional `dms_attempt_position_close`: bei persistentem HB-Fail → `close-positions` (nicht nur Orders).
3. Tests: fake post_fn inject; arm fail; disarm; kill-switch sequence order verified.

**Vergleich Binance (Agents müssen den Unterschied kennen):**

| | Binance Futures | Bitget UTA |
|---|---|---|
| Cancel all | `DELETE /fapi/v1/allOpenOrders` | `POST .../cancel-symbol-order` |
| Flatten | positionRisk + reduceOnly MARKET | **`close-positions`** one-shot |
| DMS | `countdownCancelAll` ms, symbol-scoped | `countdown-cancel-all` **seconds**, **account-wide orders**, BD enablement |
| DMS close | custom reduceOnly | reuse `close-positions` |

### Phase 4 — Hardening + Brackets + Ops

1. Funding / balance events → `event_publisher` wenn WS account channel verdrahtet.
2. Bracket adapter (venue TP/SL fields auf place-order **oder** separate plan orders — Design-Review, nicht silent).
3. Maintenance margin table: wenn Bitget tiered leverage brackets exposed, laden; sonst flat `maintenance_margin_pct`.
4. Operator SOP under `docs/operations/` (Demo drill, DMS SIGKILL drill).
5. CLI flags docs + user manual mention.
6. Optional classic surface.

---

## 9. Parser Field Maps

Alle Parser: **string_view needle extract**, keine DOM-JSON-Lib auf Hot Path.

### 9.1 Public Trade → `provider::tick` / internal trade record

UTA push (typisch):

```json
{
  "arg": {"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
  "data": [{
    "i": "tradeId",
    "p": "97000.5",
    "v": "0.01",
    "S": "buy",
    "T": "1710000000000"
  }],
  "ts": 1710000000001
}
```

| Wire | TrueTest |
|---|---|
| `p` | price |
| `v` | quantity (base) |
| `S` / `side` | buy→0/1 mapping wie Binance parser |
| `T` / `ts` | event time ms |
| `arg.symbol` | symbol |

**Fixture-driven:** Felder gegen Live-WS einmal capture; Tests mit realem Capture-Snippet. Docs drift → Test fail → fix map.

### 9.2 books5 → `provider::l2_snapshot`

| Wire | TrueTest |
|---|---|
| `b[][0]`,`b[][1]` | bid price, qty |
| `a[][0]`,`a[][1]` | ask price, qty |
| `ts` | timestamp |
| `action` | snapshot (books5 always) |

Für `books` full: `action=update` → `l2_update` oder snapshot rebuild — **Phase 0 nur books5 snapshots**.

### 9.3 kline → `provider::bar`

| Wire | TrueTest |
|---|---|
| open/high/low/close/vol arrays or fields | OHLCV |
| interval from arg | bar period |
| confirm flag if any | only emit closed bars if engine expects closed |

### 9.4 Private order channel → `parsed_exec`

| Wire (UTA order) | `parsed_exec` |
|---|---|
| `clientOid` | `client_order_id` |
| `orderId` | `exchange_order_id` |
| `symbol` | `symbol` |
| `side` buy/sell | `side` |
| `orderStatus` | kind map |
| `cumExecQty` | `cumulative_qty` |
| last fill qty/price if present | `last_fill_*` |
| `feeDetail` | commission + asset |
| `cancelReason` / err | `error` on reject |

**Status map (Vorschlag, Tests fixieren):**

| `orderStatus` | `parsed_exec::kind` |
|---|---|
| `new` / `live` / `init` | `ack` |
| `partially_filled` | `partial_fill` |
| `filled` | `full_fill` |
| `cancelled` / `canceled` | `canceled` |
| `rejected` | `rejected` |

**Wichtig:** fill channel und order channel können beide feuern — Bridge ist idempotent über client id; Parser darf doppelte full_fills liefern, Bridge deduped by design? **Verify against ExecutionBridge behavior** — prefer fill channel for `last_fill_*` accuracy, order channel for ack/cancel.

### 9.5 Position channel → `parsed_position_snapshot`

| Wire | snapshot |
|---|---|
| `symbol` | `position_row.symbol` |
| `size` / `total` signed or + `posSide` | **signed qty**: long>0, short<0 (normalize!) |
| `marginMode` crossed/isolated | canonical `CROSSED`/`ISOLATED` |
| `posSide` | `position_side` |
| ts | `ts` |

### 9.6 REST reconciler extracts

**Assets** (`/api/v3/account/assets`): available USDT (Feldnamen gegen Capture: oft `available` / `availableEquity` / coin list) → local cash compare.

**Position** (`current-position`): size → signed qty.

Helper static methods wie `BinanceFuturesReconciler::extract_position_amt` — unit-tested with canned bodies.

---

## 10. Order Encode (UTA JSON)

`IOrderEncoder` returns:

```cpp
struct encoded_order {
  std::string endpoint;      // "/api/v3/trade/place-order"
  std::string wire_payload;  // raw JSON body
  std::string client_order_id;
};
```

### 10.1 Place LIMIT buy

```json
{
  "category": "USDT-FUTURES",
  "symbol": "BTCUSDT",
  "side": "buy",
  "orderType": "limit",
  "qty": "0.001",
  "price": "90000",
  "timeInForce": "gtc",
  "clientOid": "tt-...",
  "reduceOnly": "no",
  "marginMode": "crossed"
}
```

### 10.2 Place MARKET

```json
{
  "category": "USDT-FUTURES",
  "symbol": "BTCUSDT",
  "side": "sell",
  "orderType": "market",
  "qty": "0.001",
  "clientOid": "tt-...",
  "reduceOnly": "no"
}
```

### 10.3 Type map (engine → Bitget)

| `order_type` | Bitget `orderType` | Notes |
|---|---|---|
| limit | `limit` | + price + timeInForce |
| market | `market` | no price |
| stop / stop_market | **Phase 4** | preset tp/sl fields or separate; refuse encode until implemented |

TIF: `gtc|ioc|fok|post_only` lowercase.

### 10.4 Cancel

Endpoint `/api/v3/trade/cancel-order`:

```json
{"category":"USDT-FUTURES","orderId":"..."}
// or
{"category":"USDT-FUTURES","clientOid":"..."}
```

### 10.5 Order Transport nuance vs Binance

Binance: query-string params + HMAC hex + DELETE cancel.  
Bitget: **JSON body** + HMAC base64 + **POST cancel**.  

`IOrderTransport::cancel` muss POST callen, nicht DELETE. Factory:

```cpp
make_bitget_rest_order_transport(rest) {
  post = [&](ep, body){ return rest->post_json(ep, body); };
  // cancel uses post as well
}
```

Response ok iff HTTP 2xx **and** `"code":"00000"`; `exchange_order_id` from `data.orderId`.

---

## 11. Safety Detail

### 11.1 Reconciler

```text
GET positions → signed qty
GET assets → available USDT
compare within tolerance_bps
any HTTP/code/parse error → non-empty error string (engine refuses start)
no testnet-reset soft-pass (same philosophy as Binance futures)
```

### 11.2 Kill-Switch (Bitget gold)

```text
bool cancel_all_and_flatten(deadline):
  set per-call timeout = min(1500ms, deadline/3)
  1) POST cancel-symbol-order {category, symbol}
     - treat "no orders" success codes as OK (map venue codes via tests)
  2) if time expired → false
  3) POST close-positions {category, symbol}
     - empty position should still be OK (success with empty list or no-op code)
  4) return true only if both steps OK
```

**Kein** Schedule-Retry. **Kein** auto-clear halt.

### 11.3 DMS

```text
countdown_sec = clamp(countdown_ms/1000, 5, 60)  // refuse if operator set 1–4s without clamp? 
// Policy: if configured ms < 5000 → bump to 5s and WARN, or refuse — prefer WARN+clamp for demo, refuse if ms==0 handled as off

start():
  post countdown=N; on fail return false
  spawn heartbeat every heartbeat_ms (min 1000ms due to 1/s limit)
  on success: last_beat_ms = now
  on 2 consecutive fails: optional close-positions; stop refreshing; let venue timer expire

stop(): join thread
disarm(): countdown=0
```

**Account-wide caveat:** Bitget DMS cancels **all UTA open orders**, not only symbol. Document loudly for multi-strategy operators. v1 single-symbol process assumption matches current engine usage.

### 11.4 close-positions Advantage (Messaging for Agents)

Binance Kill-Switch: 3 REST calls (cancel, positionRisk, reduceOnly order) with qty formatting footguns.  
Bitget: **2 REST calls**, venue-native flatten. Prefer `close-positions` over reinventing reduceOnly unless classic surface forces it.

---

## 12. Config / Operator Examples

### 12.1 Shadow (public only)

```bash
cmake -B build -DENABLE_BITGET=ON && cmake --build build -j"$(nproc)" --target engine_shadow

./build/engine_shadow \
  --provider bitget-futures \
  --symbol BTCUSDT \
  --stream trade \
  --depth-stream books5 \
  --no-pin --status-format off
```

### 12.2 Demo live drill (tiny)

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

### 12.3 Geo precondition (SOP snippet)

```text
[ ] Operator jurisdiction allows Bitget use (NOT DE/FR restricted accounts)
[ ] API key created with UTA trade R/W; IP whitelist set
[ ] Demo key for drills; mainnet keys never in argv if avoidable
[ ] One-way position mode
[ ] DMS BD enablement confirmed if using countdown
[ ] Kill-switch drill on demo before any mainnet
```

---

## 13. Tests (Matrix)

| Test | Art | Assert |
|---|---|---|
| `test_bitget_auth` | unit | prehash strings + base64 HMAC match golden |
| `test_bitget_endpoints` | unit | mainnet vs demo hosts/paths |
| `test_bitget_parser` | unit | trade/books/kline fixtures → fields |
| `test_bitget_futures_order_encoder` | unit | limit/market/cancel JSON exact |
| `test_bitget_futures_user_data_parser` | unit | order statuses, fills, position sign |
| `test_bitget_futures_register` | unit | missing symbol throws; demo flag selects endpoints |
| `test_bitget_futures_reconciler` | unit | cash/pos match & mismatch |
| `test_bitget_futures_kill_switch` | unit | call order cancel→close; timeout; HTTP fail |
| `test_bitget_futures_dead_mans_switch` | unit | arm/disarm/heartbeat/fail close |
| `test_bitget_demo_live` | network opt-in | skip without env |

**Pattern:** inject `post_fn` / canned bodies — **no live network in default CI**.

---

## 14. Definition of Done (core-level)

- [ ] Phases 0–3 merged (Phase 4 optional follow-up)
- [ ] `ENABLE_BITGET=ON` builds all three binaries
- [ ] Registry: `bitget` + `bitget-futures`
- [ ] Shadow public stream works against mainnet public WS
- [ ] Demo live: place/cancel + reconciler + kill-switch + DMS drills documented
- [ ] `./scripts/check-hotpath-json.sh` pass
- [ ] `./scripts/check-layer-deps.sh` pass
- [ ] `./scripts/check-live-safety-freeze.sh` pass (no accidental freeze edits)
- [ ] No venue leakage into engine/risk/threading
- [ ] No R*/S* red-line violations
- [ ] Geo restrictions documented as operator precondition
- [ ] Dual surface classic vs UTA documented; default UTA
- [ ] `/testing` mindset: focused tests green; network tests gated

---

## 15. Quirks & Pitfalls (Agents lesen zweimal)

1. **Base64 vs Hex signatures** — Copy-paste von Binance-Auth = silent 40001-style fails.  
2. **Passphrase** — drittes Secret; CLI/env muss existieren.  
3. **JSON body signing** — Key-Order stabil halten (manuell string build, kein unordered map dump).  
4. **REST `category` UPPER vs WS `instType` lower** — `USDT-FUTURES` vs `usdt-futures`.  
5. **HTTP 200 + business code** — immer `code=="00000"` prüfen.  
6. **Cancel is POST**, not DELETE.  
7. **DMS unit = seconds [5,60]**, account-wide, BD gate; Binance = ms, symbol-scoped.  
8. **close-positions** existiert UTA **und** classic — nutzen.  
9. **Demo:** `wspap` + Demo keys + REST header `paptrading:1`. Mainnet keys auf wspap = fail.  
10. **Hedge mode** refuse at open.  
11. **clientOid charset/length** strenger als Binance; Minter alphabet anpassen.  
12. **Qty/price precision** aus instruments; Encoder muss steppen (sonst reject).  
13. **Geo DE/FR** — implementieren trotzdem; Operator-SOP blockt Betrieb.  
14. **Kein `nlohmann` im Parser** — allow-list hotpath-json.  
15. **Private WS login timestamp** — ms, ~30s validity; clock sync first.  
16. **Ping is raw `"ping"` string**, not JSON.  
17. **Multi-channel public:** ein WS, multi `args` subscribe (CombinedTransport).  
18. **Funding** — account/position events; nicht mit trade ticks mischen.  
19. **Rate limit 1/s on DMS** — heartbeat ≥ 1s.  
20. **Classic has no countdown DMS** — surface=classic ⇒ DMS refuse or software-only watchdog (prefer refuse live if DMS requested).

---

## 16. Implementation Checklist (Copy into PR)

```text
[ ] CMake ENABLE_BITGET + HAS_BITGET
[ ] bitget_endpoints.h
[ ] bitget_auth.h + tests
[ ] bitget_parser.h + trade/books fixtures
[ ] bitget_transport.h + combined
[ ] bitget_futures_provider.h non-live open/close
[ ] bitget_futures_register.cpp (two names)
[ ] main.inc passphrase + env + pcfg
[ ] bitget_rest_client.h + time sync
[ ] order encoder + rest order transport
[ ] private WS transport + user data parser
[ ] reconciler + kill-switch + DMS
[ ] live open() refuse gates
[ ] FuturesRiskCheck wire-up
[ ] tests matrix
[ ] three check scripts
[ ] demo SOP note + geo precondition
```

---

## 17. Code Anchors (read before coding)

| Topic | Path |
|---|---|
| IProvider contract | `src/providers/provider.h` |
| Registry macro | `src/providers/provider_registry.h` |
| Live safety ifaces | `src/execution/live_safety.h` |
| ExecutionBridge deps | `src/execution/execution_bridge.h` |
| Order/Fill ifaces | `src/execution/order_encoder.h`, `order_transport.h`, `fill_parser.h`, `fill_transport.h` |
| Golden provider | `src/providers/binance/binance_futures_provider.h` |
| Register pattern | `src/providers/binance/binance_futures_register.cpp` |
| Kill-switch pattern | `src/providers/binance/binance_futures_kill_switch.h` |
| DMS pattern | `src/providers/binance/binance_futures_dead_mans_switch.h` |
| Reconciler pattern | `src/providers/binance/binance_futures_reconciler.h` |
| Auth/HMAC style | `src/providers/binance/binance_auth.h` (adapt Base64) |
| ENABLE wiring | `cmake/Dependencies.cmake`, `CMakeLists.txt` |
| Source lists | `cmake/Sources.cmake` |
| Layer rules | `scripts/check-layer-deps.sh` |
| Agent rules | `AGENTS.md` |
| Futures ops vibe | `docs/operations/02-futures-testnet.md` |

---

## 18. References (official)

- API index: https://www.bitget.com/api-doc/  
- UTA intro: https://www.bitget.com/api-doc/uta/intro  
- UTA guide (auth, domains, demo, limits): https://www.bitget.com/api-doc/uta/guide  
- Place order: https://www.bitget.com/api-doc/uta/trade/Place-Order  
- Cancel order: https://www.bitget.com/api-doc/uta/trade/Cancel-Order  
- Cancel symbol/all: https://www.bitget.com/api-doc/uta/trade/Cancel-All-Order  
- Close positions: https://www.bitget.com/api-doc/uta/trade/Close-All-Positions  
- Countdown cancel all (DMS): https://www.bitget.com/api-doc/uta/trade/CountDown-Cancel-All  
- Positions: https://www.bitget.com/api-doc/uta/trade/Get-Position  
- Account assets: https://www.bitget.com/api-doc/uta/account/Get-Account  
- Instruments: https://www.bitget.com/api-doc/uta/public/Instruments  
- WS order/fill/position channels under `api-doc/uta/websocket/private/`  
- Classic mix cancel-all / flash-close: under `api-doc/classic/contract/trade/`  
- Classic ↔ UTA upgrade mapping: https://www.bitget.com/api-doc/classic/uta-api-upgrade-guide  
- Terms / geo (DE/FR): Bitget Support Terms of Use + FR/DE service notices  
- SDK reference (sign vectors): https://github.com/BitgetLimited/v3-bitget-api-sdk  

---

## 19. Empfohlene Arbeitsreihenfolge für einen Grok-Build-Agenten

1. Lies `binance_futures_provider.h` live-Block und `binance_futures_register.cpp` end-to-end.  
2. Scaffold Dateien + CMake + leere Provider mit `name()` / registry.  
3. Phase 0 Public WS trade only → manuell `engine_shadow`.  
4. Auth + REST time + instruments.  
5. Encoder + order transport unit tests before any network order.  
6. Private WS login on demo + parser fixtures from captured messages.  
7. Wire ExecutionBridge live branch.  
8. Reconciler → Kill-Switch (`close-positions`) → DMS.  
9. Full check scripts + focused ctest.  
10. Write short ops note; **do not** claim mainnet readiness without human CCB.

---

*Last updated: 2026-07-29 — full Grok Build implementation guide for Bitget USDT-M futures (UTA v3 primary).*
