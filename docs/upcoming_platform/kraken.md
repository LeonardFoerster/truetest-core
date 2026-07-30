# Kraken Futures Provider — Grok Build Implementation Guide

**Registry-Namen:** `"kraken"` / `"kraken-futures"` (kanonisch: `"kraken-futures"`, Alias optional `"kraken"`)  
**Pfad:** `src/providers/kraken/`  
**Gold-Referenz:** `src/providers/binance/*` (USDT-M Futures, nicht Spot)  
**Zielbinaries:** `engine_shadow` (Daten + Paper/Shadow), später `engine_live` (Live-Orders nur hinter `TT_TARGET=LIVE`)  
**Build-Flag:** `ENABLE_KRAKEN` → Compile-Define `HAS_KRAKEN`  
**Stand der Recherche:** 2026-07 (offizielle Docs: [docs.kraken.com](https://docs.kraken.com/))  

> **Warum Kraken?** Geringere Alt-Liquidität als Binance, dafür regulierter EU-/UK-Fit, saubere Dead-Man’s-Switch-API (`cancelallordersafter`), net-only Positionsmodell (kein Hedge-Modus-Chaos), und multi-collateral linear (`PF_*`) als klare v1-Zielwelt. TrueTest bleibt venue-agnostisch: der Provider ist der **einzige** Venue-Erweiterungspunkt.

---

## 1. Ziele und Scope

### 1.1 In Scope (v1)

| Ziel | Konkret |
|------|---------|
| **Produktfamilie** | Kraken **Derivatives / Futures** — **nicht** Spot |
| **Kontraktklasse v1** | **Linear multi-collateral perpetual** → `PF_*` (z. B. `PF_XBTUSD`) |
| **Modi** | Shadow/Paper zuerst; Live erst nach Safety-Parität mit Binance-Futures |
| **Marktdaten** | WS public: `trade`, `book`, optional `ticker` |
| **User-Data** | WS private (Challenge-Auth): `fills`, `open_orders`, `open_positions`, `balances` |
| **Orders** | REST: `sendorder`, `cancelorder`, `editorder` (optional Phase 3), `cancelallorders` |
| **DMS** | REST: `cancelallordersafter` (Timeout in **Sekunden**, `0` = disarm) |
| **Safety** | `IReconciler`, `IKillSwitch`, DMS-Heartbeat, `FuturesRiskCheck`, Halt-Callback |

### 1.2 Out of Scope (v1 — explizit später)

- Inverse (`PI_*`, `FI_*`) und dated linear (`FF_*`) — eigene Encoder-/Margin-Logik
- Options (`OF_*` / `OI_*`)
- FIX-API
- Hedge mode (existiert bei Kraken Futures ohnehin **nicht** — net-only)
- Multi-Symbol Cross-Margin Engine (gleiches Gap wie Binance)
- Withdrawals / Transfers / Subaccounts

### 1.3 Strategische Priorität

```
Phase 0  Public Market Data (WS trade + book) + Register + CMake
Phase 1  REST Auth + instruments + reconciler read-only + shadow wiring
Phase 2  Order encode/send/cancel + user-data fills/open_orders
Phase 3  DMS + kill-switch + risk check + live open() gates
Phase 4  Demo soak → tiny live (attended) + Ops-Docs
```

**v1-Symbol-Default:** `PF_XBTUSD` (nicht `BTCUSDT`, nicht `XBTUSD`, nicht `PI_XBTUSD`).

---

## 2. Non-Negotiables (TrueTest + Kraken)

Diese Regeln sind **hart**. Verstöße = Review-Fail.

| ID | Regel |
|----|-------|
| N1 | Provider ist **einziger** Venue-Extension-Point. Kein `HAS_KRAKEN` in `engine/`, `core/`, `threading/`, `risk/` (außer generische `FuturesRiskCheck`-Nutzung). |
| N2 | Compile-time Live-Gate bleibt absolut (`tt_target.h`). Keine Runtime-„allow live“-Schalter. |
| N3 | Halt ist **write-once terminal**. Kein Auto-Resume, kein „hilfreiches“ Retry auf Kill/DMS/Reconciler. |
| N4 | Hot Path: **kein** `nlohmann/json`, kein Heap auf Event-Loop; Parser wie Binance mit `string_view` + manuellem Key-Find. |
| N5 | SPSC: Engine-Loop ist sole Producer für Engine-Rings. Provider-Threads → eigene Transport-Queues, Handoff wie bestehendes Pattern. |
| N6 | Reconciler **default-refuse** bei Drift/HTTP-Fehler. User-Data-WS ist Source of Truth; REST advisory bis reconciled. |
| N7 | DMS: fester countdown, fester heartbeat — **kein** adaptives Verlängern unter Last. |
| N8 | `reduceOnly` auf Flatten-Orders **immer** true. |
| N9 | Net-only: Position signed long>0 / short<0. Kein `positionSide`. |
| N10 | Gold-Parität: gleiche Safety-Hooks wie `BinanceFuturesProvider` (`get_reconciler`, `get_kill_switch`, `get_risk_check`, `get_bracket_adapter` optional, `get_liveness_sources`, `set_halt_callback`). |
| N11 | Freeze-Flächen von Binance **nicht** anfassen für Kraken-Arbeit. Kraken-Safety-Dateien sind **neu** und später ggf. eigene Freeze-Liste. |
| N12 | Layer-Deps: `providers` darf `core types utils data orderbook execution engine exits risk simulation threading ui` — nicht umgekehrt Venue in Core pushen. |

---

## 3. Architektur-Map (Spiegel von Binance Futures)

### 3.1 IProvider-Vertrag (bestehend)

```1:132:src/providers/provider.h
// IProvider — name, open/close, transport, execution,
// get_reconciler / get_kill_switch / get_risk_check / get_bracket_adapter,
// supports_event_stream + get_event_parser,
// get_liveness_sources, set_halt_callback, set_event_publisher,
// set_funding_event_factory
```

Kraken implementiert denselben Vertrag. Engine bleibt unberührt außer CLI-Hints.

### 3.2 Komponenten-Diagramm

```
                    ┌─────────────────────────────────────┐
                    │         KrakenFuturesProvider        │
                    │  (IProvider composition root)        │
                    └──────────────┬──────────────────────┘
           ┌───────────────────────┼──────────────────────────┐
           ▼                       ▼                          ▼
  KrakenTransport          KrakenRestClient            ExecutionBridge
  (public WS v1)           (HTTPS + Authent)           (deps: order_tx,
   trade/book/ticker                                    fills_tx, encoder,
           │                       │                    parser, rate_limiter)
           ▼                       │                          │
  KrakenMarketParser               │              ┌───────────┴────────────┐
  → provider::event                │              ▼                        ▼
  (tick / l2_*)                    │     KrakenOrderEncoder      KrakenUserDataTransport
                                   │     sendorder/cancel        (private WS + challenge)
                                   │                                      │
                                   │                                      ▼
                                   │                           KrakenUserDataParser
                                   │                           → parsed_exec /
                                   │                             parsed_position_snapshot
                                   ▼
                    ┌──────────────────────────────────────┐
                    │ Safety (live only)                   │
                    │  KrakenFuturesReconciler  (IReconciler)│
                    │  KrakenFuturesKillSwitch  (IKillSwitch)│
                    │  KrakenFuturesDeadMansSwitch (cancelallordersafter)
                    │  FuturesRiskCheck (generisch, reuse) │
                    └──────────────────────────────────────┘
```

### 3.3 Mapping Binance → Kraken

| Binance Futures | Kraken Futures | Bemerkung |
|-----------------|----------------|-----------|
| `fstream` combined streams | `wss://…/ws/v1` + `subscribe` JSON | Kein combined-URL-Pfad; eine Connection, multi-feed |
| `listenKey` user stream | Challenge-Response private feeds | **Größter Designunterschied** |
| `HMAC-SHA256` hex query `signature` | `HMAC-SHA512(SHA256(postData+Nonce+path))` base64 `Authent` | Secret ist **base64-decoded** |
| `/fapi/v1/order` | `/derivatives/api/v3/sendorder` | POST form params |
| `DELETE allOpenOrders` | `POST cancelallorders` | |
| `countdownCancelAll` (ms) | `cancelallordersafter` (**seconds**) | `timeout=0` disarm |
| `positionRisk` | `openpositions` + WS `open_positions` | signed `size`/`balance` |
| `availableBalance` | `accounts` / WS `balances` (`flex_futures.available_margin`) | Multi-M Wallet |
| Symbol `BTCUSDT` | `PF_XBTUSD` | **XBT ≠ BTC** |
| One-way enforced | Always net-only | Kein dual-side Probe nötig |
| `reduceOnly` | `reduceOnly` | gleich |
| Testnet `binancefuture.com` | `demo-futures.kraken.com` | Separate Keys |

---

## 4. Dateibaum (Ziel)

Alles unter `src/providers/kraken/`, analog Binance, header-first wo möglich:

```
src/providers/kraken/
  kraken_endpoints.h              # mainnet/demo hosts + path constants
  kraken_auth.h                   # SHA256 + HMAC-SHA512 Authent + WS challenge sign
  kraken_rest_client.h            # HTTPS client (Boost.Beast/OpenSSL), signed GET/POST
  kraken_transport.h              # public WS: connect, subscribe trade/book, ping
  kraken_market_parser.h          # trade / book_snapshot / book delta → provider::event
  kraken_order_encoder.h          # IOrderEncoder → sendorder / cancelorder payloads
  kraken_user_data_transport.h    # private WS: challenge → subscribe fills/orders/pos/bal
  kraken_user_data_parser.h       # IFillParser
  kraken_futures_reconciler.h     # IReconciler
  kraken_futures_kill_switch.h    # IKillSwitch
  kraken_futures_dead_mans_switch.h
  kraken_futures_safety.h         # advisories (margin/liq) helpers
  kraken_futures_provider.h       # IProvider composition root
  kraken_futures_register.cpp     # REGISTER_PROVIDER("kraken-futures", …)
  # optional later:
  kraken_futures_bracket_adapter.h
  kraken_json_util.h              # shared string_view extract_* (kein nlohmann)
```

**Tests** (explizit in `cmake/Sources.cmake`):

```
tests/test_kraken_auth.cpp
tests/test_kraken_order_encoder.cpp
tests/test_kraken_market_parser.cpp
tests/test_kraken_user_data_parser.cpp
tests/test_kraken_futures_register.cpp
tests/test_kraken_futures_reconciler.cpp
tests/test_kraken_futures_kill_switch.cpp
tests/test_kraken_futures_dead_mans_switch.cpp
tests/test_kraken_futures_demo_live.cpp   # gated, optional network
```

---

## 5. CMake / Build-Integration

### 5.1 `CMakeLists.txt`

```cmake
option(ENABLE_KRAKEN "Build with Kraken Futures (Derivatives) provider" OFF)
```

### 5.2 `cmake/Dependencies.cmake` — in `tt_wire_optional_backends`

Spiegel von Binance:

```cmake
if(ENABLE_KRAKEN)
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_sources(${target} PRIVATE
        ${_src}/providers/kraken/kraken_futures_register.cpp)
    target_link_libraries(${target} PUBLIC
        Boost::headers OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(${target} PUBLIC HAS_KRAKEN)
endif()
```

### 5.3 `cmake/Sources.cmake`

- Register-`.cpp` kommt über `tt_wire_optional_backends` (wie Binance), **nicht** in `ENGINE_CORE_SOURCES` unconditional.
- Tests unter `TEST_SOURCES` **conditional**:

```cmake
if(ENABLE_KRAKEN)
  list(APPEND TEST_SOURCES
    tests/test_kraken_auth.cpp
    tests/test_kraken_order_encoder.cpp
    # … rest
  )
endif()
```

### 5.4 Header-Guards

Jedes Kraken-Header:

```cpp
#pragma once
#ifdef HAS_KRAKEN
// …
#endif // HAS_KRAKEN
```

Register-Datei:

```cpp
#ifdef HAS_KRAKEN
#include "providers/provider_registry.h"
#include "providers/kraken/kraken_futures_provider.h"
// REGISTER_PROVIDER(...)
#endif
```

### 5.5 Build-Kommandos

```bash
cmake -B build -DENABLE_KRAKEN=ON -DENABLE_LIVE_DATA=ON -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
ctest --test-dir build -R 'Kraken|kraken' --output-on-failure
```

**Hinweis:** `ENABLE_LIVE_DATA` für generische WS-Infrastruktur; Kraken selbst braucht Boost+OpenSSL wie Binance. Wenn `ENABLE_BINANCE` und `ENABLE_KRAKEN` beide an: kein Konflikt, getrennte `HAS_*`.

---

## 6. Registry & Config

### 6.1 Registration (kanonisch)

```cpp
REGISTER_PROVIDER("kraken-futures", [](const provider_config& cfg) {
    // parse symbol, stream, api_key, api_secret, testnet/demo, host, port,
    // risk caps, dead_man_*, dms_attempt_position_close
    // default stream = "trade"
    // default symbol required e.g. PF_XBTUSD
});
// Optional alias:
REGISTER_PROVIDER("kraken", /* same factory or thin wrapper */);
```

Gold-Muster: `src/providers/binance/binance_futures_register.cpp`.

### 6.2 `provider_config` Keys

| Key | Beispiel | Pflicht |
|-----|----------|---------|
| `symbol` | `PF_XBTUSD` | ja |
| `stream` | `trade` \| `book` \| `ticker` | nein (default `trade`) |
| `depth_stream` | `book` (wenn L2 parallel zu trade) | nein |
| `api_key` / `api_secret` | Demo/Prod keys | live ja |
| `testnet` / `demo` | `1`/`true` | nein → mainnet |
| `host` / `port` | Override WS host | nein |
| `max_notional_usdt` | `15000` | nein |
| `max_leverage` | `2.5` | nein |
| `min_liquidation_distance_pct` | `7` | nein |
| `dead_man_countdown_ms` | `60000` | nein (0=off) |
| `dead_man_heartbeat_ms` | `15000` | nein (default countdown/3) |
| `dms_attempt_position_close` | `true` | nein |
| `margin_type` | `cross`/`isolated` | advisory |
| `margin_type_strict` | `1` | nein |

### 6.3 CLI (`main.inc`)

Bestehende Flags reichen größtenteils:

```bash
./build/engine_shadow \
  --provider kraken-futures \
  --symbol PF_XBTUSD \
  --stream trade \
  --testnet \
  --api-key "$KRAKEN_FUTURES_DEMO_KEY" \
  --api-secret "$KRAKEN_FUTURES_DEMO_SECRET"
```

**Kleine CLI-Erweiterungen (Phase 0/1):**

1. Help-Text / Error-Hint für `kraken-futures` (neben binance/local/synthetic).
2. `--testnet` mappt bei diesem Provider auf **demo** endpoints (nicht Binance-Testnet-Hosts).
3. Optional: Env-Resolve `KRAKEN_FUTURES_KEY` / `KRAKEN_FUTURES_SECRET` in `resolve_exchange_credentials` (oder generisch lassen und Operator exportiert in `--api-key`).

**Nicht** `validate_binance_futures_live_phase0` für Kraken missbrauchen — eigene Phase-0-Checks oder generische Live-Gates.

### 6.4 Endpoints (`kraken_endpoints.h`)

```cpp
namespace kraken {

struct endpoints {
    std::string rest_host;   // futures.kraken.com
    std::string rest_port;   // 443
    std::string ws_host;     // futures.kraken.com
    std::string ws_port;     // 443
    std::string ws_path;     // /ws/v1
    std::string rest_prefix; // /derivatives/api/v3
    bool is_demo = false;
};

inline endpoints futures_mainnet() {
    return {"futures.kraken.com", "443",
            "futures.kraken.com", "443", "/ws/v1",
            "/derivatives/api/v3", false};
}

inline endpoints futures_demo() {
    return {"demo-futures.kraken.com", "443",
            "demo-futures.kraken.com", "443", "/ws/v1",
            "/derivatives/api/v3", true};
}

// Signing path WITHOUT the "/derivatives" routing prefix:
//   HTTP URL:  https://host/derivatives/api/v3/sendorder
//   endpointPath for Authent: /api/v3/sendorder
inline std::string signing_path(std::string_view rest_resource) {
    // rest_resource e.g. "sendorder" or "/sendorder"
    // → "/api/v3/sendorder"
}

} // namespace kraken
```

**Kritisch:** Signatur-`endpointPath` = `/api/v3/<name>`, **nicht** `/derivatives/api/v3/<name>`. Request-URL enthält `/derivatives`. Das ist der #1 Auth-Pitfall.

---

## 7. Offizielle API — konkret für Implementierer

### 7.1 Base URLs

| Env | REST | WebSocket |
|-----|------|-----------|
| **Prod** | `https://futures.kraken.com/derivatives/api/v3/` | `wss://futures.kraken.com/ws/v1` |
| **Demo** | `https://demo-futures.kraken.com/derivatives/api/v3/` | `wss://demo-futures.kraken.com/ws/v1` |

Docs: [Futures Introduction](https://docs.kraken.com/exchange/guides/futures/introduction), [REST](https://docs.kraken.com/exchange/guides/futures/rest), [WebSockets](https://docs.kraken.com/exchange/guides/futures/websockets).

**Demo-Setup:**

1. https://demo-futures.kraken.com → Sign Up (E-Mail-Verifikation oft deaktiviert).
2. Settings → Create API Key: General API = **Full Access** für Order-Tests (Read Only für Shadow-only).
3. Public + Private Key **sofort** speichern (Secret nur einmal).
4. Demo-Keys funktionieren **nicht** auf Prod und umgekehrt.
5. Demo kann periodisch resetten / Status prüfen (Support-Hinweise zu Decommissioning historisch — vor Phase-4-Soak Status verifizieren).

### 7.2 REST Authentication (präzise)

Headers:

```
APIKey:  <public key>
Nonce:   <monoton steigend, empfohlen: ms epoch als string>
Authent: <base64(HMAC-SHA512(key=base64decode(secret), data=SHA256(postData+Nonce+endpointPath)))>
```

Algorithmus:

```
message     = postData + Nonce + endpointPath
sha256_bin  = SHA256(message)            // 32 raw bytes
secret_bin  = Base64Decode(api_secret)
hmac_bin    = HMAC_SHA512(secret_bin, sha256_bin)
Authent     = Base64Encode(hmac_bin)
```

- `postData`: form-urlencoded Parameter exakt wie im Request (`orderType=lmt&symbol=PF_XBTUSD&…`), leer `""` wenn keine Params.
- Bevorzuge **URL-encoded** Form (Docs: Migration bis ~Okt 2025; neue Implementierung direkt encoded bauen).
- GET: Params in Query **und** in `postData` für Signatur.
- POST (state-changing): Params im Body (application/x-www-form-urlencoded), gleiche `postData`-String für Signatur.

**OpenSSL-Skizze** (in `kraken_auth.h`, analog `binance_auth.h` aber SHA512+Base64):

```cpp
// 1) SHA256(message) → digest32
// 2) EVP_MAC HMAC with SHA512, key = decoded secret, data = digest32
// 3) Base64 encode 64-byte HMAC → Authent string
```

Unit-Test mit **bekannten Vektoren** aus Docs/Samples (challenge-Beispiel + ein REST-Vektor fixen).

### 7.3 WebSocket Authentication (Challenge)

```
Client → {"event":"challenge","api_key":"<key>"}
Server → {"event":"challenge","message":"<uuid>"}

signed_challenge =
  Base64( HMAC_SHA512( Base64Decode(secret), SHA256(challenge_string) ) )

Client → {
  "event": "subscribe",
  "feed": "fills",
  "api_key": "<key>",
  "original_challenge": "<uuid>",
  "signed_challenge": "<signed>"
}
```

- Challenge einmal pro Connection holen; bei jeder private subscribe/unsubscribe mitsenden.
- Bei Reconnect: **neue** Challenge.
- Public Feeds brauchen **keine** Auth.

Keepalive: Application-Level **ping mindestens alle 60s** (`{"event":"ping"}` bzw. Docs-Sample). Zusätzlich optional Feed `heartbeat`.

### 7.4 Public Feeds

**Subscribe:**

```json
{"event":"subscribe","feed":"trade","product_ids":["PF_XBTUSD"]}
{"event":"subscribe","feed":"book","product_ids":["PF_XBTUSD"]}
{"event":"subscribe","feed":"ticker","product_ids":["PF_XBTUSD"]}
```

| Feed | Snapshot | Update | Engine-Mapping |
|------|----------|--------|----------------|
| `trade` | `trade_snapshot` | `trade` | `provider::tick` |
| `book` | `book_snapshot` | `book` (delta, qty=0 remove) | `l2_snapshot` / `l2_update` |
| `ticker` | ticker | ~1s throttle | optional mid/mark/funding (cold/status) |

**Trade-Felder (typisch):** `product_id`, `price`, `qty`, `side` (`buy`/`sell` = Taker), `time`, `uid`, `seq`, `type` (fill/liquidation/…).

**Book:**

- Snapshot: `bids`/`asks` Arrays `{price, qty}`
- Delta: `side` buy/sell, `price`, `qty` (0 = Level löschen), `seq` — **Gap-Detection** auf `seq` implementieren; bei Gap: Resubscribe/Snapshot fordern (fail-loud in live).

### 7.5 Private Feeds

| Feed | Zweck | Parser-Ziel |
|------|-------|-------------|
| `fills` | Executions | `parsed_exec` (partial/full) |
| `open_orders` | Order lifecycle | ack / canceled / rejected reasons |
| `open_positions` | Position size/entry/liq | `parsed_position_snapshot` |
| `balances` | Multi-M flex wallet | cash/available_margin |

**Direction:** In Orders oft `0=buy`, `1=sell` — **nicht** mit side-strings verwechseln.

**cli_ord_id:** Client Order ID (max ~100 chars) — TrueTest `ClientOrderIdMinter` nutzen (`tt…` Prefix ok, uniqueness global beachten).

### 7.6 Order REST

#### sendorder — `POST /derivatives/api/v3/sendorder`

| Param | Pflicht | Notes |
|-------|---------|-------|
| `orderType` | ja | siehe Mapping unten |
| `symbol` | ja | `PF_XBTUSD` |
| `side` | ja | `buy` / `sell` (lowercase) |
| `size` | ja | Kontraktgröße (PF: Base-Asset-Einheiten) |
| `limitPrice` | cond | lmt/post/ioc/… |
| `stopPrice` | cond | stp / take_profit |
| `cliOrdId` | nein | empfohlen |
| `reduceOnly` | nein | default false; flatten true |
| `triggerSignal` | nein | `mark` / `index` / `last` |

#### cancelorder — `POST …/cancelorder`

- `order_id` **oder** `cliOrdId`

#### editorder — `POST …/editorder` (Phase 3+)

- size / limitPrice / stopPrice

#### cancelallorders — `POST …/cancelallorders`

- optional symbol filter; Cost 25

#### cancelallordersafter (DMS) — `POST …/cancelallordersafter`

- Param: `timeout` in **Sekunden** (uint)
- `timeout=0` → disable
- Empfohlen (Kraken): alle 15–20s mit `timeout=60`
- Response enthält `triggerTime`
- Cost: **25** pro Call

### 7.7 Order-Type Mapping (Engine → Kraken)

Engine (`src/core/event.h`):

```cpp
enum class order_type { market, limit, stop, stop_limit };
enum class time_in_force { ioc, fok, gtc, day };
```

| Engine | TIF | Kraken `orderType` | Notes |
|--------|-----|--------------------|-------|
| `limit` + GTC | gtc | `lmt` | Standard |
| `limit` + post-only Wunsch | — | `post` | wenn Engine/Flag post-only; sonst lmt |
| `limit` + IOC | ioc | `ioc` | |
| `limit` + FOK | fok | `fok` | |
| `market` | — | `mkt` | IOC mit ~1% price protection |
| `stop` | — | `stp` | + `stopPrice`; limitPrice optional (worst) |
| `stop_limit` | — | `stp` + `limitPrice` + `stopPrice` | |

**v1-Minimum:** `lmt`, `mkt`, `ioc`, `post` (optional), `reduceOnly`. Stops können Phase 2.5 folgen (Bracket-Adapter).

Encoder-Output (`IOrderEncoder`):

```cpp
encoded_order e;
e.endpoint     = "/derivatives/api/v3/sendorder"; // HTTP path
// wire_payload = "orderType=lmt&symbol=PF_XBTUSD&side=buy&size=0.01&limitPrice=90000&cliOrdId=tt..."
// Rest client signs with endpointPath="/api/v3/sendorder"
```

### 7.8 Symbol-Schema

| Prefix | Bedeutung | v1 |
|--------|-----------|----|
| `PF_` | Perpetual **linear** multi-collateral | **JA** |
| `PI_` | Perpetual **inverse** single-collateral | nein |
| `FF_` | Fixed maturity linear | nein |
| `FI_` | Fixed maturity inverse | nein |

- **XBT** ist Kraken-Code für Bitcoin — **nicht** `BTC` in Symbolen (`PF_XBTUSD`, nicht `PF_BTCUSD` in älteren Listen; Instruments-Endpoint ist Source of Truth).
- Case: API oft case-insensitive; intern **UPPER** kanonisieren wie Binance-Encoder.
- Instrument-Metadaten: `GET /instruments` (public, cost 0) → `tickSize`, `contractSize`, min sizes, tradeable flag → `instrument_spec`.

### 7.9 Positionen & Margin

- **Net-only:** eine Netto-Position pro Symbol (long/short), kein Hedge.
- Multi-M (`PF_`): Wallet `flex_futures` / FLEX; P&L default USD; Isolated **oder** Cross möglich.
- Inverse (`PI_`): separate Coin-Wallets — v1 ignorieren; bei Symbol-Prefix `PI_` in `open()` **refuse** mit klarer Meldung.
- Flatten: opposite side + `reduceOnly=true` + `orderType=mkt`.

### 7.10 Rate Limits

Budget authenticated `/derivatives`: **500 cost / 10s**.

| Endpoint | Cost |
|----------|------|
| sendorder / cancelorder / editorder | 10 |
| cancelallorders / cancelallordersafter | 25 |
| openpositions / accounts / openorders / fills | 2 |
| instruments (public) | 0 |

WS: max ~100 connections; ~100 requests/s/connection.

**TokenBucket in Provider:** konservativ wie Binance-Futures-Live (`capacity=50`, `refill=5/s`) ist OK; DMS-Heartbeats (cost 25) im Budget einplanen → bei countdown 60s / heartbeat 15–20s ≈ 1.25–1.7 calls/s * 25 ≈ **31–42 cost/s** → zu aggressiv!

**Korrektur DMS-Budget:**

- Kraken-Empfehlung: heartbeat alle **15–20s**, timeout **60s** → cost 25 / 15s ≈ **1.7 cost/s** → ~17/10s, unkritisch.
- **Nicht** alle 1–2s pollen.
- Mapping TrueTest-Config:
  - `dead_man_countdown_ms=60000` → `timeout=60` Sekunden
  - `dead_man_heartbeat_ms=15000` → 15s beat  
  - Default wenn heartbeat unset: `max(countdown/3, 15000)` aber **niemals < 10s** wegen Rate-Limit.

### 7.11 Response-Quirks

- Top-level `"result":"success"` heißt **Request verarbeitet**, nicht zwingend Order live — immer `sendStatus.status` / `orderEvents` prüfen (`placed`, `invalidSize`, `insufficientAvailableFunds`, …).
- Fehler: JSON mit `error` / `errors`; Rate limit: `apiLimitExceeded`.
- Zeiten: oft ISO-8601 Strings auf REST; WS trades oft ms/ISO gemischt — Parser tolerant, intern `system_clock`.

---

## 8. Implementierungsphasen (konkrete Tasks)

### Phase 0 — Scaffold + Public MD (Shadow-fähig ohne Keys)

**Dateien:** endpoints, auth (nur Unit), transport, market_parser, provider skeleton, register, CMake.

**Provider `open()`:**

1. Endpoints aus demo/mainnet wählen.
2. `KrakenTransport` öffnen → subscribe `trade` (+ optional `book`).
3. `supports_event_stream()=true`, `get_event_parser()` → market parser.
4. Execution: Paper/`BinanceExecutor`-Äquivalent — **reuse** bestehende Paper-Execution-Pfade aus Provider (wie Futures-Provider im non-live Branch): `BinanceExecutor`-Pattern heißt hier generischer paper executor den Futures-Provider schon nutzt — **nicht** neu erfinden; schau `BinanceFuturesProvider::open()` non-live Zweig und spiegele Hybrid/Paper.

**DoD Phase 0:**

```bash
./build/engine_shadow --provider kraken-futures --symbol PF_XBTUSD \
  --stream trade --no-tui --status-format off
# Ticks fließen; kein Crash; layer+json checks grün
```

### Phase 1 — REST + Instruments + Reconciler (read)

- `KrakenRestClient`: TLS, signed get/post, nonce clock, redact logs.
- Startup: `GET instruments` → Symbol existiert + tradeable + fill `instrument_spec`.
- `GET accounts` + `GET openpositions` für Reconciler.
- Live/shadow mit Keys: Reconciler wired; bei Drift refuse.

**Auth unit tests Pflicht vor Netzwerk.**

### Phase 2 — Orders + User Data

- `KrakenOrderEncoder` + REST order transport (analog `make_binance_rest_order_transport`).
- `KrakenUserDataTransport`: WS connect → challenge → subscribe `fills` + `open_orders` (+ positions/balances).
- `KrakenUserDataParser` → `IFillParser`.
- `ExecutionBridge` deps wie in `binance_futures_provider.h` live-block (~Z. 394–450).
- cliOrdId über `ClientOrderIdMinter`.

**Demo:** place tiny limit deep in book → cancel via cliOrdId → fills path mit min size.

### Phase 3 — Safety Complete

1. **DMS** `KrakenFuturesDeadMansSwitch`:
   - `post_fn` → `POST cancelallordersafter` mit `timeout=<seconds>`
   - `start()`: initial arm; fail → refuse live
   - heartbeat thread; liveness atomic für `WorkerWatchdog`
   - `disarm()`: `timeout=0`
   - countdown_ms → seconds: `(countdown_ms + 999) / 1000`, min 1 wenn armed
   - optional `close_position_fn` bei persistent heartbeat fail (Phase-3-Parität Binance)

2. **KillSwitch:**
   - `cancelallorders` (symbol-scoped wenn API filter erlaubt, sonst all + verify)
   - `openpositions` lesen → non-zero → `sendorder` mkt reduceOnly opposite
   - Deadline-bounded REST timeouts
   - Loud stderr, no retry loops

3. **Risk:** `FuturesRiskCheck` reuse (`set_max_notional_usdt` etc.) — Mark aus ticker/mid.

4. **open() Live-Gates (refuse closed):**

   - Clock/auth probe (signed `accounts` oder `openpositions`)
   - Symbol tradeable
   - Prefix gate: only `PF_` (v1)
   - Optional margin-mode advisory
   - Bridge open + DMS arm last

5. **close()/halt:** DMS disarm on orderly shutdown **after** kill-switch (oder wie Binance: kill first, disarm after — **dieselbe Ordnung dokumentieren und einhalten**).

### Phase 4 — Ops & Evidence

- `docs/operations/0x-kraken-futures-demo.md` analog `02-futures-testnet.md`
- Demo soak ≥ 4h shadow + kill/DMS drills (SIGINT, SIGKILL, net loss)
- Keine Phase-0-Mainnet-Claims ohne Governance

---

## 9. Parser-Maps (Wire → Engine)

### 9.1 Market: Trade → `provider::tick`

| Kraken | Engine |
|--------|--------|
| `product_id` | `symbol` |
| `time` | `timestamp` |
| `price` | `price` |
| `qty` | `quantity` (scale: double→int64 policy wie Binance qty_scale / config) |
| `side` buy/sell | `side` uint8 |

### 9.2 Market: Book

| Kraken | Engine |
|--------|--------|
| `book_snapshot` bids/asks | `l2_snapshot` |
| delta `side`+`price`+`qty` | `l2_update` (`new_quantity`, 0=delete) |
| `seq` | intern gap check; nicht zwingend im event |

### 9.3 User: fills → `parsed_exec`

| Kraken fill | parsed_exec |
|-------------|-------------|
| `cli_ord_id` | `client_order_id` |
| `order_id` | `exchange_order_id` |
| `instrument` | `symbol` |
| `buy` bool | `side` |
| `qty` | `last_fill_qty` |
| `price` | `last_fill_price` |
| `fee_paid` / `fee_currency` | `commission` / `commission_asset` |
| remaining 0 vs >0 | `full_fill` / `partial_fill` |
| `time` | `ts` |

### 9.4 User: open_orders → ack/cancel

| Event | kind |
|-------|------|
| new order delta, not cancel | `ack` |
| `is_cancel=true` reason cancelled_by_user/… | `canceled` |
| reason not_enough_margin / reject | `rejected` + `error` |
| full_fill (orders feed) | oft redundant zu fills — idempotent handhaben |

### 9.5 Positions / Balances → `parsed_position_snapshot`

| Quelle | Mapping |
|--------|---------|
| `open_positions` size/balance | `position_row.qty` signed |
| `balances.flex_futures.available_margin` | cash proxy für Reconciler |
| funding fields | `reason::funding_fee` wenn erkennbar |

**Kein nlohmann:** `kraken_json_util.h` mit `find_key` / `extract_sv_string` / `extract_number` — Copy-Paste-Adaption aus `binance_parser.h` detail-namespace, Keys anpassen.

---

## 10. DMS — `cancelallordersafter` (1:1 Pattern)

Gold-Referenz: `binance_futures_dead_mans_switch.h`.

```text
Arm:     POST /derivatives/api/v3/cancelallordersafter  timeout=60
Beat:    same every heartbeat_interval
Disarm:  timeout=0
Fail:    two consecutive post fails → log loud; optional close_position_fn;
         do NOT update liveness_ts → WorkerWatchdog halts
```

**Unterschiede zu Binance:**

| | Binance | Kraken |
|--|---------|--------|
| Einheit | Millisekunden `countdownTime` | **Sekunden** `timeout` |
| Scope | oft symbol-param | **alle** open orders (account) |
| Endpoint cost | weight-based | **25** fixed |

**Impl-Detail:** Interne Config bleibt `*_ms` (CLI-Stabilität); Konversion nur an der HTTP-Grenze. Kommentare **fett** an die Einheiten-Falle.

**Layered Safety (Copy aus Binance-Kommentar, anpassen):**

- Orderly: Kill-Switch flatten + cancel → DMS disarm
- Catastrophic: Prozess tot → Venue cancels orders nach timeout; Positionen bleiben → Operator/SOP
- DMS cancelt **Orders**, nicht zwingend Positionen — Flatten ist Kill-Switch / optional DMS-close-fn

---

## 11. Kill-Switch

Gold: `binance_futures_kill_switch.h`.

```text
1) POST cancelallorders
2) GET  openpositions  → size for PF_XBTUSD
3) if |size| > eps:
     POST sendorder orderType=mkt side=opposite size=abs reduceOnly=true cliOrdId=mint
4) deadline checks between steps; per-call REST timeout
```

Fehler laut loggen, `false` returnen — **kein** Retry-Backoff.

---

## 12. Reconciler

Gold: `binance_futures_reconciler.h`.

```text
GET openpositions → venue_qty for symbol
GET accounts      → available margin (flex_futures / multi-collateral field path)
Compare local portfolio cash & position within tolerance_bps
Any HTTP/parse fail → non-empty error string (refuse start)
Demo: NO "testnet wipe" heuristic (wie Futures-Binance: kein Spot-Reset-Shortcut)
```

Field extraction als `static` Methoden für Unit-Tests mit canned JSON.

---

## 13. Provider `open()` Live-Checkliste (Copy-Target)

Reihenfolge analog `BinanceFuturesProvider` live-block:

1. FuturesRiskCheck construct if caps set  
2. Transport open (market WS) + halt_cb  
3. RestClient + auth probe  
4. instruments probe (symbol)  
5. PF_-only gate; refuse PI_/FI_/…  
6. Optional margin/liq advisories  
7. minter + rate limiter  
8. reconciler + kill_switch (+ bracket later)  
9. ExecutionBridge: order_tx, user_data_tx, encoder, fill parser, position_snapshot_handler  
10. bridge->open()  
11. **DMS start last** — fail → lifecycle::error, return false  
12. Register liveness_sources: user-data thread + DMS heartbeat  

`name()` → `"kraken-futures"`.

---

## 14. Tests — Pflichtmatrix

| Test | Inhalt |
|------|--------|
| `test_kraken_auth` | REST Authent known vector; WS challenge sign; empty secret fails closed |
| `test_kraken_order_encoder` | lmt/mkt/ioc/reduceOnly/cliOrdId; cancel by order_id & cliOrdId; prefix PF |
| `test_kraken_market_parser` | trade_snapshot, trade delta, book_snapshot, book delete qty=0 |
| `test_kraken_user_data_parser` | fills partial/full; open_orders cancel; position snapshot |
| `test_kraken_futures_register` | missing symbol throws; demo flag selects endpoints |
| `test_kraken_futures_reconciler` | canned match/mismatch/HTTP fail |
| `test_kraken_futures_kill_switch` | fake rest: cancel+flatten order of calls; reduceOnly asserted |
| `test_kraken_futures_dead_mans_switch` | arm/disarm payload timeout units; heartbeat fail silence |
| demo live (optional) | `#ifdef` network; skip if no env keys |

Inject `post_fn` / fake clients wie Binance — **keine** echten Sockets in Default-CI.

---

## 15. Definition of Done (Kraken-Provider)

- [ ] `ENABLE_KRAKEN=ON` baut `engine_shadow` / `engine_live` / tests  
- [ ] Registry: `kraken-futures` listbar  
- [ ] Phase 0: public trades auf `PF_XBTUSD`  
- [ ] Auth unit-tested; signing path `/api/v3/...` dokumentiert im Code  
- [ ] Reconciler refuse on drift  
- [ ] Kill-switch cancel+flatten reduceOnly  
- [ ] DMS seconds mapping + disarm 0 + liveness source  
- [ ] Challenge WS private fills path  
- [ ] PI_/non-PF refuse in v1  
- [ ] XBT-Symbole; kein stilles BTC-Rewrite  
- [ ] `./scripts/check-hotpath-json.sh`  
- [ ] `./scripts/check-layer-deps.sh`  
- [ ] `./scripts/check-live-safety-freeze.sh` (Binance-Freeze unberührt)  
- [ ] Keine Venue-Ifdefs in core/engine  
- [ ] Hot-path: kein nlohmann in kraken parsers  
- [ ] Docs: kurzer Ops-Demo-Guide  
- [ ] Liquiditätshinweis in Ops: Alts dünner als Binance — Size-Limits enger wählen  

---

## 16. Pitfalls (Agent-Checkliste)

| # | Falle | Mitigation |
|---|-------|------------|
| 1 | **XBT vs BTC** | Instruments-Endpoint; Defaults `PF_XBTUSD`; nie `BTCUSDT` annehmen |
| 2 | **PF vs PI** | v1 nur `PF_`; PI = inverse, andere Wallet/PnL-Mathe |
| 3 | **Signing path** | URL hat `/derivatives`, Signatur `/api/v3/...` |
| 4 | **Secret encoding** | Base64-**decode** vor HMAC; nicht raw string wie Binance secret |
| 5 | **HMAC-SHA512 o. SHA256** | SHA256 nur über Message; HMAC ist SHA512 |
| 6 | **Authent vs Binance signature** | Header `Authent` + `APIKey`, nicht query `signature` |
| 7 | **DMS Einheiten** | Kraken Sekunden; CLI ms → convert |
| 8 | **DMS scope** | Account-weit alle Orders — multi-symbol Vorsicht |
| 9 | **Challenge WS** | Jeder Reconnect neue Challenge; private feeds ohne Signatur = silent fail |
| 10 | **result=success** | Order-Status im Nested-Object prüfen |
| 11 | **side encoding** | REST: `buy`/`sell`; manchen WS orders: `0`/`1` |
| 12 | **mkt vs ioc** | `mkt` hat 1% protection; reines IOC = `ioc` |
| 13 | **Rate limit DMS** | Nicht sub-10s heartbeats |
| 14 | **Demo ≠ Prod keys** | Getrennte Hosts + Keys |
| 15 | **Liquidity** | EU-Fit stark; Orderbuch-Tiefe Alts << Binance → kleinere Caps, aggressivere min-liq-distance |
| 16 | **Hot-path JSON** | Kein nlohmann; CI-Script allow-list nicht erweitern |
| 17 | **Qty scale** | PF contractSize oft 1 base unit — double fine, aber int64 tick path konsistent halten |
| 18 | **Nonce** | Monoton; bei Auth-Fails neuen Key/Nonce-Reset bedenken |
| 19 | **postData encoding** | Exakt body/query string signieren; UTF-8 |
| 20 | **Freeze** | Binance Safety-Dateien nicht „refactored“ während Kraken-Port |

---

## 17. Konkrete Code-Anker (lesen vor dem Schreiben)

| Thema | Datei |
|-------|--------|
| IProvider Vertrag | `src/providers/provider.h` |
| Registry Makro | `src/providers/provider_registry.h` |
| Live Safety Ifaces | `src/execution/live_safety.h` |
| Fill/Position Parse | `src/execution/fill_parser.h` |
| Order Encoder Iface | `src/execution/order_encoder.h` |
| Futures Risk | `src/risk/futures_risk_check.h` |
| Instrument Spec | `src/execution/instrument.h` |
| Gold Provider | `src/providers/binance/binance_futures_provider.h` |
| Gold Register | `src/providers/binance/binance_futures_register.cpp` |
| Gold DMS | `src/providers/binance/binance_futures_dead_mans_switch.h` |
| Gold Kill | `src/providers/binance/binance_futures_kill_switch.h` |
| Gold Reconciler | `src/providers/binance/binance_futures_reconciler.h` |
| Gold Encoder | `src/providers/binance/binance_futures_order_encoder.h` |
| REST Client Pattern | `src/providers/binance/binance_rest_client.h` |
| Auth Pattern | `src/providers/binance/binance_auth.h` |
| CMake wire | `cmake/Dependencies.cmake` (`tt_wire_optional_backends`) |
| Sources list | `cmake/Sources.cmake` |
| CLI provider_config | `src/bin/main.inc` (~1558+) |
| Layer rules | `scripts/check-layer-deps.sh` |
| Agent rules | `AGENTS.md` |
| Ops analog | `docs/operations/02-futures-testnet.md` |

---

## 18. Beispiel-Kommandos (Zielzustand)

```bash
# Build
cmake -B build -DENABLE_KRAKEN=ON -DENABLE_LIVE_DATA=ON -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"

# Shadow public data (keine Keys)
./build/engine_shadow \
  --provider kraken-futures \
  --symbol PF_XBTUSD \
  --stream trade \
  --no-pin --status-format off --no-tui

# Demo shadow/live-wiring (Keys)
export KRAKEN_FUTURES_DEMO_KEY=...
export KRAKEN_FUTURES_DEMO_SECRET=...

./build/engine_live \
  --provider kraken-futures \
  --symbol PF_XBTUSD \
  --stream trade \
  --testnet \
  --live \
  --api-key "$KRAKEN_FUTURES_DEMO_KEY" \
  --api-secret "$KRAKEN_FUTURES_DEMO_SECRET" \
  --dead-man-countdown-ms 60000 \
  --dead-man-heartbeat-ms 15000 \
  --max-notional 150 \
  --max-leverage 2.5 \
  --min-liq-distance-pct 0.07 \
  --reconcile-tolerance-bps 3
```

---

## 19. Referenzen (offiziell)

- Introduction: https://docs.kraken.com/exchange/guides/futures/introduction  
- REST Auth: https://docs.kraken.com/exchange/guides/futures/rest  
- WebSockets: https://docs.kraken.com/exchange/guides/futures/websockets  
- Rate limits: https://docs.kraken.com/exchange/guides/futures/ratelimits  
- Dead man’s switch: https://docs.kraken.com/api-reference/order-management/dead-mans-switch  
- Send order: https://docs.kraken.com/api-reference/order-management/send-order  
- Cancel all: https://docs.kraken.com/api-reference/order-management/cancel-all-orders  
- Instruments: https://docs.kraken.com/api-reference/instrument-details/get-instruments  
- Ticker symbols (PF/PI): https://support.kraken.com/articles/360022835891-ticker-symbols-derivatives  
- Demo env: https://support.kraken.com/articles/360024809011-api-testing-environment-derivatives  
- Sample clients: Kraken GitHub / historische CryptoFacilities REST-v3 Samples  

---

## 20. Agent-Arbeitsanweisung (kurz)

1. **Nicht** `engine.cpp` / Binance-Freeze anfassen.  
2. Phase 0 scaffold mirror Binance file set under `src/providers/kraken/`.  
3. Auth + seconds-DMS + challenge-WS zuerst unit-testen.  
4. PF_ linear only; refuse everything else.  
5. Nach jedem `src/`-Edit: drei Check-Scripts.  
6. Safety loud, fail-closed, no helpful retries.  
7. Liquidität EU-Vorteil, Depth-Nachteil → konservative Caps.  

**Kanonischer Name in Logs und `name()`:** `kraken-futures`.  
**Registry:** `"kraken-futures"` (+ optional Alias `"kraken"`).  
**Pfad:** `src/providers/kraken/`.

---

*Ende des Implementation Guides — für Grok Build Agents. Commit-Ziel: `docs/upcoming_platform/kraken.md`.*
