# Gate.io Futures Provider — Grok Build Implementation Guide

> **Ziel-Datei:** `core/docs/upcoming_platform/gateio.md`  
> **Registry-Namen:** `"gate"` (Alias) / `"gate-futures"` (kanonisch)  
> **Code-Pfad:** `src/providers/gate/`  
> **Golden Reference:** `src/providers/binance/*` (USDT-M Futures-Stack)  
> **Stand der API-Recherche:** 2026-07-29 (Gate APIv4 + Futures WS v4)

Dieses Dokument ist die **handlungsfähige Bauanleitung** für Grok-Build-Agenten. Es spiegelt die Binance-Futures-Integration 1:1 in Architektur und Safety-Kultur, mappt aber alle Venue-Details auf Gate.io (Gate.com).

---

## 1. Ziele

| Priorität | Ziel |
|-----------|------|
| P0 | USDT-margined Perpetuals (`settle=usdt`) als `IProvider` mit Market-Data + Paper/Shadow-Execution |
| P1 | Live-Order-Routing hinter `TT_TARGET=LIVE` + Safety-Stack (Reconciler, Kill-Switch, DMS, Risk-Check) |
| P2 | Testnet-Drills + Shadow-Mainnet-Observability |
| Nicht-Ziel (v1) | Inverse BTC-settle, Delivery/Options, WS-Order-API als Primary, Hedge/`dual_mode`, Multi-Symbol-Cross-Margin |

**Erfolgskriterium v1:**  
`--provider gate-futures --symbol BTC_USDT` läuft in `engine_shadow` und `engine_backtest`-nahen Streaming-Pfaden; Live nur in `engine_live` mit armiertem DMS und Reconciler-Refusal-Default.

---

## 2. Non-Negotiables (TrueTest + Gate)

Diese Regeln sind **nicht verhandelbar**. Verstöße → PR ablehnen.

### 2.1 Engine / Safety (aus `AGENTS.md`)

| ID | Regel |
|----|-------|
| S1 | Provider ist der **einzige** Venue-Extension-Point (`IProvider` + Safety-Hooks). Kein `HAS_GATE` in `engine/`, `risk/` (generisch), `threading/`. |
| S2 | Live-Orders nur wenn `target_allows_live_orders()` (constexpr) — kein Runtime-Bypass. |
| S3 | Halt ist **write-once / terminal**. Kein Auto-Resume, kein Retry-Backoff auf Kill/DMS/Reconciler. |
| S4 | DMS countdown/heartbeat **fixed conservative** — kein adaptive Lengthening. Gate: `timeout` in **Sekunden**, **min 5**. |
| S5 | Reconciler **default-refuse**. User-data-Stream ist Source of Truth; REST advisory bis reconciled. |
| S6 | Pre-trade: Venue-`IRiskCheck` vor generischem `RiskManager` (Futures-Hotpath). |
| R1–R10 | Hot path zero-heap, SPSC sole-producer, kein JSON/nlohmann auf Hotpath, `forbid_runtime_grow`. |
| Layer | `scripts/check-layer-deps.sh` muss grün bleiben — Provider darf Engine nicht „nach oben“ includen außer erlaubten Types. |

### 2.2 Gate-spezifische Non-Negotiables (v1)

| ID | Regel |
|----|-------|
| G1 | Nur `settle=usdt` (linear). Kein `btc`-settle in v1. |
| G2 | Contract-Naming: `BTC_USDT` (Unterstrich, Uppercase) — **nicht** `BTCUSDT`. |
| G3 | Order-`size` ist **signed**: `>0` = buy/long, `<0` = sell/short. Kein separates `side`-Feld wie bei Binance. |
| G4 | **One-way / single** Position Mode only. `in_dual_mode==true` → `open()` refuses. |
| G5 | Client-Order-ID im Feld `text`, Prefix `t-`, Zeichenklasse streng (siehe §9). |
| G6 | REST-Auth: **HMAC-SHA512** (nicht SHA256 wie Binance). Timestamp in **Sekunden**. |
| G7 | Private WS-Subs brauchen oft `user_id` (UID) im Payload — Config-Pflicht für Live. |
| G8 | DMS: `POST /futures/{settle}/countdown_cancel_all` mit `timeout` ≥ 5 s (oder 0 = disarm). |
| G9 | `X-Gate-Size-Decimal: 1` setzen (REST + WS), sobald Decimal-Sizes relevant; Parser müssen String-**und** Int-Sizes lesen. |
| G10 | Keine Soft-Fail-Greenwash: Auth/Signatur/Clock-Skew/Contract-Probe → refuse `open()`. |

---

## 3. Architektur-Map (TrueTest ↔ Gate)

```
┌─────────────────────────────────────────────────────────────────┐
│ engine (composition root)                                       │
│  - configure(provider) → open() → get_transport / get_execution │
│  - get_reconciler / get_kill_switch / get_risk_check / brackets │
│  - WorkerWatchdog ← provider.get_liveness_sources()             │
└────────────────────────────┬────────────────────────────────────┘
                             │ IProvider
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│ GateFuturesProvider  ("gate-futures")                           │
│  open(): endpoints, clock skew, contract probe, dual_mode gate, │
│          risk_check, ExecutionBridge, DMS arm                   │
└───┬──────────────┬──────────────┬──────────────┬────────────────┘
    │              │              │              │
    ▼              ▼              ▼              ▼
 Market WS     REST Client    User-Data WS   Safety
 GateTransport GateRestClient GateUserData   Reconciler
 GateCombined  (HMAC-SHA512)  Transport      KillSwitch
 Parser        OrderEncoder   FillParser     DeadMansSwitch
               OrderTransport                FuturesRiskCheck
               BracketAdapter                  (reuse generic)
```

### 3.1 Core-Interfaces (nicht neu erfinden)

| Interface | Datei | Gate-Impl |
|-----------|-------|-----------|
| `IProvider` | `src/providers/provider.h` | `GateFuturesProvider` |
| `IDataTransport` | `src/providers/transport.h` | `GateTransport`, `GateCombinedTransport`, `GateUserDataTransport` |
| `IDataParser<provider::event>` | `src/providers/parser.h` | `GateCombinedParser` |
| `IOrderEncoder` | `src/execution/order_encoder.h` | `GateFuturesOrderEncoder` |
| `IOrderTransport` | `src/execution/order_transport.h` | `GateRestOrderTransport` |
| `IFillParser` | `src/execution/fill_parser.h` | `GateFuturesUserDataParser` |
| `IReconciler` / `IKillSwitch` | `src/execution/live_safety.h` | `GateFuturesReconciler`, `GateFuturesKillSwitch` |
| `IRiskCheck` | `src/risk/futures_risk_check.h` | **Reuse** `FuturesRiskCheck` (venue-agnostisch) |
| `IBracketAdapter` | `src/exits/bracket_adapter.h` | `GateFuturesBracketAdapter` (Phase live+) |

### 3.2 Binance-Golden → Gate-Mapping

| Binance (Referenz) | Gate (neu) | Hinweis |
|--------------------|------------|---------|
| `BinanceFuturesProvider` | `GateFuturesProvider` | gleiche `open()`-Phasenlogik |
| `BinanceTransport` / Combined | `GateTransport` / Combined | Subscribe-JSON statt URL-Stream |
| `BinanceRestClient` HMAC-SHA256 | `GateRestClient` HMAC-SHA512 | Signatur-String **komplett anders** |
| `/fapi/v1/order` query form | `POST /futures/usdt/orders` **JSON body** | Encoder erzeugt JSON |
| `side=BUY/SELL` + `quantity` | signed `size` | G3 |
| `newClientOrderId` | `text` mit `t-` | G5 |
| `listenKey` User-Data | Private WS channels + auth | kein listenKey |
| `countdownCancelAll` ms | `countdown_cancel_all` **seconds** ≥5 | G8 |
| `positionSide/dual` refuse | `accounts.in_dual_mode` refuse | G4 |
| `fstream` + `fapi` hosts | `fx-ws` + `api.gateio.ws` / `fx-api` | siehe §5 |

---

## 4. Dateibaum (konkret)

Alles unter `src/providers/gate/`. Header-only wo möglich (wie Binance), `.cpp` nur für Register und ggf. schwere Transport-Bodies.

```
src/providers/gate/
├── gate_auth.h                      # HMAC-SHA512 REST + WS sign helpers
├── gate_endpoints.h                 # mainnet/testnet hosts + settle paths
├── gate_parser.h                    # low-level JSON field extract (no nlohmann)
├── gate_json_util.h                 # brace-walk, string/number dual parse
├── gate_rest_client.h               # Boost.Beast TLS REST + sign + clock
├── gate_time_sync.h                 # /spot/time skew check
├── gate_transport.h                 # public market WS
├── gate_combined_transport.h        # multi-channel on one socket
├── gate_combined_parser.h           # trades / order_book_update / candlesticks → provider::event
├── gate_depth_sync.h                # REST snapshot + U/u gap recovery
├── gate_order_encoder.h             # IOrderEncoder (JSON body)
├── gate_rest_order_transport.h      # IOrderTransport POST/DELETE
├── gate_user_data_transport.h       # private WS (orders, usertrades, positions, balances)
├── gate_user_data_parser.h          # IFillParser + position snapshots
├── gate_futures_provider.h          # IProvider composition root
├── gate_futures_register.cpp        # REGISTER_PROVIDER("gate-futures") + "gate"
├── gate_futures_reconciler.h        # IReconciler (LIVE-SAFETY surface)
├── gate_futures_kill_switch.h       # IKillSwitch (LIVE-SAFETY)
├── gate_futures_dead_mans_switch.h  # countdown_cancel_all + heartbeat
├── gate_futures_bracket_adapter.h   # optional Phase live+
├── gate_futures_safety.h            # advisories (margin / liq distance)
├── gate_backfill.h                  # REST candlesticks → prepend bars
└── gate_recorder.h                  # optional cold-path dump (nicht Hotpath)
```

**Tests** (explizit in `cmake/Sources.cmake`):

```
tests/test_gate_auth.cpp
tests/test_gate_endpoints.cpp
tests/test_gate_parser.cpp
tests/test_gate_combined_parser.cpp
tests/test_gate_order_encoder.cpp
tests/test_gate_rest_order_transport.cpp
tests/test_gate_user_data_parser.cpp
tests/test_gate_futures_register.cpp
tests/test_gate_futures_reconciler.cpp
tests/test_gate_futures_kill_switch.cpp
tests/test_gate_futures_dead_mans_switch.cpp
tests/test_gate_futures_safety.cpp
tests/test_gate_futures_testnet_live.cpp   # opt-in, env-gated
```

---

## 5. Endpoints & URLs

### 5.1 `gate_endpoints.h` (Soll-API)

```cpp
namespace gate {

enum class settle_ccy { usdt /*, btc v2 */ };

struct endpoints {
    std::string ws_host;      // e.g. fx-ws.gateio.ws
    std::string ws_port;      // 443
    std::string ws_path;      // /v4/ws/usdt
    std::string rest_host;    // api.gateio.ws  (or fx-api.gateio.ws)
    std::string rest_port;    // 443
    std::string rest_prefix;  // /api/v4
    settle_ccy  settle = settle_ccy::usdt;
    bool is_testnet = false;
};

// Mainnet USDT-M
inline endpoints usdt_mainnet() {
    return {
        "fx-ws.gateio.ws", "443", "/v4/ws/usdt",
        "api.gateio.ws",   "443", "/api/v4",
        settle_ccy::usdt, false
    };
    // REST alternative host: fx-api.gateio.ws (futures-only traffic)
}

// Testnet USDT-M (verify at implement time; both families exist in docs)
inline endpoints usdt_testnet() {
    return {
        "fx-ws-testnet.gateio.ws", "443", "/v4/ws/usdt",
        // alt docs: wss://ws-testnet.gate.com/v4/ws/futures/usdt
        "api-testnet.gateapi.io",  "443", "/api/v4",
        // alt: fx-api-testnet.gateio.ws
        settle_ccy::usdt, true
    };
}

inline std::string settle_str(const endpoints& e) {
    return e.settle == settle_ccy::usdt ? "usdt" : "btc";
}

// Path helpers (REST path WITHOUT host; WITH /api/v4 for signing URL)
inline std::string futures_path(const endpoints& e, std::string_view tail) {
    // tail e.g. "/orders" → "/api/v4/futures/usdt/orders"
    return e.rest_prefix + "/futures/" + settle_str(e) + std::string(tail);
}

} // namespace gate
```

### 5.2 REST — kanonische Pfade (`settle=usdt`)

| Zweck | Method | Path (nach Prefix) |
|-------|--------|--------------------|
| Server time | GET | `/spot/time` → `{ "server_time": <ms> }` |
| Contracts | GET | `/futures/usdt/contracts` / `.../contracts/BTC_USDT` |
| Order book snapshot | GET | `/futures/usdt/order_book?contract=BTC_USDT&limit=100&with_id=true` |
| Trades history | GET | `/futures/usdt/trades?contract=BTC_USDT` |
| Candlesticks | GET | `/futures/usdt/candlesticks?contract=BTC_USDT&interval=1m` |
| Account | GET | `/futures/usdt/accounts` → `available`, `in_dual_mode`, … |
| Positions | GET | `/futures/usdt/positions` / `.../positions/BTC_USDT` |
| Place order | POST | `/futures/usdt/orders` JSON body |
| Get order | GET | `/futures/usdt/orders/{order_id}` (oder by `text`) |
| Cancel one | DELETE | `/futures/usdt/orders/{order_id}` |
| Cancel all | DELETE | `/futures/usdt/orders?contract=BTC_USDT` |
| Amend | PUT/POST | docs: order amend endpoint (implement after place/cancel) |
| My trades | GET | `/futures/usdt/my_trades` |
| **DMS** | POST | `/futures/usdt/countdown_cancel_all` |
| Dual mode (legacy) | POST | `/futures/usdt/dual_mode` |
| Position mode (neu) | POST | `/futures/usdt/set_position_mode` |

### 5.3 WebSocket

| Env | URL |
|-----|-----|
| Mainnet USDT | `wss://fx-ws.gateio.ws/v4/ws/usdt` |
| Testnet USDT | `wss://fx-ws-testnet.gateio.ws/v4/ws/usdt` (oder aktuelle Docs-Variante) |
| SBE (optional v2) | `.../usdt/sbe` — **nicht** in v1 |

**Public channels (v1):**

| Channel | Payload-Beispiel | → `provider::event` |
|---------|------------------|---------------------|
| `futures.trades` | `["BTC_USDT"]` | `provider::tick` |
| `futures.order_book_update` | `["BTC_USDT","100ms","100"]` | `l2_snapshot` / `l2_update` |
| `futures.candlesticks` | `["1m","BTC_USDT"]` | `provider::bar` (optional) |

**Private channels (live):**

| Channel | Payload | Zweck |
|---------|---------|-------|
| `futures.orders` | `["USER_ID","BTC_USDT"]` oder `!all` | Order lifecycle → `parsed_exec` |
| `futures.usertrades` | `["USER_ID","BTC_USDT"]` | Fills |
| `futures.positions` | `["USER_ID","BTC_USDT"]` | Position snapshots |
| `futures.balances` | `["USER_ID"]` | Balance / funding-like |

WS-Request-Rahmen:

```json
{
  "time": 1710000000,
  "channel": "futures.trades",
  "event": "subscribe",
  "payload": ["BTC_USDT"]
}
```

Private + Auth:

```json
{
  "time": 1710000000,
  "channel": "futures.orders",
  "event": "subscribe",
  "payload": ["12345678", "BTC_USDT"],
  "auth": {
    "method": "api_key",
    "KEY": "<api_key>",
    "SIGN": "<hmac_sha512_hex>"
  }
}
```

WS-Sign-String:

```
channel=<channel>&event=<event>&time=<time>
```

HMAC-SHA512(secret, string) → hex.

Ping: `futures.ping` / `futures.pong` + TCP keepalive analog BinanceTransport.

---

## 6. Auth (`gate_auth.h`)

### 6.1 REST HMAC-SHA512

Signatur-String (exakt, Newlines):

```
METHOD\n
URL_PATH\n
QUERY_STRING\n
HEX(SHA512(body))\n
TIMESTAMP
```

- `METHOD`: `GET` / `POST` / `DELETE` / `PUT` (uppercase)
- `URL_PATH`: z.B. `/api/v4/futures/usdt/orders` (**mit** `/api/v4`, **ohne** Host)
- `QUERY_STRING`: raw query ohne `?`; leer `""`
- `body`: raw JSON bytes; leer → SHA512("")  
  Known empty digest:  
  `cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e`
- `TIMESTAMP`: Unix **seconds** (string)

Headers:

```
KEY: <api_key>
Timestamp: <seconds>
SIGN: <hmac_sha512_hex>
Accept: application/json
Content-Type: application/json   // bei Body
X-Gate-Size-Decimal: 1           // empfohlen
```

Implementierung: OpenSSL `EVP_MAC` mit digest `SHA512` (analog `HmacSha256Signer` in `binance_auth.h`, aber SHA512 + 64-Byte Digest → 128 Hex-Chars). Zusätzlich `sha512_hex(body)` für Payload-Hash.

### 6.2 Clock skew

- Endpoint: `GET /api/v4/spot/time` → `server_time` in **ms**
- Gate erlaubt typisch ~15 min Drift; TrueTest soll **enger** sein (wie Binance-Check, z.B. refuse > 1000–2000 ms skew nach Offset-Korrektur)
- `GateRestClient` speichert `server_offset_ms_` und setzt `Timestamp = now_s + offset`

### 6.3 Keine Secrets im Log

`gate::redact_for_log(body, n)` analog Binance — Keys, SIGN, partial secrets strippen.

---

## 7. CMake / Feature-Flag / Registry

### 7.1 CMake

In `CMakeLists.txt`:

```cmake
option(ENABLE_GATE "Build with Gate.io futures provider" OFF)
```

In `cmake/Dependencies.cmake` (neben Binance-Block):

```cmake
if(ENABLE_GATE)
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_sources(${target} PRIVATE
        ${_src}/providers/gate/gate_futures_register.cpp)
    target_link_libraries(${target} PUBLIC
        Boost::headers OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(${target} PUBLIC HAS_GATE)
endif()
```

In `cmake/Sources.cmake` → `TEST_SOURCES` alle `tests/test_gate_*.cpp` anhängen (explizit, **kein GLOB**).

**Alle** Gate-Header mit:

```cpp
#pragma once
#ifdef HAS_GATE
// ...
#endif
```

### 7.2 Register (`gate_futures_register.cpp`)

```cpp
#ifdef HAS_GATE
#include "providers/provider_registry.h"
#include "providers/gate/gate_futures_provider.h"
#include "providers/gate/gate_endpoints.h"

static std::shared_ptr<IProvider> make_gate(const provider_config& cfg) {
    auto get = [&](const std::string& k) -> std::string {
        auto it = cfg.find(k); return it == cfg.end() ? "" : it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error(
            "gate-futures requires 'symbol' (e.g. BTC_USDT)");

    // Normalize: allow btcusdt → BTC_USDT helper (optional, strict preferred)
    const bool testnet =
        get("testnet") == "1" || get("testnet") == "true" ||
        gate::looks_like_testnet_host(get("host"));

    auto ep = testnet ? gate::usdt_testnet() : gate::usdt_mainnet();
    if (!get("host").empty()) ep.ws_host = get("host");
    if (!get("port").empty()) ep.ws_port = get("port");
    if (!get("rest_host").empty()) ep.rest_host = get("rest_host");

    auto p = std::make_shared<GateFuturesProvider>(
        symbol,
        get("stream").empty() ? "trades" : get("stream"),
        get("api_key"), get("api_secret"),
        get("user_id"),   // REQUIRED for private WS in live
        ep);

    // depth: "100ms:100" or config keys depth_interval / depth_level
    if (!get("depth_stream").empty())
        p->set_depth_spec(get("depth_stream"));

    // risk + DMS mirrors binance-futures keys
    // max_notional_usdt, max_leverage, min_liquidation_distance_pct,
    // dead_man_countdown_ms, dead_man_heartbeat_ms,
    // dms_attempt_position_close, margin_type, margin_type_strict, ...

    return p;
}

REGISTER_PROVIDER("gate-futures", make_gate);
REGISTER_PROVIDER("gate", make_gate);  // alias
#endif
```

### 7.3 CLI-Wiring (`main.inc`) — **minimal, phased**

Phase data: `--provider gate-futures` muss über generische `ProviderRegistry::create` laufen (tut es bereits).  
Phase live: bestehende Flags (`--api-key`, `--testnet`, DMS, risk) **provider-agnostisch** machen, wo sie heute `"binance-futures only"` hardcoden — **ohne** Venue-Logik in den Engine-Core zu ziehen. Prefer:

```text
provider_config keys from CLI → registry factory
```

Nicht: `if (provider == "gate-futures") { ... special engine path }`.

---

## 8. Implementierungs-Phasen (PR-Schnitt)

Jeder PR muss: `check-hotpath-json.sh`, `check-layer-deps.sh`, `check-live-safety-freeze.sh`, relevante Tests grün.

### Phase 0 — Scaffold (kein Netzwerk nötig für Unit-Tests)

1. `gate_endpoints.h`, `gate_auth.h`, `gate_parser.h` / `gate_json_util.h`
2. Unit-Tests: Signatur-Vektoren (fixture mit known key/secret/body), empty-body SHA512, endpoints
3. `ENABLE_GATE` + `HAS_GATE` CMake
4. Empty `GateFuturesProvider` mit `name()=="gate-futures"`, `has_data_feed/execution`, Registry

**DoD P0:** `ctest -R gate` grün offline; Binary linkt mit `-DENABLE_GATE=ON`.

### Phase 1 — Market Data (Shadow/Paper-tauglich)

1. `GateTransport`: connect WS, subscribe `futures.trades`, `read_frame` liefert raw JSON lines
2. `GateCombinedTransport`: trades + `futures.order_book_update` auf **einem** Socket
3. `GateCombinedParser` → `provider::event` (`tick`, `l2_*`, optional `bar`)
4. `GateBackfill` via REST candlesticks + `PrependTransport` (wie Binance)
5. `GateFuturesProvider::open()` für non-live: transport + `HybridExecutor` / `TradeTapeShadowAdapter`
6. Shadow-Smoke: `engine_shadow --provider gate-futures --symbol BTC_USDT ...`

**DoD P1:** Live-Ticks parsen; L2-Gap-Recovery mit REST `with_id=true`; keine Secrets nötig.

### Phase 2 — REST Client + Instrument Spec

1. `GateRestClient` (get/post/del, signed + unsigned, TLS session cache pattern von Binance)
2. `gate_time_sync.h` skew refuse
3. `get_instrument()` aus `GET .../contracts/BTC_USDT`:
   - `order_price_round` → `tick_size`
   - `order_size_min` / step → `min_qty` / `lot_size`
   - `quanto_multiplier` speichern (intern) für Notional:  
     `notional ≈ |size| * quanto_multiplier * mark_price` (USDT linear)
4. Contract-Probe in `open()`: unbekannter Contract → refuse

**DoD P2:** Instrument-Cache befüllt; Notional-Rechnung in Risk-Check stimmig.

### Phase 3 — Live Safety Stack (noch ohne echte Orders ok)

1. `GateFuturesReconciler`: cash = `accounts.available`, pos = `positions.size` (signed)
2. `GateFuturesKillSwitch`: `DELETE .../orders?contract=` dann reduce-only close (signed size opposite)
3. `GateFuturesDeadMansSwitch`: `countdown_cancel_all`, timeout **seconds**, min 5; heartbeat thread; `WorkerWatchdog` liveness
4. dual_mode gate: `accounts.in_dual_mode == true` → refuse
5. Unit-Tests mit fake `post_fn` / canned JSON (Binance-DMS-Test-Stil)

**DoD P3:** Alle Safety-Unit-Tests; `open()` live weigert dual_mode und fehlendes DMS-Arm.

### Phase 4 — Execution Bridge Live

1. `GateFuturesOrderEncoder` (JSON)
2. `GateRestOrderTransport` (POST place, DELETE cancel; exchange id aus Response `id`)
3. `GateUserDataTransport` + `GateFuturesUserDataParser`
4. `ExecutionBridge` wiring exakt wie `BinanceFuturesProvider` live-block
5. Rate limiter: Gate private place/amend ~**100 r/s** — TokenBucket konservativ (z.B. 20/s capacity) start
6. `ClientOrderIdMinter` → map to `text` mit `t-` Prefix und Längen-Cap

**DoD P4:** Testnet place/cancel/fill path; kill-switch flatten; DMS arm/disarm.

### Phase 5 — Ops / Evidence

1. Docs: `docs/operations/` Gate-Testnet-SOP (analog `02-futures-testnet.md`)
2. Env: `GATE_FUTURES_TESTNET_KEY/SECRET`, `GATE_USER_ID`
3. Optional bracket adapter (price-triggered / TP-SL) — **nach** stabilen Fills
4. Mainnet shadow only first; live capital nur mit Phase-0-Ritual (prod.md)

---

## 9. Order Encode (kritisch)

### 9.1 Place — `POST /api/v4/futures/usdt/orders`

Body (JSON):

```json
{
  "contract": "BTC_USDT",
  "size": 1,
  "price": "65000.1",
  "tif": "gtc",
  "reduce_only": false,
  "text": "t-tt-<id>"
}
```

| TrueTest | Gate |
|----------|------|
| `order_side::buy` + qty | `size = +qty` (contracts) |
| `order_side::sell` + qty | `size = -qty` |
| `order_type::limit` | `price` string, `tif` |
| `order_type::market` | `price: "0"`, `tif: "ioc"` (Gate-Konvention) |
| `time_in_force::gtc` | `"gtc"` |
| `time_in_force::ioc` | `"ioc"` |
| post-only | `"poc"` (Pending-Or-Cancel) |
| reduce-only close | `"reduce_only": true` und/oder `"close": true` mit size 0 |
| client id | `"text": "t-..."` |

**Wichtig:**  
- `size` kann Integer oder String sein (Decimal-Header). Encoder: bevorzugt JSON-number wenn ganze Contracts, sonst String.  
- `quanto_multiplier`: Engine-Qty ist in **Contracts** (Gate-native), nicht in Coin — dokumentieren in Provider-README. Falls Strategies in Base-Asset denken: Conversion an der Provider-Grenze, nicht im Engine-Core.  
- Kein `positionSide` in v1 (one-way).

### 9.2 Cancel

- Single: `DELETE /futures/usdt/orders/{id}`  
  oder by `text` falls Docs erlauben (Query/Path — in Tests verifizieren)
- All: `DELETE /futures/usdt/orders?contract=BTC_USDT`

`encode_cancel` setzt `endpoint` + ggf. leeren body; Transport wählt HTTP-Methode.

### 9.3 `encoded_order` Konvention

Binance nutzt query-string `wire_payload`. Gate nutzt **JSON body**:

```cpp
e.endpoint     = "/api/v4/futures/usdt/orders"; // full path for signing
e.wire_payload = json_body; // raw JSON
e.client_order_id = text_without_or_with_prefix; // consistent with minter
```

`GateRestClient::post_json(path, body)` signiert mit body-hash.

### 9.4 Client ID Regeln (`text`)

- Prefix **`t-`** empfohlen/erzwungen
- Erlaubt: `[0-9A-Za-z_.-]`
- Länge: konservativ **≤ 28** nach `t-` (Gesamt oft ≤ 32) — an `ClientOrderIdMinter` Prefix anpassen (`"t-tt"` o.ä.)
- Minter-Output sanitizen: keine SQL-Keyword-Fallen nötig wie Binance-Testnet-WAF, aber ungültige Zeichen strippen

---

## 10. Parser-Maps

### 10.1 `futures.trades` → `provider::tick`

Beispiel-Update:

```json
{
  "channel": "futures.trades",
  "event": "update",
  "result": [{
    "id": 123,
    "create_time": 1710000000,
    "create_time_ms": 1710000000123,
    "contract": "BTC_USDT",
    "size": -3,
    "price": "65000.1",
    "is_internal": false
  }]
}
```

| Gate field | TrueTest |
|------------|----------|
| `create_time_ms` / `create_time` | `tick.timestamp` |
| `contract` | `tick.symbol` |
| `price` | `tick.price` |
| `abs(size)` | `tick.quantity` (Scale beachten: int64; Decimal → quantize) |
| `size > 0` | side buy/taker buy → `side=0` (bid) analog Binance-Map |
| `size < 0` | sell |
| `is_internal==true` | drop oder status-event (kein normaler Match) |

### 10.2 `futures.order_book_update` → L2

```json
"result": {
  "t": 1615366381417,
  "s": "BTC_USDT",
  "U": 2517661101,
  "u": 2517661113,
  "b": [{"p": "54672.1", "s": "0"}],
  "a": [{"p": "54743.6", "s": "1"}],
  "full": false
}
```

**Sync-Algorithmus (wie Docs):**

1. Subscribe + buffer updates
2. REST `order_book?with_id=true` → base `id`
3. Apply first update where `U <= base_id+1 <= u`
4. Danach sequentiell; Gap → resync
5. `s=="0"` → Level löschen; absolute sizes
6. `full:true` → replace local book

Map zu `provider::l2_snapshot` (full) / `l2_update` (incremental). Hotpath: **kein** `nlohmann`; handgeparste Extractors wie `binance_parser.h`.

### 10.3 Candlesticks → `provider::bar`

REST/WS: `t,o,h,l,c,v` — Interval aus Subscribe/Backfill-Config (`1m` default).

### 10.4 Private: Orders / UserTrades → `parsed_exec`

| Gate | `parsed_exec` |
|------|----------------|
| `text` | `client_order_id` (strip `t-` optional, konsistent halten) |
| `id` | `exchange_order_id` |
| `contract` | `symbol` |
| `size` sign | `side` |
| fill size/price from usertrades or order update | `last_fill_*` |
| `status` / `finish_as` / `left` | `kind`: open→ack, left↓→partial, finished+filled→full_fill, cancelled→canceled, … |
| fees if present | `commission` |

`finish_as` Werte u.a.: `filled`, `cancelled`, `liquidated`, `ioc`, `auto_deleveraged`, `poc` — map loud, unknown → `other` + log.

### 10.5 Positions / Balances → `parsed_position_snapshot`

| Gate | Snapshot |
|------|----------|
| position `size` | `qty` signed |
| `contract` | `symbol` |
| margin mode fields | `margin_type` → `ISOLATED`/`CROSSED` |
| balance `change` + `type` | `balance_change`; `type==fund` → `reason::funding_fee` |
| `type` fee/pnl/dnw | map to `reason` enum |

---

## 11. DMS / Kill-Switch / Reconciler

### 11.1 Dead-Man's Switch

**Endpoint:** `POST /api/v4/futures/usdt/countdown_cancel_all`

Body:

```json
{ "timeout": 30, "contract": "BTC_USDT" }
```

| Param | Bedeutung |
|-------|-----------|
| `timeout` | Sekunden bis Auto-Cancel **aller** (oder contract-filter) Open Orders; **min 5**; `0` = disarm |
| `contract` | optional; v1 **immer setzen** auf Provider-Symbol |

**TrueTest-Config bleibt ms** (CLI-Kompatibilität zu Binance):

```text
dead_man_countdown_ms=30000  → timeout_s = max(5, countdown_ms/1000)
dead_man_heartbeat_ms=8000   → default countdown/3, aber heartbeat < timeout und ≥1s
```

**Invariants (copy from Binance DMS comments):**

- Arm fail → **refuse live**
- Heartbeat thread sole refresher; `last_beat_ms_` für Watchdog
- `stop()` ohne disarm; `disarm()` explizit bei orderly shutdown
- DMS cancelt **nur Orders**, nicht Position — Kill-Switch flatten ist die andere Hälfte
- Optional Phase-3: `dms_attempt_position_close` → reduce-only market close fn (Gate signed size)

**Niemals:** timeout unter Last erhöhen, Retry-Loops mit Backoff die Safety „grün“ malen, auto-clear halt.

### 11.2 Kill-Switch

`cancel_all_and_flatten(deadline)`:

1. Per-call REST timeout = `min(1500ms, deadline/3)`
2. `DELETE /futures/usdt/orders?contract=BTC_USDT`  
   - „no orders“ als Erfolg behandeln (Gate-Error-Label mappen, analog Binance `-2011`)
3. `GET /futures/usdt/positions/BTC_USDT` → `size`
4. Wenn `|size| < eps` → success
5. Place reduce-only market: `size = -position_size`, `price="0"`, `tif="ioc"`, `reduce_only=true`, unique `text`
6. Deadline überschritten → `return false` (loud), **kein** Retry-Scheduler

### 11.3 Reconciler

```text
GET /futures/usdt/accounts     → available  ↔ local cash
GET /futures/usdt/positions/X  → size       ↔ local position.qty
```

- Tolerance: `tolerance_bps` wie Binance
- **Kein** Spot-Testnet-Reset-Heuristic (Futures analog Binance-Futures: refuse on drift)
- Network/HTTP fail → non-empty error string → engine refuses start
- dual_mode nicht hier „reparieren“ — bereits in `open()` geblockt

### 11.4 Freeze-Hinweis

Neue `*kill_switch*`, `*dead_mans*`, `*reconciler*` unter `providers/gate/` sind **safety-adjacent**.  
Aktueller Freeze-Script-Pfad listet primär Binance-Dateien — bei Merge:

1. Governance klären: Gate-Safety analog freesen oder explizit „new venue under review“
2. T2/T3 Multi-Agent Protocol (root `AGENTS.md` §6)
3. Keine stillen Verhaltenänderungen an `live_safety.h` Interfaces ohne CCB

---

## 12. Provider-Config Keys

| Key | Pflicht | Beispiel | Bedeutung |
|-----|---------|----------|-----------|
| `symbol` | ja | `BTC_USDT` | Contract |
| `stream` | nein | `trades` | public channel family |
| `depth_stream` | nein | `100ms:100` | order_book_update freq:level |
| `api_key` / `api_secret` | live | | APIv4 key |
| `user_id` | live private WS | `12345678` | Gate UID |
| `testnet` | nein | `true` | endpoint preset |
| `host` / `port` / `rest_host` | nein | override | |
| `max_notional_usdt` | nein | `150` | FuturesRiskCheck |
| `max_leverage` | nein | `2.5` | |
| `min_liquidation_distance_pct` | nein | `0.07` | |
| `maintenance_margin_pct` | nein | | flat fallback |
| `dead_man_countdown_ms` | live emp. | `30000` | → timeout_s |
| `dead_man_heartbeat_ms` | nein | `8000` | |
| `dms_attempt_position_close` | nein | `true` | |
| `margin_type` | nein | `isolated` | advisory |
| `margin_type_strict` | nein | `true` | refuse on mismatch |

---

## 13. `GateFuturesProvider::open()` — Ablauf (Checkliste)

Spiegle `BinanceFuturesProvider::open()`:

1. `state_ = opening`
2. Build `FuturesRiskCheck` if any cap > 0
3. Construct market transport(s); `set_halt_callback` wiring
4. Optional backfill candlesticks → `PrependTransport`
5. **If live + keys:**
   1. `GateRestClient` + `resync_clock` + skew refuse
   2. Contract probe `GET .../contracts/{symbol}`
   3. Load contract meta (`quanto_multiplier`, ticks, min size) → instrument_spec + mm if available
   4. `GET accounts` → if `in_dual_mode` **refuse**
   5. Position advisories (liq distance / margin) via `gate_futures_safety.h`
   6. `ClientOrderIdMinter`, rate limiter
   7. Reconciler + KillSwitch + BracketAdapter
   8. `ExecutionBridge` deps: order_tx, fills_tx (user data), encoder, parser, rate limiter, client_id_fn, position_snapshot_handler (funding)
   9. `bridge_->open()` or refuse
   10. DMS `start()` last; fail → close bridge, refuse
6. Else if shadow → `TradeTapeShadowAdapter`
7. Else → `HybridExecutor` paper path
8. `live_transport->open()`; `state_ = open`

`close()`: DMS stop+disarm → transport close → bridge close.

`get_liveness_sources()`: DMS heartbeat atomic + optional user-data / market WS last-alive.

---

## 14. Rate Limits & Quirks

| Thema | Detail | Implikation |
|-------|--------|-------------|
| Private place/amend | ~100 r/s (UID) | TokenBucket konservativ |
| Cancel | ~200 r/s | Kill-switch bursts ok |
| Other private | ~200 / 10s / endpoint | Backfill nicht spammen |
| WS connections | ≤ ~300 / IP | ein Combined-Socket |
| `user_id` | private sub payload | Config-pflicht live |
| `quanto_multiplier` | Contract meta | Notional/Risk |
| Decimal sizes | Header `X-Gate-Size-Decimal: 1` | Parser dual-type |
| dual / dual_plus | account modes | v1 refuse non-single |
| `text` client id | `t-` prefix | Encoder/Minter |
| Timestamp | seconds REST | nicht ms in SIGN |
| Server time | ms in `/spot/time` | convert carefully |
| Internal trades | `is_internal` | filter |
| SBE | binary market data | v2 only |
| WS trading API | `futures.order_place` | v1 REST primary (simpler bridge) |

---

## 15. Tests — Pflichtmatrix

| Test | Inhalt |
|------|--------|
| `test_gate_auth` | REST sign vector, empty body hash, WS sign string |
| `test_gate_endpoints` | mainnet/testnet hosts, path builders, testnet detection |
| `test_gate_parser` | number/string size, nested result arrays |
| `test_gate_combined_parser` | trades + book_update fixtures → events |
| `test_gate_order_encoder` | buy/sell size sign, market ioc, text prefix, reduce_only |
| `test_gate_rest_order_transport` | status mapping, id extract, error redact |
| `test_gate_user_data_parser` | order update kinds, funding balance type |
| `test_gate_futures_register` | registry names `gate` + `gate-futures`, missing symbol throws |
| `test_gate_futures_reconciler` | canned accounts/positions match/mismatch |
| `test_gate_futures_kill_switch` | cancel+flatten sequence with fake REST |
| `test_gate_futures_dead_mans_switch` | arm/refresh/disarm; fail arm; timeout_s≥5 |
| `test_gate_futures_safety` | dual_mode refuse helpers, liq advisory |
| `test_gate_futures_testnet_live` | `GATE_*` env; skip if unset |

Fixtures: `tests/fixtures/gate/*.json` mit redacted real payloads.

---

## 16. Definition of Done (Integration komplett)

- [ ] `ENABLE_GATE=ON` baut `engine_{backtest,shadow,live}`
- [ ] Registry: `gate-futures` und Alias `gate`
- [ ] Phase-1 Shadow: trades (+ optional L2) stabil ≥ 30 min ohne Leak (ASAN build empfohlen)
- [ ] dual_mode refuse getestet
- [ ] Reconciler refuse on cash/pos drift
- [ ] Kill-switch cancel+flatten auf Testnet geübt
- [ ] DMS: arm, heartbeat, SIGKILL-Drill → Venue cancelt Orders nach timeout
- [ ] Live orders nur in `engine_live`
- [ ] `./scripts/check-hotpath-json.sh` / `check-layer-deps.sh` / `check-live-safety-freeze.sh` grün
- [ ] Kein `HAS_GATE` außerhalb Provider/CMake/Tests
- [ ] Docs: Testnet-SOP + Config-Keys in `docs/reference/` oder operations
- [ ] Keine Perf-Claims ohne Benchmark
- [ ] Multi-Agent Review T2 für Safety-Dateien

---

## 17. Pitfalls (häufige Fußangeln)

1. **SHA256 vs SHA512** — Copy-Paste aus `binance_auth.h` ohne Digest-Wechsel → `INVALID_SIGNATURE`.
2. **Timestamp units** — REST SIGN will **seconds**; `/spot/time` liefert **ms**; WS `time` seconds.
3. **Sign path** muss `/api/v4/...` enthalten, nicht nur `/futures/...`.
4. **Body hash** über exakt gesendete Bytes (kein pretty-print / Key-Reorder nach Sign).
5. **`size` signed** — `side=SELL` vergessen und positives size senden → falsche Richtung.
6. **`BTCUSDT` vs `BTC_USDT`** — Gate will Underscore.
7. **`user_id` missing** — private subscribe „success“ aber keine Events.
8. **DMS in ms an Gate senden** — `timeout: 30000` wäre 30000 **Sekunden**; immer `/1000` und `max(5,…)`.
9. **Hedge mode** stillschweigend erlauben — Encoder kennt keine dual positions → refuse.
10. **Hotpath JSON library** — verboten; hand parser only.
11. **listenKey-Mentalmodell** — Gate hat keines; Reconnect = re-auth subscribe, kein PUT keepalive.
12. **Quantity scale** — `provider::tick.quantity` ist `int64_t`; Decimal-Contracts brauchen feste Scale-Konvention (z.B. contracts * 1e8) oder v1 nur integer contracts.
13. **Notional ohne `quanto_multiplier`** — Risk-Check unter-/überschätzt Exposure.
14. **Cancel-all ohne contract** — kann Account-weit löschen; v1 immer symbol-scopen.
15. **Safety retries** — „hilfreiche“ Rest-Retry-Wrapper um Kill-Switch → S3/S4 Violation.

---

## 18. Beispiel-Kommandos (Zielzustand)

```bash
# Build
cmake -B build -DENABLE_GATE=ON -DENABLE_QUESTDB=ON -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"

# Shadow market data
./build/engine_shadow \
  --provider gate-futures \
  --symbol BTC_USDT \
  --stream trades \
  --depth-stream 100ms:100 \
  --no-tui --status-format off

# Testnet live drill (tiny)
export GATE_FUTURES_TESTNET_KEY=...
export GATE_FUTURES_TESTNET_SECRET=...
export GATE_USER_ID=...

./build/engine_live \
  --provider gate-futures \
  --symbol BTC_USDT \
  --testnet --live \
  --api-key "$GATE_FUTURES_TESTNET_KEY" \
  --api-secret "$GATE_FUTURES_TESTNET_SECRET" \
  # user_id via config file or extended flag → provider_config["user_id"]
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \
  --max-notional 150 --max-leverage 2.5 --min-liq-distance-pct 0.07
```

---

## 19. Offizielle Links

| Thema | URL |
|-------|-----|
| REST APIv4 | https://www.gate.com/docs/developers/apiv4/en/ |
| Futures WebSocket | https://www.gate.com/docs/developers/futures/ws/en/ |
| Ältere Futures REST-Beispiele | https://www.gate.com/docs/futures/api/index.html |
| SDKs (Sign-Referenz) | https://github.com/gate/gateapi-python (u.a.) |

Interne Referenzen:

| Thema | Pfad |
|-------|------|
| Provider interface | `src/providers/provider.h` |
| Live safety interfaces | `src/execution/live_safety.h` |
| Golden futures provider | `src/providers/binance/binance_futures_provider.h` |
| DMS reference | `src/providers/binance/binance_futures_dead_mans_switch.h` |
| Register pattern | `src/providers/binance/binance_futures_register.cpp` |
| Sources list | `cmake/Sources.cmake` |
| Feature flag pattern | `cmake/Dependencies.cmake` (`ENABLE_BINANCE`) |
| Agent rules | `AGENTS.md` |
| Futures testnet SOP (Binance) | `docs/operations/02-futures-testnet.md` |
| Future venues todo | `docs/todos/09-other-future-gates.md` |

---

## 20. Agent-Arbeitsprotokoll (kurz)

1. **Nicht** mit Live-Safety-Dateien starten — Phase 0/1 zuerst.  
2. Bei jedem PR die drei Check-Scripts aus `AGENTS.md` §2.  
3. Safety-Phasen: Verifier + Fact-checker + 2 Parallel Executors (T2/T3).  
4. Keine Venue-Ifdefs in Engine; CLI nur `provider_config` füttern.  
5. Bei Unklarheit in Gate-Docs: Testnet-Probe + Fixture committen, nicht raten und soft-failen.  
6. Decimal-Size und dual_plus als **explizite Follow-ups** tracken, nicht still in v1 mischen.

---

*Ende der Bauanleitung. Nächster konkreter Implementierungs-PR: Phase 0 Scaffold (`gate_auth`, `gate_endpoints`, CMake `ENABLE_GATE`, Registry-Stub, Auth-Unit-Tests).*
