# Bybit Futures Provider — Grok Build Implementation Guide

> **Ziel-Datei:** `upcoming_plattform/bybit.md`  
> **Goldene Referenz im Repo:** `src/providers/binance/*` (insb. `binance_futures_provider.h`)  
> **API-Stand:** Bybit V5 (offizielle Docs: https://bybit-exchange.github.io/docs/v5/)  
> **Sprache:** Narrative auf Deutsch; Identifier, Pfade, API-Namen unverändert Englisch.

---

## 1. Goal & Scope

### In Scope (Phase 0–4)

| Scope | Detail |
|-------|--------|
| **Produkt** | Bybit **USDT linear perpetuals** (`category=linear`, Symbol z. B. `BTCUSDT`) |
| **Modi** | `engine_backtest` (kein Netzwerk nötig), `engine_shadow` (MD live, paper fills), `engine_live` (REST orders + private WS) |
| **Daten** | Public WS: `publicTrade`, `orderbook.{depth}`, `kline.{interval}` |
| **Orders** | REST: create / amend / cancel / cancel-all; fills über private WS `execution` + `order` |
| **Safety** | Reconciler, Kill-Switch, DMS-Strategie (DCP nur wenn institutional), `FuturesRiskCheck`, optional Brackets |
| **Build-Flag** | `ENABLE_BYBIT` → `HAS_BYBIT` (Spiegel von `ENABLE_BINANCE` / `HAS_BINANCE`) |

### Out of Scope (später, explizit nicht mitmischen)

- Spot (`category=spot`)
- Inverse (`category=inverse`)
- Options, Spread, RFQ
- WebSocket Trade-Entry (`wss://…/v5/trade`) — Phase 2 nutzt **REST** wie Binance
- Hedge-Mode-Trading (Account muss One-Way sein; wie Binance: refuse at `open()`)
- Multi-Symbol concurrent live (ein Symbol pro Provider-Instanz, wie Binance-Futures)

### Erfolgsbild

```bash
# Shadow (MD only, keine Live-Orders)
./build/engine_shadow \
  --provider bybit-futures --symbol BTCUSDT --stream trade \
  --depth-stream orderbook.50 --testnet \
  --no-pin --status-format off --no-tui

# Live testnet (nach Phase 4 Ritual)
./build/engine_live \
  --provider bybit-futures --symbol BTCUSDT --stream trade \
  --testnet --live \
  --max-notional-usdt 50 --max-leverage 5 \
  --dead-man-countdown-ms 30000
```

---

## 2. Non-Negotiables from TrueTest

Abgeleitet aus `AGENTS.md`, `docs/architecture/01-target-architecture.md`, `src/providers/provider.h`, Freeze-Liste.

| ID | Regel | Konsequenz für Bybit |
|----|-------|----------------------|
| **R1** | Zero heap auf Hot Path | Hand-Parser à la `binance_parser.h` (`for_each_flat_field`, `find_key`); **kein** `nlohmann::json` in Parser/Transport-Hot-Path |
| **R2** | `forbid_runtime_grow` nach Prewarm | Pools nur Engine-seitig; Provider allokiert nicht pro Tick auf dem Event-Loop |
| **R3** | SPSC, sole producer | Bybit-WS-Reader-Thread → Frame an Engine; nie MPMC „für Bequemlichkeit“ |
| **R6** | Kein JSON-Allowlist-Missbrauch | `scripts/check-hotpath-json.sh` muss grün bleiben; Parser bleiben header-only Hand-Scan |
| **R10** | Bestehende Brücken nutzen | `ExecutionBridge` + `IOrderEncoder` + `IOrderTransport` + `IFillTransport` + `IFillParser` — **kein** paralleles Order-Subsystem |
| **S2** | `TT_TARGET` compile-time gate | Live-Orders nur in `engine_live`; kein Runtime-„allow live“-Schalter |
| **S3** | Halt write-once terminal | Fatal WS-Disconnect → `set_halt_callback` → Engine halt; kein Auto-Resume |
| **S4** | Safety loud, non-retrying, fail-closed | Kill/DMS/Reconciler: Fehler → refuse/halt, **kein** silent backoff-retry auf Safety-Pfaden |
| **S6** | Reconciler default-refuse | Drift > tolerance → non-empty error string → Engine startet nicht |
| **S7** | Venue `IRiskCheck` vor `RiskManager` | `FuturesRiskCheck` aus Provider `get_risk_check()` |
| **S9** | **Kein Venue-Leak in Core** | **`HAS_BYBIT` nur in `src/providers/bybit/*` und CMake/Register**; **niemals** in `src/engine/`, `src/core/`, `src/threading/`, `src/risk/` (generisch). Optional dünne CLI-Zweige in `src/bin/main.inc` sind erlaubt (wie Binance), aber **kein** Bybit-Endpoint/Auth in Engine |
| **Layer** | `scripts/check-layer-deps.sh` | Provider darf `core types utils data orderbook execution engine exits risk simulation threading ui` — nicht umgekehrt Venue in Core ziehen |
| **Freeze** | Live-Safety Freeze | Neue Bybit-Safety-Dateien sind **nicht** in der 10er-Freeze-Liste, aber **dieselben Invarianten**. Edits an `live_safety.h` / `engine.cpp` / `risk/*` brauchen CCB-Token |

### Bad vs Good (Bybit-spezifisch)

```cpp
// BAD — nlohmann auf Market-Data-Hot-Path
auto j = nlohmann::json::parse(frame);
double px = j["data"][0]["p"];

// GOOD — hand parser (Spiegel binance::detail::for_each_flat_field)
auto tick = bybit::parse_public_trade(frame); // optional<provider::tick>
```

```cpp
// BAD — HAS_BYBIT in engine.cpp
#ifdef HAS_BYBIT
  if (provider_name == "bybit") special_case();
#endif

// GOOD — nur ProviderRegistry + IProvider Polymorphie
auto p = ProviderRegistry::instance().create(name, cfg);
```

```cpp
// BAD — HTTP 200 reicht
if (resp.status == 200) ok = true;

// GOOD — Bybit retCode
// retCode == 0 und HTTP 2xx; sonst fail-closed mit redact_for_log
```

---

## 3. Architecture Mapping: Bybit API → TrueTest Interfaces

| TrueTest Interface / Typ | Datei (Core) | Bybit-Äquivalent | Bybit-Datei (neu) |
|--------------------------|--------------|------------------|-------------------|
| `IProvider` | `src/providers/provider.h` | Gesamter Venue-Adapter | `bybit_futures_provider.h` |
| `IDataTransport` | `src/providers/transport.h` | Public WS linear | `bybit_transport.h`, `bybit_combined_transport.h` |
| `IDataParser<provider::event>` | `src/providers/parser.h` | Trade/OB/Kline JSON | `bybit_parser.h`, `bybit_combined_parser.h` |
| `provider::tick/bar/l2_*` | `src/providers/provider_event.h` | `publicTrade` / `kline` / `orderbook` | Parser maps |
| `IExecutionAdapter` | `src/execution/execution_adapter.h` | Live: `ExecutionBridge`; Shadow: `TradeTapeShadowAdapter` | verdrahtet in Provider `open()` |
| `IOrderEncoder` | `src/execution/order_encoder.h` | JSON body create/cancel | `bybit_futures_order_encoder.h` |
| `IOrderTransport` | `src/execution/order_transport.h` | `POST /v5/order/*` | `bybit_rest_order_transport.h` |
| `IFillTransport` | `src/execution/fill_transport.h` | Private WS `wss://…/v5/private` | `bybit_user_data_transport.h` |
| `IFillParser` | `src/execution/fill_parser.h` | topics `execution`, `order`, `position`, `wallet` | `bybit_futures_user_data_parser.h` |
| `IReconciler` | `src/execution/live_safety.h` | `GET /v5/position/list` + `GET /v5/account/wallet-balance` | `bybit_futures_reconciler.h` |
| `IKillSwitch` | `src/execution/live_safety.h` | cancel-all + reduceOnly MARKET | `bybit_futures_kill_switch.h` |
| Dead-man switch | (Binance: venue countdown) | **DCP institutional-only** → siehe §10 | `bybit_futures_dead_mans_switch.h` |
| `IRiskCheck` / `FuturesRiskCheck` | `src/risk/futures_risk_check.h` | Caps aus Config + instruments-info | Provider setzt `rc_cfg_` |
| `IBracketAdapter` | `src/exits/bracket_adapter.h` | TP/SL params oder separate conditional orders | `bybit_futures_bracket_adapter.h` |
| `ProviderRegistry` | `src/providers/provider_registry.h` | `REGISTER_PROVIDER("bybit-futures", …)` | `bybit_futures_register.cpp` |
| `WorkerWatchdog` liveness | `IProvider::get_liveness_sources()` | DMS heartbeat / private-WS ping thread | Provider |

### Mode Dispatch (1:1 Spiegel `BinanceFuturesProvider::open()`)

```
configure(engine_config)  → speichert mode_, fee/fill/latency models, qty_scale, seed, …
open():
  1. FuturesRiskCheck bauen (falls Caps > 0)
  2. Public WS Transport bauen (trade und/oder orderbook)
  3. Optional REST kline backfill → PrependTransport
  4. mode == live && api_key:
       RestClient + clock skew check
       instruments-info probe (Symbol existiert)
       position-mode probe → refuse hedge
       margin/liq advisories
       minter + rate limiter
       reconciler + kill_switch + bracket_adapter
       ExecutionBridge{order_tx, fills_tx, encoder, parser, …}
       DMS arm (last) — fail → refuse live
     mode == shadow:
       TradeTapeShadowAdapter
     else (paper/backtest hybrid):
       HybridExecutor + local orderbook
  5. live_transport->open()
  6. state_ = open
close():
  dms stop+disarm → transport close → bridge close
```

Referenz-Implementierung:  
`/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_provider.h` (Zeilen ~159–580).

---

## 4. File Checklist

Alle unter `src/providers/bybit/` (neue Directory). Header-only wo Binance es auch ist; nur Register als `.cpp`.

### Pflicht (Phase 0–3)

| Pfad | Rolle | Binance-Spiegel |
|------|-------|-----------------|
| `src/providers/bybit/bybit_endpoints.h` | Hosts/ports mainnet/testnet/demo | `binance_endpoints.h` |
| `src/providers/bybit/bybit_auth.h` | HMAC-SHA256, header signing, redact | `binance_auth.h` |
| `src/providers/bybit/bybit_parser.h` | Hand-JSON helpers + trade/kline | `binance_parser.h` |
| `src/providers/bybit/bybit_depth_parser.h` | orderbook snapshot/delta | `binance_depth_parser.h` |
| `src/providers/bybit/bybit_combined_parser.h` | Multi-topic → `provider::event` | `binance_combined_parser.h` |
| `src/providers/bybit/bybit_transport.h` | Single public WS (linear) | `binance_transport.h` |
| `src/providers/bybit/bybit_combined_transport.h` | Multi-subscribe public WS | `binance_combined_transport.h` |
| `src/providers/bybit/bybit_rest_client.h` | Signed REST (JSON body + headers) | `binance_rest_client.h` |
| `src/providers/bybit/bybit_time_sync.h` | `/v5/market/time` skew check | `binance_time_sync.h` |
| `src/providers/bybit/bybit_futures_order_encoder.h` | `order_event` → JSON payload | `binance_futures_order_encoder.h` |
| `src/providers/bybit/bybit_rest_order_transport.h` | `IOrderTransport` | `binance_rest_order_transport.h` |
| `src/providers/bybit/bybit_user_data_transport.h` | Private WS + auth + ping | `binance_user_data_transport.h` |
| `src/providers/bybit/bybit_futures_user_data_parser.h` | execution/order/position → `parsed_exec` | `binance_futures_user_data_parser.h` |
| `src/providers/bybit/bybit_futures_reconciler.h` | Startup cash+position gate | `binance_futures_reconciler.h` |
| `src/providers/bybit/bybit_futures_kill_switch.h` | cancel-all + flatten | `binance_futures_kill_switch.h` |
| `src/providers/bybit/bybit_futures_dead_mans_switch.h` | DCP oder process-local DMS | `binance_futures_dead_mans_switch.h` |
| `src/providers/bybit/bybit_futures_safety.h` | Advisories (margin/liq) | `binance_futures_safety.h` |
| `src/providers/bybit/bybit_futures_provider.h` | `IProvider` composition root | `binance_futures_provider.h` |
| `src/providers/bybit/bybit_futures_register.cpp` | `REGISTER_PROVIDER` | `binance_futures_register.cpp` |

### Phase 1/4 optional / später

| Pfad | Rolle |
|------|-------|
| `src/providers/bybit/bybit_backfill.h` | REST klines → PrependTransport |
| `src/providers/bybit/bybit_executor.h` | Paper mid-price executor (wie `BinanceExecutor`) |
| `src/providers/bybit/hybrid_executor.h` | **Nicht kopieren** — `providers/binance/hybrid_executor.h` wiederverwenden oder generisch halten; wenn Bybit-spezifisch nötig: eigene dünne Wrapper |
| `src/providers/bybit/bybit_futures_bracket_adapter.h` | Venue SL/TP |
| `src/providers/bybit/bybit_recorder.h` / `bybit_replay_transport.h` | Optional replay (Phase später) |

### Tests (immer in `cmake/Sources.cmake` listen, Runtime-Guard `#ifdef HAS_BYBIT`)

```
tests/test_bybit_auth.cpp
tests/test_bybit_endpoints.cpp
tests/test_bybit_parser.cpp
tests/test_bybit_depth_parser.cpp
tests/test_bybit_combined_parser.cpp
tests/test_bybit_futures_order_encoder.cpp
tests/test_bybit_rest_order_transport.cpp
tests/test_bybit_futures_user_data_parser.cpp
tests/test_bybit_futures_register.cpp
tests/test_bybit_futures_reconciler.cpp
tests/test_bybit_futures_kill_switch.cpp
tests/test_bybit_futures_dead_mans_switch.cpp
tests/test_bybit_futures_safety.cpp
tests/test_bybit_futures_bracket_adapter.cpp
tests/test_bybit_futures_testnet_live.cpp   # GATED: env + HAS_BYBIT
```

### CLI / CMake Touches (minimal, nicht in `src/engine/`)

| Datei | Änderung |
|-------|----------|
| `CMakeLists.txt` | `option(ENABLE_BYBIT … OFF)` |
| `cmake/Dependencies.cmake` | `if(ENABLE_BYBIT)` → sources + `HAS_BYBIT` + Boost/OpenSSL |
| `cmake/Sources.cmake` | Test-Liste erweitern |
| `CMakePresets.json` | optional `"ENABLE_BYBIT": "ON"` in linux-tests |
| `src/bin/main.inc` | Provider-Namen, Env-Vars, Help-Text, `pcfg` keys (dünn, analog Binance) |

---

## 5. CMake / `ENABLE_BYBIT` / `HAS_BYBIT` Wiring

### `CMakeLists.txt` (neben Zeile 10)

```cmake
option(ENABLE_BYBIT "Build with Bybit exchange provider" OFF)
```

### `cmake/Dependencies.cmake` (Spiegel Block `ENABLE_BINANCE` ~108–118)

```cmake
# Bybit exchange provider
if(ENABLE_BYBIT)
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_sources(${target} PRIVATE
        ${_src}/providers/bybit/bybit_futures_register.cpp)
    target_link_libraries(${target} PUBLIC
        Boost::headers OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(${target} PUBLIC HAS_BYBIT)
endif()
```

### Header-Guard-Muster (jedes Bybit-Header, das OpenSSL/Beast braucht)

```cpp
#pragma once
#ifdef HAS_BYBIT
// ...
#endif // HAS_BYBIT
```

Parser-Helpers ohne Netzwerk **dürfen** ohne `HAS_BYBIT` kompilierbar sein (wie Teile von `binance_parser.h`), aber Register **muss** guarded sein:

```cpp
// bybit_futures_register.cpp
#ifdef HAS_BYBIT
#include "providers/provider_registry.h"
#include "providers/bybit/bybit_futures_provider.h"
// REGISTER_PROVIDER(...)
#endif
```

### Build

```bash
cmake -B build -DENABLE_BYBIT=ON -DENABLE_BINANCE=ON -DBUILD_TESTS=ON
cmake --build build -j
# Nach jeder src/-Änderung:
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

---

## 6. Registration

### `bybit_futures_register.cpp` (1:1 Struktur wie `binance_futures_register.cpp`)

```cpp
#ifdef HAS_BYBIT

#include "providers/provider_registry.h"
#include "providers/bybit/bybit_futures_provider.h"
#include "providers/bybit/bybit_endpoints.h"

#include <stdexcept>

// Primärname (analog "binance-futures")
REGISTER_PROVIDER("bybit-futures", [](const provider_config& cfg) {
    auto get = [&](const std::string& key) -> std::string {
        auto it = cfg.find(key);
        return it == cfg.end() ? "" : it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error(
            "bybit-futures provider requires 'symbol' (e.g. BTCUSDT)");

    auto stream = get("stream");
    if (stream.empty()) stream = "trade";  // mapped → publicTrade

    const auto host_cfg = get("host");
    const auto port_cfg = get("port");
    const auto testnet_cfg = get("testnet");
    const auto demo_cfg = get("demo");
    const bool want_demo =
        demo_cfg == "1" || demo_cfg == "true";
    const bool want_testnet =
        !want_demo &&
        (testnet_cfg == "1" || testnet_cfg == "true" ||
         bybit::looks_like_testnet_host(host_cfg));

    bybit::endpoints ep = want_demo
        ? bybit::linear_demo()
        : want_testnet
            ? bybit::linear_testnet()
            : bybit::linear_mainnet();

    if (!host_cfg.empty()) ep.ws_public_host = host_cfg;
    if (!port_cfg.empty()) ep.ws_port = port_cfg;

    auto provider = std::make_shared<BybitFuturesProvider>(
        symbol, stream,
        get("api_key"), get("api_secret"),
        ep);
    provider->set_endpoints(ep);

    // depth_stream: z.B. "orderbook.50" → topic orderbook.50.BTCUSDT
    auto depth = get("depth_stream");
    if (!depth.empty())
        provider->set_depth_stream(depth);

    // Risk / DMS keys — gleiche Namen wie Binance pcfg, damit main.inc reuse:
    // margin_type, liquidation_warn_pct, margin_type_strict,
    // max_notional_usdt, max_leverage, min_liquidation_distance_pct,
    // maintenance_margin_pct, dead_man_countdown_ms, dead_man_heartbeat_ms,
    // dms_attempt_position_close

    return provider;
});

// Optionaler Alias (CLI-Komfort)
REGISTER_PROVIDER("bybit", /* same factory as above */);

#endif // HAS_BYBIT
```

### `provider_config` Keys (verbindlich)

| Key | Beispiel | Bedeutung |
|-----|----------|-----------|
| `symbol` | `BTCUSDT` | Linear symbol (uppercase intern) |
| `stream` | `trade` / `kline.1` | Public primary stream |
| `depth_stream` | `orderbook.50` | Optional second topic |
| `api_key` / `api_secret` | … | Live credentials |
| `testnet` | `1` | Testnet endpoints |
| `demo` | `1` | Demo Trading (`api-demo.bybit.com`) |
| `host` / `port` | override | WS host override |
| `dead_man_countdown_ms` | `30000` | 0 = disarm |
| `dead_man_heartbeat_ms` | `8000` | 0 → countdown/3 |
| `dms_attempt_position_close` | `true` | DMS flatten on heartbeat fail |
| Risk-Keys | wie Binance | an `FuturesRiskCheck::config` |

---

## 7. Phase Plan

### Phase 0 — Scaffold + Endpoints + Auth Unit Tests

**Deliverables**

1. Directory `src/providers/bybit/`
2. `bybit_endpoints.h` mit:

```cpp
namespace bybit {
struct endpoints {
    std::string rest_host;       // api.bybit.com
    std::string rest_port;       // 443
    std::string ws_public_host;  // stream.bybit.com
    std::string ws_private_host; // stream.bybit.com (gleicher Host, anderer path)
    std::string ws_port;         // 443
    std::string ws_public_path;  // /v5/public/linear
    std::string ws_private_path; // /v5/private
    bool is_testnet = false;
    bool is_demo = false;
};

inline endpoints linear_mainnet() {
    return {"api.bybit.com", "443",
            "stream.bybit.com", "stream.bybit.com", "443",
            "/v5/public/linear", "/v5/private", false, false};
}
inline endpoints linear_testnet() {
    return {"api-testnet.bybit.com", "443",
            "stream-testnet.bybit.com", "stream-testnet.bybit.com", "443",
            "/v5/public/linear", "/v5/private", true, false};
}
// Demo: REST api-demo.bybit.com; private WS stream-demo.bybit.com;
// Public MD laut Docs: mainnet public streams (Demo hat kein eigenes public MD).
inline endpoints linear_demo() {
    return {"api-demo.bybit.com", "443",
            "stream.bybit.com", "stream-demo.bybit.com", "443",
            "/v5/public/linear", "/v5/private", false, true};
}
}
```

3. `bybit_auth.h`:
   - `hmac_sha256_hex(secret, payload)` — OpenSSL EVP wie Binance
   - REST sign:  
     `payload = timestamp + api_key + recv_window + (queryString|jsonBody)`  
     Headers: `X-BAPI-API-KEY`, `X-BAPI-TIMESTAMP`, `X-BAPI-SIGN`, `X-BAPI-RECV-WINDOW`
   - WS private auth sign:  
     `HMAC_SHA256(secret, "GET/realtime" + expires)` → hex
   - `redact_for_log` (Keys: apiKey, api_secret, signature, X-BAPI-*, orderLinkId optional)

4. Unit tests mit **festen Vektoren** aus Bybit-Docs (timestamp+key+window+body → known signature).  
5. CMake `ENABLE_BYBIT` verdrahten; leerer Provider stub der `name()=="bybit-futures"` und `open()` false zurückgibt, ist OK.

**DoD Phase 0:** `ctest -R bybit_auth|bybit_endpoints` grün; Layer/JSON/Freeze-Scripts grün.

---

### Phase 1 — Market Data + Hand Parsers + Shadow

**Transport-Unterschiede zu Binance (kritisch!)**

| | Binance | Bybit |
|--|---------|-------|
| Connect target | `/ws/<stream>` oder `/stream?streams=` | Path `/v5/public/linear` |
| Subscribe | URL-encoded streams | Nach Connect: `{"op":"subscribe","args":["publicTrade.BTCUSDT"]}` |
| Heartbeat | Beast idle pings | App-level `{"op":"ping"}` alle ~20s **zusätzlich** |
| Envelope | `{"stream":…,"data":{…}}` oder raw | `{"topic":"…","type":"…","ts":…,"data":…}` |

**Implementationsschritte**

1. `BybitTransport::open()`:
   - TLS WS zu `ws_public_host:443` + `ws_public_path`
   - Sende subscribe JSON
   - Reader-Thread: `read_frame` → queue/line für Engine
   - Ping-Thread oder integrierter Timer: `{"op":"ping"}`
   - TCP keepalive + idle timeout analog `BinanceCombinedTransport` (~1500ms idle, keep_alive_pings)
   - Live: `set_halt_callback` bei fatal disconnect (kein endlos reconnect)

2. `bybit_parser.h` / `bybit_combined_parser.h`:
   - Topic-Dispatch über `"topic"` string (nicht `"e"`)
   - Maps → `provider::tick` / `bar` / `l2_snapshot` (siehe §8)

3. Provider `open()` shadow branch:
   - `TradeTapeShadowAdapter` (wie Binance Zeile ~536–542)
   - `supports_event_stream() == true` wenn depth gesetzt
   - `get_event_parser()` → `BybitCombinedParser`

4. Shadow manuell:
   ```bash
   ./build/engine_shadow --provider bybit-futures --symbol BTCUSDT \
     --stream trade --depth-stream orderbook.50 --testnet \
     --strategy sma --no-tui --status-format off
   ```

**DoD Phase 1:** Parser-Unit-Tests mit canned fixtures; Shadow läuft ≥5 min ohne Crash; kein `nlohmann` in Parser-Headers.

---

### Phase 2 — REST Orders + User Stream Fills

**REST-Client-Unterschiede**

| | Binance | Bybit |
|--|---------|-------|
| Sign location | Query `&signature=` | Header `X-BAPI-SIGN` |
| POST body | form/query params | **JSON object** |
| Cancel HTTP verb | `DELETE` | **`POST /v5/order/cancel`** |
| Success | HTTP 2xx + body | HTTP 2xx **und** `retCode==0` |
| Time | `/fapi/v1/time` | `GET /v5/market/time` |
| Order id field | `orderId` | `result.orderId` |

**`BybitRestClient` API-Skizze**

```cpp
struct response { int status; std::string body; int ret_code = -1; };
response get(path, query_string);           // signed, query in URL
response post_json(path, json_body);        // signed, body = JSON
response get_unsigned(path, query_string);
bool ok(const response& r) { return r.status>=200 && r.status<300 && r.ret_code==0; }
```

**ExecutionBridge Wiring** (live block, Spiegel Binance ~394–449)

```cpp
ExecutionBridge::deps d;
d.order_tx = make_bybit_rest_order_transport(rest_);
d.fills_tx = bybit_user_data_;  // private WS
d.encoder  = std::make_shared<BybitFuturesOrderEncoder>(symbol_);
d.parser   = std::make_shared<BybitFuturesUserDataParser>();
d.order_rate_limiter = TokenBucketRateLimiter(/*cap=*/10, /*refill=*/10);
// Bybit linear create default ~10 req/s — konservativ starten
d.client_id_fn = [m=minter_](uint64_t){ return m->next(); }; // max 36 chars!
d.position_snapshot_handler = /* log + funding */;
bridge_ = std::make_shared<ExecutionBridge>(std::move(d));
```

**Private WS (`bybit_user_data_transport.h`)**

1. Connect `wss://stream-testnet.bybit.com/v5/private`
2. Auth:
   ```json
   {"op":"auth","args":["<api_key>", <expires_ms>, "<sig>"]}
   ```
   `sig = HMAC_SHA256(secret, "GET/realtime" + expires)`
3. Subscribe:
   ```json
   {"op":"subscribe","args":["order.linear","execution.linear","position.linear","wallet"]}
   ```
   **Nicht** All-In-One und categorised mischen.
4. Ping alle 20s: `{"op":"ping"}`
5. Kein listenKey/keepalive-PUT (Binance-spezifisch entfällt)
6. Fatal disconnect → halt callback

**Fill-Wahrheit:** `execution` topic → `parsed_exec` (fills); `order` topic → ack/cancel/reject; `position`/`wallet` → `parsed_position_snapshot`.

**DoD Phase 2:** Encoder + mock transport tests; user-data parser fixtures; testnet place+cancel manuell (gated test).

---

### Phase 3 — Reconciler + Kill Switch + DMS/Watchdog

#### Reconciler (`BybitFuturesReconciler`)

```
GET /v5/position/list?category=linear&symbol=BTCUSDT
  → size + side → signed_qty = (side=="Buy" ? +size : side=="Sell" ? -size : 0)
GET /v5/account/wallet-balance?accountType=UNIFIED&coin=USDT
  → totalAvailableBalance (oder coin[].walletBalance — dokumentiere Wahl + Test)
Compare vs portfolio cash + position.qty within tolerance_bps
Non-empty string on drift/HTTP/retCode fail  → engine refuses start
```

**Kein** Spot-Testnet-Reset-Heuristic (wie Futures-Binance-Kommentar).

#### Kill Switch Sequenz (fail-closed, deadline-bound)

```
1. set_per_call_timeout(min(1500ms, deadline/3))
2. POST /v5/order/cancel-all
   body: {"category":"linear","symbol":"BTCUSDT"}
   // optional orderFilter weglassen = alle Typen
3. GET /v5/position/list?category=linear&symbol=BTCUSDT
4. if |signed_qty| > eps:
     POST /v5/order/create
     {
       "category":"linear","symbol":"BTCUSDT",
       "side": qty>0 ? "Sell" : "Buy",
       "orderType":"Market",
       "qty": "<abs>",
       "reduceOnly": true,
       "positionIdx": 0,
       "orderLinkId": "<minted>"
     }
5. Any step fail past deadline → return false (operator intervene)
```

**Hinweis:** Bybit erlaubt laut Docs `qty="0"` + `reduceOnly` + `closeOnTrigger` für Full-Close — **nur verwenden wenn in Testnet verifiziert**; sonst explizite size aus position list (robuster, Spiegel Binance).

#### DMS (§10 für Details)

- Wenn DCP verfügbar (institutional): `POST /v5/order/disconnected-cancel-all` + private WS `dcp.future`
- Sonst: **process-local** heartbeat + `WorkerWatchdog` + optional reduceOnly flatten (wie Binance Phase-3 `dms_attempt_position_close`)
- Arm **nach** bridge open; disarm **bei** close vor kill path

**DoD Phase 3:** Unit tests mit fake `post_fn`/`get_fn`; kill-switch canned responses; DMS start fail → provider `open()` false.

---

### Phase 4 — Brackets + Risk Check + Testnet Live Ritual

1. **`FuturesRiskCheck`**: gleiche Config-Keys wie Binance; instruments-info für tick/lot optional in `get_instrument()`.
2. **Brackets:**  
   - Bybit native TP/SL auf Entry (`takeProfit`/`stopLoss` in create) **oder** separate conditional orders.  
   - Spiegel Binance: zwei reduceOnly legs, partial `qty_fraction` decline.  
   - Capabilities: `stop_market=true`, `oco=false` bis atomare Placement belegt ist.
3. **Position mode gate:**  
   - Query positions / account; wenn hedge (`positionIdx` 1/2 aktiv oder switch-mode=3) → **refuse live**.  
   - One-way: immer `positionIdx=0` im Encoder.
4. **Live ritual (testnet):**
   - Tiny notional, attended, keys via env
   - Shadow 4h evidence optional analog Governance
   - Kill-switch manuell triggern und Position flat verifizieren
5. Docs: `docs/operations/` Kurz-Runbook (optional, nicht Teil Freeze)

**DoD Phase 4:** siehe §13.

---

## 8. Parser Field Maps

### 8.1 Public Trade → `provider::tick`

**Wire (WS):** topic `publicTrade.BTCUSDT`

```json
{
  "topic": "publicTrade.BTCUSDT",
  "type": "snapshot",
  "ts": 1672304486868,
  "data": [
    {
      "T": 1672304486865,
      "s": "BTCUSDT",
      "S": "Buy",
      "v": "0.001",
      "p": "16578.50",
      "i": "20f43950-…",
      "BT": false,
      "seq": 1783284617
    }
  ]
}
```

| Bybit field | `provider::tick` | Notes |
|-------------|------------------|-------|
| `T` | `timestamp` | ms → `system_clock` |
| `s` | `symbol` | uppercase |
| `p` | `price` | `from_chars` double |
| `v` | `quantity` | **scaled:** `int64_t(qty * qty_scale_)` mit default `1e8` (wie Binance trade parser) |
| `S` | `side` | Taker side: `"Buy"` → `side=0` (bid aggressor? **festlegen und testen**): Empfehlung Spiegel Binance `m` mapping: Buy-taker = aggressive buy → `side=1` (ask hit) **oder** dokumentiere Engine-Konvention. **Konkret vorschreiben:** `S=="Buy"` → `side=1` (taker buy), `S=="Sell"` → `side=0` (taker sell); Unit-Test fixiert das. |
| `data[]` | N ticks | Ein WS-Frame kann **viele** Trades enthalten → Parser muss **Liste** liefern oder CombinedParser emittiert nur ersten und Transport splittet; **Empfehlung:** Transport/Parser-API: `parse_records` multi-emit über Engine-Bridge, oder in `parse_record` den Frame in Zeilen splitten. Minimal: iteriere `data[]` und publishiere N Events (Engine-Bridge muss das können — prüfe `data_bridge`; falls nur 1:1, split im Transport in N frames). |

**Action for implementer:** Lies `src/providers/data_bridge.h` — wenn 1 Frame → 1 Event, dann Transport splittet Trades in separate queued lines.

### 8.2 Orderbook → `provider::l2_snapshot` / `l2_update`

**Topic:** `orderbook.{depth}.{symbol}`  
Depth linear: `1`, `50`, `200`, `1000`.

| Bybit field | TrueTest | Notes |
|-------------|----------|-------|
| `type=="snapshot"` | `l2_snapshot` | Full book replace |
| `type=="delta"` | `l2_update` **oder** snapshot-merge | Engine/orderbook erwartet ggf. snapshot; **wie Binance depthUpdate:** bevorzugt `l2_snapshot` mit nur geänderten levels **oder** true delta. Spiegel `binance_depth_parser.h`: oft snapshot-shaped. **Empfehlung Phase 1:** snapshot + delta beide als `l2_snapshot` mit partial levels (qty=0 = delete), Engine-OB merged — verifiziere gegen `orderbook` apply path. |
| `data.s` | `symbol` | |
| `data.b[]` | `bids` | Array `[price, size]` strings |
| `data.a[]` | `asks` | |
| `ts` / `cts` | `timestamp` | prefer `cts` (matching trades) if present |
| size `"0"` | level delete | qty=0 |

### 8.3 Kline → `provider::bar`

**Topic:** `kline.{interval}.{symbol}`  
Intervals: `1,3,5,15,30,60,120,240,360,720,D,W,M`.

| Bybit `data[]` field | `provider::bar` |
|----------------------|-----------------|
| `start` | `date` (ISO oder epoch-string — **match existing bar_record convention** in Binance parser) |
| symbol from topic | `symbol` |
| `open` | `open` |
| `high` | `high` |
| `low` | `low` |
| `close` | `close` |
| `volume` | `volume` (scale to int64) |
| `confirm` | nur closed candles emittieren wenn `confirm==true` (wie Binance final kline) |

### 8.4 Private Execution → `parsed_exec`

**Topic:** `execution` / `execution.linear`

| Bybit field | `parsed_exec` |
|-------------|---------------|
| `orderLinkId` | `client_order_id` |
| `orderId` | `exchange_order_id` |
| `symbol` | `symbol` |
| `side` | `side` (`Buy`/`Sell`) |
| `execQty` | `last_fill_qty` |
| `execPrice` | `last_fill_price` |
| `execFee` | `commission` |
| `feeCurrency` | `commission_asset` |
| `execTime` | `ts` |
| `execType` | kind mapping: `Trade` → partial/full via leavesQty; `Funding` → nicht als fill (→ snapshot path) |
| `leavesQty` | `0` → `full_fill`, else `partial_fill` if Trade |
| cum: optional aus `order` topic `cumExecQty` | `cumulative_qty` |

### 8.5 Private Order → `parsed_exec` (acks)

| Bybit `orderStatus` | `parsed_exec::kind` |
|---------------------|---------------------|
| `New` / `Created` | `ack` |
| `PartiallyFilled` | `partial_fill` (qty oft 0 hier — prefer execution topic) |
| `Filled` | `full_fill` |
| `Cancelled` | `canceled` |
| `Rejected` | `rejected` + `rejectReason` → `error` |
| `Deactivated` / expired | `expired` |

### 8.6 Position / Wallet → `parsed_position_snapshot`

| Bybit | snapshot field |
|-------|----------------|
| position `symbol` | `positions[].symbol` |
| `side`+`size` | `positions[].qty` signed |
| `positionIdx` | map to `position_side`: 0→`BOTH`, 1→`LONG`, 2→`SHORT` |
| wallet coin `USDT` `walletBalance` | `balances[].wallet_balance` |
| funding via execution `execType=Funding` | `reason=funding_fee`, `balance_change` |

---

## 9. Order Encode Table

`BybitFuturesOrderEncoder` erzeugt **JSON objects** (nicht query-string).  
`encoded_order.endpoint` = path; `wire_payload` = JSON body string.

### 9.1 Submit (`encode_submit`)

| `order_event` | Bybit JSON field | Werte |
|---------------|------------------|-------|
| (const) | `category` | `"linear"` |
| `get_symbol()` | `symbol` | UPPER |
| `get_side()` | `side` | `buy`→`"Buy"`, `sell`→`"Sell"` |
| `get_order_type()` | `orderType` | `market`→`Market`, `limit`→`Limit` |
| `get_quantity()` | `qty` | string `%.8f` (trim trailing zeros optional) |
| `get_price()` | `price` | nur Limit/stop_limit |
| `get_tif()` | `timeInForce` | `GTC`/`IOC`/`FOK`; Market → typisch weglassen (Venue IOC) |
| `get_stop_price()` | `triggerPrice` | für conditional; `orderType` ggf. anders — **Phase 2: nur Market+Limit**; Stop in Phase 4 |
| client id | `orderLinkId` | **max 36 chars** (ClientOrderIdMinter kürzen!) |
| (const) | `positionIdx` | `0` (one-way only) |
| (optional kill/close) | `reduceOnly` | `true` nur wenn Encoder-API erweitert oder Kill-Switch raw post |

```cpp
// endpoint
e.endpoint = "/v5/order/create";
// wire_payload example
// {"category":"linear","symbol":"BTCUSDT","side":"Buy","orderType":"Limit",
//  "qty":"0.001","price":"50000","timeInForce":"GTC","positionIdx":0,
//  "orderLinkId":"tt-…"}
```

### 9.2 Cancel (`encode_cancel`)

| Input | Bybit field |
|-------|-------------|
| | `category=linear` |
| symbol | `symbol` |
| exchange_order_id | `orderId` (prefer) |
| client_order_id | `orderLinkId` |
| endpoint | `/v5/order/cancel` |
| HTTP | **POST** (OrderTransport `cancel()` mappt auf `post_json`, nicht DELETE!) |

### 9.3 Amend (optional Phase 2.5)

`POST /v5/order/amend` — nur wenn `IOrderEncoder` erweitert wird; sonst out of scope. Engine hat ggf. kein generic amend — **nicht erfinden**.

### 9.4 Cancel-all / Kill (nicht Encoder, raw REST)

`POST /v5/order/cancel-all` + `{"category":"linear","symbol":"BTCUSDT"}`

### 9.5 Response mapping (`BybitRestOrderTransport`)

```
HTTP 2xx && retCode==0 → ok
  exchange_order_id = result.orderId (hand-extract)
else → error = "HTTP … retCode=… " + redact(body)
```

---

## 10. Safety Implementation Recipes

### 10.1 Kill-Switch (verbindliche Sequenz)

Siehe Phase 3. Zusätzliche Regeln:

- **Kein Retry-Loop** mit Backoff bei HTTP 5xx — ein Versuch pro Step innerhalb Deadline, dann `return false`.
- `reduceOnly=true` **immer** beim Flatten.
- Logging: `redact_for_log(body, 240)` auf stderr.
- Nach Kill: Engine halt bleibt terminal; kein Auto-Clear.

### 10.2 DMS / DCP Strategie

#### Fakt aus Bybit-Docs (nicht erfinden)

- **DCP** (`POST /v5/order/disconnected-cancel-all`, WS topic `dcp.future` / `dcp.spot` / `dcp.option`):  
  - **Institutional clients only** (Apply via client manager; VIP allein reicht nicht).  
  - Window 3–300s (default oft 10s).  
  - Cancelt **Orders**, nicht zwingend Positionen (analog Binance countdownCancelAll).  
  - Query: `GET /v5/account/query-dcp-info`.

#### TrueTest Default für Retail/Testnet (kein DCP)

Implementiere `BybitFuturesDeadMansSwitch` als **layered local DMS**:

| Layer | Mechanismus | Schützt |
|-------|-------------|---------|
| L1 | Private-WS + public-WS ping + `WorkerWatchdog` liveness | hung threads → engine halt |
| L2 | Heartbeat-Thread: alle `heartbeat_ms` Health-Check (z. B. `GET /v5/market/time` unsigned oder signed wallet) | process alive |
| L3 | Bei 2× consecutive fail + `dms_attempt_position_close`: reduceOnly MARKET flatten (wie Binance Phase 3) | Position |
| L4 | Orderly `close()`: kill-switch cancel+flatten | clean shutdown |
| L5 | Optional DCP wenn `dcp_available` config + arm success | Orders nach total death |

```
start():
  if dcp_enabled && account_has_dcp:
     POST disconnected-cancel-all {product:DERIVATIVES, timeWindow: secs}
     subscribe dcp.future on private WS
     if fail → refuse live (fail-closed)
  else:
     log "DCP unavailable — local DMS only (orders NOT auto-cancelled on SIGKILL)"
     // NO silent pretend-DCP
  spawn heartbeat thread; update last_beat_ms_
  return true

// WICHTIG: Ohne DCP darf open() trotzdem live gehen NUR wenn
// operator --dead-man-countdown-ms > 0 und local DMS armed;
// oder explicit --disarm-deadman für supervised testnet.
// Empfohlen: default countdown 30000 wie Binance main.inc.
```

**Niemals** countdown adaptiv verlängern unter Load (S8).

### 10.3 Reconciler

- Default refuse on any REST/retCode/parse error.
- Cash field: bevorzuge `totalAvailableBalance` aus UNIFIED wallet; **Unit-Test mit realem canned JSON** aus Testnet-Capture.
- Position: signed size; zero-size rows ok.

### 10.4 Pre-live gates (in `open()`)

1. Clock skew vs `/v5/market/time` (refuse if outside recv_window policy, z. B. >1s)
2. `GET /v5/market/instruments-info?category=linear&symbol=X` — Symbol Trading
3. Hedge mode refuse
4. Optional leverage read + warn if > `max_leverage` config
5. Reconciler (engine-side after open — existing engine flow)
6. DMS arm last

---

## 11. Config Keys / CLI Flags / Env Vars

### Env Vars (erweitern `resolve_exchange_credentials` in `main.inc`)

| Variable | Zweck |
|----------|-------|
| `TRUETEST_BYBIT_API_KEY` | API key (preferred over argv) |
| `TRUETEST_BYBIT_API_SECRET` | API secret |
| Fallback | bestehende `TRUETEST_BINANCE_*` **nicht** wiederverwenden — getrennte Namespaces |

Resolution order:

1. Env `TRUETEST_BYBIT_API_KEY` / `_SECRET` wenn provider bybit*
2. Else `--api-key` / `--api-secret`
3. Warn if argv used (argv visible in `ps`)

### CLI (reuse existing flags where possible)

| Flag | Mapping |
|------|---------|
| `--provider bybit-futures` | registry name |
| `--symbol BTCUSDT` | symbol |
| `--stream trade` | primary MD |
| `--depth-stream orderbook.50` | depth topic prefix |
| `--testnet` | testnet endpoints |
| `--demo` | **neu** optional demo endpoints |
| `--api-key` / `--api-secret` | credentials |
| `--host` / `--port` | override |
| `--max-notional-usdt` etc. | risk (reuse) |
| `--dead-man-countdown-ms` | DMS |
| `--disarm-deadman` | countdown=0 |
| `--live` | engine_live only |

### JSON config keys (main.inc `get_str`)

Gleiche Keys wie `provider_config` in §6.

---

## 12. Test Plan

### Unit (kein Netzwerk)

| Test | Inhalt |
|------|--------|
| `test_bybit_auth` | HMAC vectors REST + WS `GET/realtime` |
| `test_bybit_endpoints` | mainnet/testnet/demo host strings; `looks_like_testnet_host` |
| `test_bybit_parser` | canned publicTrade → tick; multi-trade array |
| `test_bybit_depth_parser` | snapshot + delta qty=0 delete |
| `test_bybit_combined_parser` | topic routing |
| `test_bybit_futures_order_encoder` | Market/Limit/Cancel JSON exact string match |
| `test_bybit_rest_order_transport` | mock post; retCode≠0 → error |
| `test_bybit_futures_user_data_parser` | execution/order/position fixtures |
| `test_bybit_futures_reconciler` | cash/pos drift / happy path |
| `test_bybit_futures_kill_switch` | sequence with fake REST |
| `test_bybit_futures_dead_mans_switch` | arm fail; heartbeat liveness |
| `test_bybit_futures_register` | missing symbol throws; testnet flag |

### Mock transport

- Inject `request_fn` lambdas (Pattern aus `BinanceRestOrderTransport` / DMS `post_fn`).
- Keine echten Sockets in Unit-Tests.

### Integration / Testnet (gated)

```cpp
// test_bybit_futures_testnet_live.cpp
#ifdef HAS_BYBIT
TEST(...) {
  if (!std::getenv("TRUETEST_BYBIT_TESTNET_LIVE")) GTEST_SKIP();
  // place min qty, cancel, assert retCode 0
}
#endif
```

Env: `TRUETEST_BYBIT_TESTNET_LIVE=1`, keys gesetzt.

### Scripts nach Implementierung

```bash
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
ctest --test-dir build -R 'bybit|Bybit' --output-on-failure
```

---

## 13. Acceptance Criteria / Definition of Done

- [ ] `ENABLE_BYBIT=ON` baut `engine_{backtest,shadow,live}` + Tests
- [ ] `ProviderRegistry` listet `bybit-futures` (und optional `bybit`)
- [ ] **Kein** `HAS_BYBIT` / Bybit-Include in `src/engine/`, `src/core/`, `src/threading/`, generischem `src/risk/`
- [ ] Hand-Parser ohne `nlohmann` auf MD/fill-parse Pfaden
- [ ] Shadow: live MD trades (+ optional L2) ≥5 min stabil
- [ ] Live testnet: place limit → see ack on private WS → cancel → kill-switch flattens
- [ ] Reconciler refuses on intentional cash drift fixture
- [ ] DMS: arm failure → `open()` false; heartbeat updates liveness atomic
- [ ] One-way only; hedge → refuse
- [ ] Rate limiter ≤ ~10 order/s linear default
- [ ] `check-hotpath-json`, `check-layer-deps`, `check-live-safety-freeze` grün
- [ ] Unit tests grün ohne Netzwerk
- [ ] Docs/Runbook: Testnet keys + Ritual (kurz)
- [ ] Keine erfundenen API-Features; DCP institutional limitation dokumentiert im Log

---

## 14. Pitfalls & Bybit-Specific Quirks

| # | Pitfall | Mitigation |
|---|---------|------------|
| 1 | **JSON body + header sign** ≠ Binance query sign | Eigene `BybitRestClient`; Tests mit canonical strings |
| 2 | **Cancel ist POST**, nicht DELETE | `IOrderTransport::cancel` → `post_json` |
| 3 | **Success = retCode 0**, nicht nur HTTP 200 | Immer `retCode` parsen |
| 4 | **Side casing** `Buy`/`Sell` vs `BUY`/`SELL` | Encoder/Parser exakt Bybit |
| 5 | **Immer `category=linear`** | Jeder Request |
| 6 | **positionIdx** in hedge required; one-way `0` | Gate hedge out |
| 7 | **orderLinkId ≤ 36** | Minter prefix `tt` + kurze id |
| 8 | **Private WS auth** message, kein listenKey | Anderer Transport als Binance |
| 9 | **App ping 20s** sonst Disconnect | Eigenes ping; `max_active_time` query optional |
| 10 | **DCP institutional only** | Local DMS + ehrliches Logging; kein Fake-DCP |
| 11 | **DCP/DMS cancelt Orders, nicht Position** | Kill-switch flatten + optional `dms_attempt_position_close` |
| 12 | **Position size unsigned + side** | Signed conversion zentral in einer Helper-Funktion |
| 13 | **UTA wallet fields** | Canned real responses; `totalAvailableBalance` vs coin wallet |
| 14 | **Demo ≠ Testnet** | Getrennte Endpoints/Keys; Demo public MD = mainnet streams |
| 15 | **Regional hosts** (`.nl`, `.tr`, …) | Phase 0 nur global; override via `--host` |
| 16 | **US/CN IP 403** | Ops-Hinweis, kein Code-Workaround |
| 17 | **Multi-trade frames** | Split vor Engine |
| 18 | **Orderbook delta** | qty 0 = delete; apply order korrekt |
| 19 | **Rate limit headers** `X-Bapi-Limit-*` | Optional log; TokenBucket 10/s |
| 20 | **recv_window / clock** | Time sync at live open; refuse skew |
| 21 | **reduceOnly + TP/SL** | Docs: reduceOnly cannot set TP/SL together — Brackets separate |
| 22 | **Qty strings / lot filters** | instruments-info `qtyStep` vor Live; min notional |
| 23 | **Freeze surface** | Bybit-Safety-Dateien neu; `engine.cpp` unangetastet lassen wenn möglich — Wiring nur über `IProvider` |
| 24 | **HybridExecutor reuse** | Nicht Binance-Header in Bybit-Provider hard-coden wenn er `HAS_BINANCE` braucht — paper path ggf. generischen Executor nutzen |

---

## 15. References (official URLs)

### Bybit V5

| Topic | URL |
|-------|-----|
| V5 Guide / Auth | https://bybit-exchange.github.io/docs/v5/guide |
| Intro | https://bybit-exchange.github.io/docs/v5/intro |
| WS Connect | https://bybit-exchange.github.io/docs/v5/ws/connect |
| Public Trade | https://bybit-exchange.github.io/docs/v5/websocket/public/trade |
| Public Orderbook | https://bybit-exchange.github.io/docs/v5/websocket/public/orderbook |
| Public Kline | https://bybit-exchange.github.io/docs/v5/websocket/public/kline |
| Private Order | https://bybit-exchange.github.io/docs/v5/websocket/private/order |
| Private Execution | https://bybit-exchange.github.io/docs/v5/websocket/private/execution |
| Private Position | https://bybit-exchange.github.io/docs/v5/websocket/private/position |
| Private Wallet | https://bybit-exchange.github.io/docs/v5/websocket/private/wallet |
| DCP WS | https://bybit-exchange.github.io/docs/v5/websocket/private/dcp |
| Create Order | https://bybit-exchange.github.io/docs/v5/order/create-order |
| Amend Order | https://bybit-exchange.github.io/docs/v5/order/amend-order |
| Cancel Order | https://bybit-exchange.github.io/docs/v5/order/cancel-order |
| Cancel All | https://bybit-exchange.github.io/docs/v5/order/cancel-all |
| DCP set | https://bybit-exchange.github.io/docs/v5/order/dcp |
| DCP info | https://bybit-exchange.github.io/docs/v5/account/dcp-info |
| Wallet Balance | https://bybit-exchange.github.io/docs/v5/account/wallet-balance |
| Account Info | https://bybit-exchange.github.io/docs/v5/account/account-info |
| Position List | https://bybit-exchange.github.io/docs/v5/position |
| Position Mode | https://bybit-exchange.github.io/docs/v5/position/position-mode |
| Set Leverage | https://bybit-exchange.github.io/docs/v5/position/leverage |
| Instruments Info | https://bybit-exchange.github.io/docs/v5/market/instrument |
| Rate Limit | https://bybit-exchange.github.io/docs/v5/rate-limit |
| Demo Trading | https://bybit-exchange.github.io/docs/v5/demo |
| Enums | https://bybit-exchange.github.io/docs/v5/enum |

### Endpoints (quick)

| Env | REST | Public WS linear | Private WS |
|-----|------|------------------|------------|
| Mainnet | `https://api.bybit.com` | `wss://stream.bybit.com/v5/public/linear` | `wss://stream.bybit.com/v5/private` |
| Testnet | `https://api-testnet.bybit.com` | `wss://stream-testnet.bybit.com/v5/public/linear` | `wss://stream-testnet.bybit.com/v5/private` |
| Demo | `https://api-demo.bybit.com` | public: mainnet streams | `wss://stream-demo.bybit.com` |

API keys:  
- Mainnet: https://www.bybit.com/app/user/api-management  
- Testnet: https://testnet.bybit.com/app/user/api-management  

### TrueTest Code References (absolute)

| Role | Path |
|------|------|
| IProvider | `/home/leonard/work/projects/truetest/core/src/providers/provider.h` |
| Registry | `/home/leonard/work/projects/truetest/core/src/providers/provider_registry.h` |
| Events | `/home/leonard/work/projects/truetest/core/src/providers/provider_event.h` |
| Live safety | `/home/leonard/work/projects/truetest/core/src/execution/live_safety.h` |
| ExecutionBridge | `/home/leonard/work/projects/truetest/core/src/execution/execution_bridge.h` |
| Fill parser types | `/home/leonard/work/projects/truetest/core/src/execution/fill_parser.h` |
| Golden futures provider | `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_provider.h` |
| Futures register | `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_register.cpp` |
| CMake deps | `/home/leonard/work/projects/truetest/core/cmake/Dependencies.cmake` |
| Layer deps | `/home/leonard/work/projects/truetest/core/scripts/check-layer-deps.sh` |
| Agent rules | `/home/leonard/work/projects/truetest/core/AGENTS.md` |

---

## Appendix A — Minimal `open()` Live Pseudocode

```cpp
bool BybitFuturesProvider::open() {
  state_ = lifecycle::opening;
  build_risk_check_if_needed();
  auto pub = make_public_transport(); // subscribe publicTrade[+orderbook]
  apply_halt_cb(pub);

  if (mode_ == engine_mode::live && !api_key_.empty()) {
    rest_ = std::make_shared<BybitRestClient>(api_key_, api_secret_, endpoints_);
    if (!rest_->verify_clock_skew()) return fail("clock skew");
    if (!probe_instrument(symbol_)) return fail("unknown symbol");
    if (is_hedge_mode(rest_)) return fail("hedge mode");
    minter_ = std::make_shared<ClientOrderIdMinter>("tt", seed_); // ensure ≤36
    order_rate_limiter_ = std::make_shared<TokenBucketRateLimiter>(10.0, 10.0);
    reconciler_ = std::make_shared<BybitFuturesReconciler>(rest_, symbol_);
    kill_switch_ = std::make_shared<BybitFuturesKillSwitch>(rest_, symbol_, minter_);
    // bracket_adapter_ = ... Phase 4

    ExecutionBridge::deps d;
    d.order_tx = make_bybit_rest_order_transport(rest_);
    d.fills_tx = std::make_shared<BybitUserDataTransport>(rest_, endpoints_);
    d.encoder  = std::make_shared<BybitFuturesOrderEncoder>(symbol_);
    d.parser   = std::make_shared<BybitFuturesUserDataParser>();
    d.order_rate_limiter = order_rate_limiter_;
    d.client_id_fn = [m=minter_](uint64_t){ return m->next(); };
    bridge_ = std::make_shared<ExecutionBridge>(std::move(d));
    if (!bridge_->open()) return fail(bridge_->last_error());
    executor_ = bridge_;

    if (dead_man_countdown_ms_ > 0) {
      dms_ = make_bybit_dms(...);
      if (!dms_->start()) { bridge_->close(); return fail("DMS arm"); }
    }
  } else if (mode_ == engine_mode::shadow) {
    executor_ = std::make_shared<TradeTapeShadowAdapter>(...);
  } else {
    // paper hybrid
  }

  if (!pub->open()) return fail("public WS");
  transport_ = pub;
  state_ = lifecycle::open;
  return true;
}
```

---

## Appendix B — Uncertainty Log (nicht raten — vor Live verifizieren)

| Item | Status |
|------|--------|
| Exaktes Wallet-Feld für Reconciler-Cash (`totalAvailableBalance` vs coin `walletBalance`) | Mit Testnet-Capture bestätigen |
| Full-close `qty=0`+`reduceOnly`+`closeOnTrigger` Verhalten UTA | Vor Kill-Switch-Prod testen; Fallback = explicit size |
| Demo private path exact (`/v5/private` auf stream-demo) | Docs: `wss://stream-demo.bybit.com` — path beim Connect verifizieren |
| Hedge detection Endpoint (UTA 2.0) | Über `position/list` positionIdx und/oder switch-mode state; Account-Info lesen |
| Orderbook apply: delta vs snapshot in Engine OB | Gegen `orderbook` Code + Binance depth path abgleichen |
| Funding event mapping aus execution vs wallet | Phase 2 log-only zuerst (wie Binance funding handler) |

---

*Ende des Guides. Implementierer: Phase 0 starten, pro Phase DoD erfüllen, Binance-Futures-Dateien als strukturelle Vorlage öffnen und Zeile-für-Zeile mappen — Bybit-Wire-Format und Auth sind die echten Deltas, nicht die Engine-Architektur.*
