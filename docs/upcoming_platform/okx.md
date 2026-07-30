# OKX Futures Provider — Grok Build Implementation Guide

**Zielpfad:** `docs/upcoming_platform/okx.md`  
**Workspace:** `/home/leonard/work/projects/truetest/core`  
**Golden Reference:** `src/providers/binance/*` (USDT-M Futures)  
**API-Quelle:** [OKX API v5](https://www.okx.com/docs-v5/en/) — nur dokumentierte Features, keine Erfindungen.

---

## 1. Goal & Scope

### Scope (Phase-1 Produktziel)

| In Scope | Out of Scope (später) |
|----------|------------------------|
| **USDT-M Perpetual SWAP** (`instType=SWAP`, z.B. `BTC-USDT-SWAP`, linear `ctType=linear`) | Coin-margined Inverse (`BTC-USD-SWAP`) |
| Public Market Data: trades, books/books5, candles | Options, Spread, RFQ, Block |
| Shadow/Paper Execution (Hybrid + TradeTape) | Multi-symbol portfolio margin |
| Live Execution über `ExecutionBridge` (REST place/cancel + private WS fills) | VIP-only `fills` channel (VIP4+), SBE binary books |
| Safety: Reconciler, Kill-Switch, DMS (`cancel-all-after`), `FuturesRiskCheck` | Auto-flip von `posMode` / Leverage |
| Brackets via `attachAlgoOrds` oder separate algo orders | Partial scale-out TP1/TP2 mit `qty_fraction < 1` |
| Demo Trading (`x-simulated-trading: 1` + `wspap.okx.com`) | Withdraw/Deposit, Sub-account copy trading |

### Warum SWAP zuerst

Binance-Futures-Provider (`BinanceFuturesProvider`) ist die Referenz für:

- `IProvider` + 4 Safety Hooks (`IReconciler`, `IKillSwitch`, `IRiskCheck`, `IBracketAdapter`)
- Live-Pfad: `ExecutionBridge` + `IOrderEncoder` + `IFillParser` + REST + private User-Data WS
- DMS-Pattern, Kill-Switch flatten, net-mode only, fail-closed open()

OKX mappt 1:1 auf diese Architektur — **nur** unter `src/providers/okx/`, nie mit `HAS_OKX` in `engine/`, `core/`, `threading/`, `risk/` (außer generischem `FuturesRiskCheck`).

---

## 2. Non-Negotiables (TrueTest)

Aus `AGENTS.md`, `docs/architecture/01-target-architecture.md`, Freeze-Scripts:

| ID | Regel | Konsequenz für OKX |
|----|-------|--------------------|
| **R1–R2** | Zero heap / no runtime grow on hot path | Parser: kein `nlohmann::json`; string_view extract wie `binance::extract_*` |
| **R3** | SPSC sole producer | Market-data WS → engine loop sole consumer; keine zweiten Producer auf Engine-Ringen |
| **R6** | No JSON on hot path outside allow-list | JSON-Parsing nur in Provider-Parsern (kalt relativ zur Strategy); kein Expand von `check-hotpath-json.sh` ohne Ask |
| **S2** | Compile-time live gate | Live-Orders nur in `engine_live`; OKX-Code ändert **nicht** `tt_target.h` |
| **S3** | Halt write-once terminal | Fatal WS disconnect → `set_halt_callback` → `engine::trigger_halt` |
| **S4** | Kill/DMS/Reconciler loud, non-retrying, fail-closed | Kein Auto-Resume nach Kill; DMS-Arm-Fail → `open()` returns false |
| **S6** | Reconciler default-refuse | Drift → non-empty error string → Engine startet nicht |
| **S9** | No `HAS_*` venue leakage into core | `HAS_OKX` nur in `src/providers/okx/*` und CMake wiring |
| **Provider sole extension** | Layer graph | `scripts/check-layer-deps.sh`: `providers` darf `execution`, `exits`, `risk`, `engine` nutzen — umgekehrt nicht |

### Freeze-Disziplin

Die **bestehenden** Freeze-Dateien (`scripts/check-live-safety-freeze.sh`) bleiben unberührt, solange OKX **eigene** Dateien hat:

```
src/providers/okx/okx_futures_provider.h
src/providers/okx/okx_futures_dead_mans_switch.h
src/providers/okx/okx_futures_kill_switch.h
src/providers/okx/okx_futures_reconciler.h
```

**Nicht** anfassen ohne CCB: `src/execution/live_safety.h`, `src/risk/futures_risk_check.h`, `src/engine/engine.cpp`, `src/core/tt_target.h`.

Neue OKX-Safety-Dateien sind **noch nicht** im Freeze-Set. Nach Phase-3-Live-Härtung: in Freeze-Script + `docs/governance/02-prerequisites.md` aufnehmen (T3-Protokoll).

---

## 3. Architecture Mapping: OKX → TrueTest Interfaces

### 3.1 Interface-Stack (Ist-Code)

| Interface | Datei | Rolle |
|-----------|-------|-------|
| `IProvider` | `src/providers/provider.h` | Composition root pro Venue |
| `IDataTransport` | `src/providers/transport.h` | Rohframes (WS/file) |
| `IDataParser<T>` | `src/providers/parser.h` | Line/Frame → typed |
| `provider::event` | `src/providers/provider_event.h` | `variant<bar, tick, l2_snapshot, l2_update, status>` |
| `IExecutionAdapter` | `src/execution/execution_adapter.h` | submit/poll/cancel |
| `ExecutionBridge` | `src/execution/execution_bridge.h` | Live: order_tx + fills_tx + encoder + parser |
| `IOrderEncoder` | `src/execution/order_encoder.h` | `order_event` → `encoded_order{endpoint, wire_payload, client_order_id}` |
| `IOrderTransport` | `src/execution/order_transport.h` | REST submit/cancel |
| `IFillTransport` | `src/execution/fill_transport.h` | Private WS push |
| `IFillParser` | `src/execution/fill_parser.h` | raw → `parsed_exec` / `parsed_position_snapshot` |
| `IReconciler` / `IKillSwitch` | `src/execution/live_safety.h` | Startup gate / emergency flatten |
| `IRiskCheck` | `src/risk/futures_risk_check.h` | Pre-trade (venue-agnostisch nutzbar) |
| `IBracketAdapter` | `src/exits/bracket_adapter.h` | Venue-resting SL/TP |

### 3.2 OKX → Komponenten (Soll)

```
                    ┌─────────────────────────────────────────┐
                    │         OkxFuturesProvider : IProvider  │
                    │  open()/close()/configure()             │
                    │  get_transport / get_execution_adapter  │
                    │  get_reconciler / get_kill_switch       │
                    │  get_risk_check / get_bracket_adapter   │
                    │  get_liveness_sources / set_halt_cb     │
                    └───────────────┬─────────────────────────┘
           ┌────────────────────────┼────────────────────────┐
           v                        v                        v
  OkxPublicTransport        ExecutionBridge (live)     OkxFuturesDeadMansSwitch
  + OkxCombinedParser       ├ OkxRestOrderTransport    POST /api/v5/trade/cancel-all-after
  (trades+books)            ├ OkxPrivateWsTransport    heartbeat thread
  wss://ws.okx.com          ├ OkxFuturesOrderEncoder   get_liveness_sources()
  :8443/ws/v5/public        └ OkxUserDataParser
           │                        │
           │                        ├ OkxFuturesReconciler
           │                        │   GET account/balance + account/positions
           │                        ├ OkxFuturesKillSwitch
           │                        │   cancel-batch + close-position
           │                        └ OkxFuturesBracketAdapter
           │                            attachAlgoOrds / algo place
           v
  HybridExecutor / TradeTapeShadowAdapter  (backtest/shadow — reuse Binance helpers
                                             or shared execution adapters; no venue I/O)
```

### 3.3 Vier Safety Hooks (explizit)

| Hook | `IProvider` method | OKX Implementierung | Binance-Referenz |
|------|-------------------|---------------------|------------------|
| Reconciler | `get_reconciler()` | `OkxFuturesReconciler` | `binance_futures_reconciler.h` |
| Kill-Switch | `get_kill_switch()` | `OkxFuturesKillSwitch` | `binance_futures_kill_switch.h` |
| Risk Check | `get_risk_check()` | **reuse** `FuturesRiskCheck` (keine Venue-Logik) | gleich |
| Bracket | `get_bracket_adapter()` | `OkxFuturesBracketAdapter` | `binance_futures_bracket_adapter.h` |

Zusätzlich (nicht in `live_safety.h`, aber im Provider wie Binance):

| Extra | Rolle |
|-------|-------|
| `OkxFuturesDeadMansSwitch` | Server-side order cancel countdown |
| `get_liveness_sources()` | DMS-Heartbeat → `WorkerWatchdog` |
| `set_halt_callback()` | Fatal private/public WS loss → engine halt |

### 3.4 Mode-Dispatch in `open()` (kopiere Binance-Logik)

Aus `binance_futures_provider.h` `open()`:

| `engine_mode` | Execution |
|---------------|-----------|
| `live` + credentials | `ExecutionBridge` + REST + private WS + DMS + reconciler/kill/brackets |
| `shadow` | `TradeTapeShadowAdapter` |
| sonst (backtest/paper) | `HybridExecutor` / venue-lokaler paper executor |

Live-only Komponenten bleiben `nullptr` in shadow/backtest → Engine installiert `NoopReconciler` / `NoopKillSwitch`.

---

## 4. File Checklist — `src/providers/okx/`

Alles header-first wie Binance, außer Register-`.cpp`. Jede Datei mit `#ifdef HAS_OKX` … `#endif`.

```
src/providers/okx/
├── okx_endpoints.h                 # mainnet/demo hosts, ports, paths
├── okx_auth.h                      # HMAC-SHA256 Base64, ISO timestamp, sign helpers
├── okx_json.h                      # extract_sv_string/number, envelope code=="0"
│                                   # (kann anfangs aus binance_parser-Pattern klonen,
│                                   #  NICHT binance:: aus okx/ include — Copy/adapt)
├── okx_rest_client.h               # Boost.Beast HTTPS, signed headers, JSON body
├── okx_public_transport.h          # Public WS: connect, subscribe, ping/pong, read_frame
├── okx_combined_transport.h        # Optional: multi-channel public (trades+books)
├── okx_parser.h                    # trades → tick, candles → bar
├── okx_books_parser.h              # books/books5 → l2_snapshot / l2_update
├── okx_combined_parser.h           # IDataParser<provider::event>
├── okx_private_transport.h         # Private WS: login + subscribe orders/positions/account
├── okx_user_data_parser.h          # IFillParser (orders channel + account/positions)
├── okx_rest_order_transport.h      # IOrderTransport → POST place/cancel
├── okx_futures_order_encoder.h     # IOrderEncoder → JSON wire_payload
├── okx_futures_reconciler.h        # IReconciler
├── okx_futures_kill_switch.h       # IKillSwitch
├── okx_futures_dead_mans_switch.h  # cancel-all-after + heartbeat
├── okx_futures_bracket_adapter.h   # IBracketAdapter
├── okx_futures_safety.h            # Advisories: mgnMode, liq distance (optional)
├── okx_futures_provider.h          # IProvider composition root
├── okx_futures_register.cpp        # REGISTER_PROVIDER("okx" / "okx-futures")
└── (optional later)
    okx_backfill.h                  # REST history-candles prepend
    okx_executor.h                  # paper mid-price executor if needed
```

### Mapping 1:1 zu Binance-Dateien

| OKX | Binance-Referenz |
|-----|------------------|
| `okx_futures_provider.h` | `binance_futures_provider.h` |
| `okx_futures_register.cpp` | `binance_futures_register.cpp` |
| `okx_endpoints.h` | `binance_endpoints.h` |
| `okx_auth.h` | `binance_auth.h` |
| `okx_rest_client.h` | `binance_rest_client.h` |
| `okx_public_transport.h` | `binance_transport.h` |
| `okx_combined_parser.h` | `binance_combined_parser.h` |
| `okx_futures_order_encoder.h` | `binance_futures_order_encoder.h` |
| `okx_user_data_parser.h` | `binance_futures_user_data_parser.h` |
| `okx_private_transport.h` | `binance_user_data_transport.h` |
| `okx_rest_order_transport.h` | `binance_rest_order_transport.h` |
| `okx_futures_reconciler.h` | `binance_futures_reconciler.h` |
| `okx_futures_kill_switch.h` | `binance_futures_kill_switch.h` |
| `okx_futures_dead_mans_switch.h` | `binance_futures_dead_mans_switch.h` |
| `okx_futures_bracket_adapter.h` | `binance_futures_bracket_adapter.h` |

---

## 5. CMake: `ENABLE_OKX` / `HAS_OKX`

### 5.1 `CMakeLists.txt`

Neben `ENABLE_BINANCE`:

```cmake
option(ENABLE_OKX "Build with OKX exchange provider (USDT-M SWAP)" OFF)
```

### 5.2 `cmake/Dependencies.cmake` — in `tt_wire_optional_backends`

Pattern exakt wie Binance (Zeilen ~108–118):

```cmake
if(ENABLE_OKX)
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_sources(${target} PRIVATE
        ${_src}/providers/okx/okx_futures_register.cpp)
    target_link_libraries(${target} PUBLIC
        Boost::headers OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(${target} PUBLIC HAS_OKX)
endif()
```

**Kein** Glob. Register-`.cpp` ist der einzige Compile-Unit-Einstieg; Headers werden transitv via includes gezogen (wie Binance).

### 5.3 `cmake/Sources.cmake` — Tests

Unter `TEST_SOURCES` (oder conditional):

```cmake
if(ENABLE_OKX)
  list(APPEND TEST_SOURCES
    tests/test_okx_auth.cpp
    tests/test_okx_endpoints.cpp
    tests/test_okx_parser.cpp
    tests/test_okx_books_parser.cpp
    tests/test_okx_combined_parser.cpp
    tests/test_okx_futures_order_encoder.cpp
    tests/test_okx_user_data_parser.cpp
    tests/test_okx_futures_register.cpp
    tests/test_okx_futures_reconciler.cpp
    tests/test_okx_futures_kill_switch.cpp
    tests/test_okx_futures_dead_mans_switch.cpp
    tests/test_okx_futures_bracket_adapter.cpp
    # optional network-gated:
    # tests/test_okx_demo_live.cpp
  )
endif()
```

### 5.4 Build

```bash
cmake -B build -DENABLE_OKX=ON -DENABLE_BINANCE=ON -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -R 'okx|Okx' --output-on-failure
```

Abhängigkeiten: Boost.Beast (WS/HTTPS), OpenSSL (HMAC + TLS) — identisch zu Binance.

---

## 6. `REGISTER_PROVIDER` — `"okx"` / `"okx-futures"`

### 6.1 `okx_futures_register.cpp`

```cpp
#ifdef HAS_OKX

#include "providers/provider_registry.h"
#include "providers/okx/okx_futures_provider.h"
#include "providers/okx/okx_endpoints.h"

// Primary name (docs/CLI):
REGISTER_PROVIDER("okx-futures", [](const provider_config& cfg) {
    return make_okx_futures_provider(cfg);
});

// Alias (shorter operator spelling):
REGISTER_PROVIDER("okx", [](const provider_config& cfg) {
    return make_okx_futures_provider(cfg);
});

#endif
```

### 6.2 Pflicht-Keys in `provider_config`

| Key | Pflicht | Beispiel | Notes |
|-----|---------|----------|-------|
| `symbol` / `instId` | ja | `BTC-USDT-SWAP` | Intern immer `instId` canonical |
| `stream` | nein | `trades` (default) | Public channel name |
| `depth_stream` | nein | `books5` | Optional second channel |
| `api_key` | live | … | |
| `api_secret` | live | … | |
| `api_passphrase` | live | … | **OKX-spezifisch** (Binance hat das nicht) |
| `demo` / `testnet` | nein | `1` | Demo trading |
| `td_mode` | nein | `cross` (default) | `cross` \| `isolated` |
| `dead_man_countdown_ms` | nein | `60000` | → `timeOut` seconds clamp |
| `dead_man_heartbeat_ms` | nein | countdown/3 | |
| `max_notional_usdt` etc. | nein | | → `FuturesRiskCheck` |

Factory-Logik: 1:1 an `binance_futures_register.cpp` spiegeln (parse double/int64/bool, set_* am Provider).

### 6.3 `name()` override

```cpp
std::string name() const override { return "okx-futures"; }
```

Registry-Alias `"okx"` erstellt dieselbe Klasse; `name()` bleibt `"okx-futures"` für Dashboard/Logs.

---

## 7. Phased Implementation

### Phase 0 — Scaffold (kein Netzwerk in CI)

1. `okx_endpoints.h` mit Mainnet/Demo URLs (siehe §16).
2. `okx_auth.h`: Unit-Tests mit **offiziellen** Signatur-Beispielen aus der Doku (prehash = `timestamp + method + requestPath + body`).
3. `okx_futures_register.cpp` + leerer `OkxFuturesProvider` (nur `name()`, `has_data_feed()`, stubs).
4. CMake `ENABLE_OKX` verdrahten.
5. `REGISTER_PROVIDER` smoke: `tests/test_okx_futures_register.cpp` → `ProviderRegistry::instance().has("okx-futures")`.

**Exit:** `cmake -DENABLE_OKX=ON` + Registry-Test grün; `check-layer-deps.sh` clean.

### Phase 1 — Market Data

1. `OkxPublicTransport`:
   - Connect `wss://ws.okx.com:8443/ws/v5/public` (Demo: `wspap.okx.com`).
   - Nach Open: JSON subscribe `{"op":"subscribe","args":[{"channel":"trades","instId":"..."}]}`.
   - **Ping/Pong:** OKX sendet Text-`ping` ~alle 30s; Client antwortet mit Text-`pong` (oder umgekehrt — implementiere beides robust). Idle-Timeout wie Binance (~1.5–5s Beast options) anpassen, weil OKX seltener pingt.
   - `read_frame` liefert rohe JSON-Messages.
2. `OkxParser` / `OkxBooksParser` / `OkxCombinedParser` → `provider::event`.
3. Candles: Business-WS `wss://ws.okx.com:8443/ws/v5/business`, channel `candle1m` etc.
4. Provider `open()`: nur Transport + HybridExecutor; **kein** Live-REST.
5. CLI: `--provider okx-futures --symbol BTC-USDT-SWAP --stream trades`.

**Exit:** Shadow/Backtest mit echten Public-Ticks; Parser-Unit-Tests mit canned JSON.

### Phase 2 — Execution (Live-fähig, Safety stubs)

1. `OkxRestClient`:
   - Headers: `OK-ACCESS-KEY`, `OK-ACCESS-SIGN`, `OK-ACCESS-TIMESTAMP`, `OK-ACCESS-PASSPHRASE`.
   - Demo: zusätzlich `x-simulated-trading: 1` auf **jeder** REST-Request.
   - Body: JSON (nicht form-urlencoded!).
   - Success: HTTP 2xx **und** top-level `"code":"0"`; per-item `sCode` bei trade endpoints.
2. `OkxFuturesOrderEncoder` + `OkxRestOrderTransport`.
3. `OkxPrivateTransport` (`IFillTransport`):
   - Connect private WS → login op → subscribe `orders` + `positions` + `account` (`instType=SWAP` / `ANY`).
   - **Kein listenKey** (Binance-Konzept existiert hier nicht).
4. `OkxUserDataParser` : `IFillParser`.
5. Live-`open()` Gate (wie Binance):
   - Clock skew via `GET /api/v5/public/time` (`ts` ms).
   - `GET /api/v5/public/instruments?instType=SWAP&instId=...` — Instrument existiert, `state=live`, lade `ctVal`, `lotSz`, `tickSz`, `minSz`.
   - `GET /api/v5/account/config` — **refuse** wenn `posMode != net_mode` (long/short braucht `posSide` auf jedem Order; Engine ist netted).
   - Optional: `GET /api/v5/account/positions` Advisories (liqPx).
6. `ExecutionBridge::deps` verdrahten analog `binance_futures_provider.h` ~394–450.

**Exit:** Demo place/cancel + fill path mit `engine_live` + Demo-Keys; Unit-Tests ohne Netz.

### Phase 3 — Safety

1. `OkxFuturesReconciler` (refuse on drift).
2. `OkxFuturesKillSwitch` (batch cancel + `close-position`).
3. `OkxFuturesDeadMansSwitch` (`cancel-all-after`).
4. `get_liveness_sources()` + `set_halt_callback` auf public/private transports.
5. `FuturesRiskCheck` caps aus Config.

**Exit:** Fake-REST-Tests für Reconciler/Kill/DMS (Pattern: `tests/providers/binance/test_binance_futures_*.cpp`); Demo-Run mit bewaffnetem DMS.

### Phase 4 — Brackets / Risk / Demo-Hardening

1. `OkxFuturesBracketAdapter` via `attachAlgoOrds` (bevorzugt atomar am Entry) oder separate algo endpoints.
2. Operator-SOP Demo → Tiny mainnet (Governance, nicht Code).
3. Optional: Freeze-Aufnahme der OKX-Safety-Dateien.

---

## 8. Concrete Parser Field Maps

### 8.1 Envelope (alle OKX REST/WS)

```json
{ "code": "0", "msg": "", "data": [ ... ] }
```

- REST: nur wenn `code == "0"` und HTTP 2xx → Erfolg.
- Trade-POST: zusätzlich `data[0].sCode == "0"`.
- WS push: oft **ohne** outer `code`; Channel-Push:

```json
{
  "arg": { "channel": "trades", "instId": "BTC-USDT-SWAP" },
  "data": [ { ... } ]
}
```

### 8.2 Trades → `provider::tick`

**Subscribe (public WS):**

```json
{
  "op": "subscribe",
  "args": [{ "channel": "trades", "instId": "BTC-USDT-SWAP" }]
}
```

**Push fields → TrueTest:**

| OKX field | Typ | → `provider::tick` |
|-----------|-----|---------------------|
| `instId` | string | `symbol` |
| `px` | string | `price` (parse double) |
| `sz` | string | `quantity` — **Achtung:** Kontrakte; für Engine ggf. `sz * ctVal` als Base-Qty (Config: `qty_unit=contracts\|base`) |
| `side` | `"buy"`/`"sell"` | `side`: buy→0 (bid aggressor?), sell→1 — **gleiche Konvention wie BinanceCombinedParser** (`data_tick_side`) |
| `ts` | string ms | `timestamp` |
| `tradeId` | string | (optional log only; tick hat kein tradeId) |
| `count` | string | ignore (aggregation count) |

**Pseudo-Mapping:**

```text
tick.timestamp = system_clock(ms(ts))
tick.symbol    = instId
tick.price     = stod(px)
tick.quantity  = scale(sz)   // Phase-1: raw contracts as int64 if small; document
tick.side      = (side=="sell") ? 1 : 0
```

### 8.3 Order books

#### `books5` (empfohlen Phase-1, kein VIP)

| Property | Value |
|----------|-------|
| Channel | `books5` |
| Push | Snapshot alle ~100ms bei Änderung |
| Levels | 5 |

**Push:**

```json
{
  "arg": { "channel": "books5", "instId": "BTC-USDT-SWAP" },
  "data": [{
    "asks": [["px","sz","deprecated","orderCount"], ...],
    "bids": [["px","sz","deprecated","orderCount"], ...],
    "ts": "...",
    "seqId": 123
  }]
}
```

→ `provider::l2_snapshot`:

| OKX | → |
|-----|---|
| `bids[i][0]` | `level.price` |
| `bids[i][1]` | `level.quantity` |
| `asks[i][0/1]` | analog |
| `ts` | `timestamp` |
| `instId` from `arg` | `symbol` |

#### `books` (400 levels, snapshot + incremental)

| `action` | Mapping |
|----------|---------|
| `"snapshot"` | `l2_snapshot` full |
| `"update"` | pro Level `l2_update` **oder** re-snapshot (einfacher, Phase-1) |

Felder: `asks`/`bids` arrays, `seqId`, `prevSeqId`, `ts`.

**Sequenz-Regel:** Bei Gap (`prevSeqId` mismatch) → REST snapshot `GET /api/v5/market/books?instId=...&sz=400` und Resync (kalt, nicht im Strategy-Hotpath).

#### `books-l2-tbt`

- Tick-by-tick 400 levels, ~10ms.
- **Requires login + VIP5** (offizielle Doku).
- Phase-1: **nicht** default; Config-Flag `depth_stream=books-l2-tbt` nur mit dokumentiertem VIP.

### 8.4 Candles → `provider::bar`

**Business WS channel:** `candle1m`, `candle5m`, `candle1H`, …  

```json
{
  "arg": { "channel": "candle1m", "instId": "BTC-USDT-SWAP" },
  "data": [[
    "ts", "o", "h", "l", "c", "vol", "volCcy", "volCcyQuote", "confirm"
  ]]
}
```

| Index/Field | → `provider::bar` |
|-------------|-------------------|
| `[0] ts` | `date` (ISO oder epoch string — match existing engine bar convention) |
| `[1] o` | `open` |
| `[2] h` | `high` |
| `[3] l` | `low` |
| `[4] c` | `close` |
| `[5] vol` | `volume` (contracts; document) |
| `[8] confirm` | nur `confirm=="1"` emittieren für closed bars (wie Binance `k.x==true`) |

### 8.5 Private: Orders channel → `parsed_exec`

**Subscribe (nach login):**

```json
{
  "op": "subscribe",
  "args": [{ "channel": "orders", "instType": "SWAP", "instId": "BTC-USDT-SWAP" }]
}
```

| OKX field | → `parsed_exec` |
|-----------|-----------------|
| `clOrdId` | `client_order_id` |
| `ordId` | `exchange_order_id` |
| `instId` | `symbol` |
| `side` (`buy`/`sell`) | `order_side` |
| `fillSz` | `last_fill_qty` |
| `fillPx` | `last_fill_price` |
| `accFillSz` | `cumulative_qty` |
| `fee` | `commission` (OKX: oft negativ für fee — **abs** oder sign-aware; match portfolio convention) |
| `feeCcy` | `commission_asset` |
| `fillTime` / `uTime` | `ts` |
| `state` | `kind` (siehe unten) |
| `sCode` / reject reason if any | `error` |

**`state` → `parsed_exec::kind`:**

| OKX `state` | `kind` |
|-------------|--------|
| `live` | `ack` (wenn fillSz==0) |
| `partially_filled` | `partial_fill` |
| `filled` | `full_fill` |
| `canceled` | `canceled` |
| (place REST `sCode != 0`) | `rejected` |

**Hinweis:** Orders-Channel hat **kein** Initial-Snapshot. Vor Live: optional `GET /api/v5/trade/orders-pending`.

### 8.6 Positions / Account → `parsed_position_snapshot`

**Positions channel** (private):

| OKX | → `position_row` |
|-----|------------------|
| `instId` | `symbol` |
| `pos` | `qty` (signed in net mode) |
| `mgnMode` | `margin_type` (`cross`→`CROSSED`, `isolated`→`ISOLATED`) |
| `posSide` | `position_side` (`net`/`long`/`short`) |

**Account channel** / balance push:

| OKX | → `balance_row` |
|-----|-----------------|
| `ccy` | `asset` |
| `eq` / `cashBal` | `wallet_balance` |
| delta if available | `balance_change` |

Funding: aus positions/account events oder REST history; optional `funding_event` wie Binance `ACCOUNT_UPDATE` path in Provider.

---

## 9. Order Encode Mapping

### 9.1 TrueTest `order_event` → OKX place body

**Endpoint:** `POST /api/v5/trade/order`  
**`encoded_order.endpoint`:** `/api/v5/trade/order`  
**`wire_payload`:** JSON string (nicht query-string!)

| TrueTest | OKX JSON field | Notes |
|----------|----------------|-------|
| symbol | `instId` | `BTC-USDT-SWAP` |
| (config) | `tdMode` | `cross` \| `isolated` (required) |
| side buy/sell | `side` | lowercase `buy`/`sell` |
| order_type market | `ordType` | `market` |
| order_type limit | `ordType` | `limit` |
| order_type stop | `ordType` | conditional: use algo endpoint **or** `ordType` + attach — Phase-2: plain market/limit first; stops via brackets |
| order_type stop_limit | — | Phase-4 |
| quantity | `sz` | contracts as string; round to `lotSz` |
| price | `px` | string; round to `tickSz`; omit for market |
| tif IOC | `ordType` | `ioc` (OKX: TIF steckt im ordType, nicht separates Feld) |
| tif FOK | `ordType` | `fok` |
| tif GTC | `ordType` | `limit` (default GTC) |
| client id | `clOrdId` | max 32 alphanumeric; **ClientOrderIdMinter kürzen/anpassen** (Binance-Prefix kann >32 werden) |
| reduce-only | `reduceOnly` | `true`/`false`; **nur net mode** |
| (net mode) | `posSide` | omit or `"net"` — **nie** long/short wenn net_mode enforced |

**Limit GTC Beispiel:**

```json
{
  "instId": "BTC-USDT-SWAP",
  "tdMode": "cross",
  "side": "buy",
  "ordType": "limit",
  "sz": "1",
  "px": "50000.1",
  "clOrdId": "tt0a1b2c3d4e5f6"
}
```

**Market:**

```json
{
  "instId": "BTC-USDT-SWAP",
  "tdMode": "cross",
  "side": "sell",
  "ordType": "market",
  "sz": "1",
  "clOrdId": "tt0a1b2c3d4e5f7"
}
```

### 9.2 Cancel

**Endpoint:** `POST /api/v5/trade/cancel-order`  
(OKX cancel ist POST, **nicht** HTTP DELETE — `IOrderTransport::cancel` mappt trotzdem auf POST.)

```json
{
  "instId": "BTC-USDT-SWAP",
  "ordId": "123456",
  "clOrdId": "tt..."
}
```

`ordId` preferred if both present (Doku).

### 9.3 Amend (optional Phase-2+)

`POST /api/v5/trade/amend-order` mit `newPx` / `newSz`.  
Engine `IExecutionAdapter::modify_order` — nur verdrahten wenn Bridge Amend unterstützt; sonst skip.

### 9.4 Batch (Kill-Switch / multi)

| Op | Endpoint | Body |
|----|----------|------|
| Place batch | `POST /api/v5/trade/batch-orders` | Array ≤20 |
| Cancel batch | `POST /api/v5/trade/cancel-batch-orders` | Array ≤20 `{instId, ordId\|clOrdId}` |

Rate (Doku): place ~60/2s per UID+instId; batch place/cancel ~300 orders/2s.

### 9.5 `OkxRestOrderTransport::map_response`

```text
if HTTP not 2xx → fail
if code != "0" → fail (msg)
if data[0].sCode != "0" → fail (sMsg)
exchange_order_id = data[0].ordId
```

---

## 10. DMS via `cancel-all-after`

### 10.1 OKX API (offiziell)

- **Endpoint:** `POST /api/v5/trade/cancel-all-after`
- **Body:**

```json
{ "timeOut": "60", "tag": "tt" }
```

| Field | Rule |
|-------|------|
| `timeOut` | String seconds: `0` = disarm, or **[10, 120]** |
| `tag` | optional, ≤16 alphanumerics; rate limit scoped UserId+tag |
| Rate | **1 request / second** (UserId+tag) |

Bei Trigger: pending order-book orders cancel; `cancelSource` kann 20 sein.

### 10.2 Mapping auf `BinanceFuturesDeadMansSwitch`-Pattern

| Binance | OKX |
|---------|-----|
| `POST /fapi/v1/countdownCancelAll` | `POST /api/v5/trade/cancel-all-after` |
| `countdownTime` ms | `timeOut` **seconds** |
| per-`symbol` | **account-wide** (optional `tag`) — **kein Symbol-Filter** |
| `countdownTime=0` disarm | `timeOut="0"` |

### 10.3 Implementierungsskizze `OkxFuturesDeadMansSwitch`

Copy structure from `binance_futures_dead_mans_switch.h`:

```text
class OkxFuturesDeadMansSwitch {
  using post_fn = function<response(endpoint, json_body)>;
  bool start();   // initial arm; fail → refuse live
  void stop();    // join heartbeat thread only
  bool disarm();  // timeOut=0
  atomic<int64_t>& liveness_ts();
  int64_t heartbeat_interval_ms() const;
};
```

**Config conversion (Provider):**

```text
// dead_man_countdown_ms from CLI (ms, same flag as Binance for operator UX)
seconds = clamp(countdown_ms / 1000, 10, 120)   // if countdown_ms > 0
// if user sets 5000ms → round up to 10s minimum (OKX floor)
// heartbeat_ms default = countdown_ms / 3, but ≥ 1000ms because rate limit 1 rps
```

**Kritisch:** Heartbeat **≤ 1 Hz** (Rate limit). Bei countdown 60s → heartbeat 20s ist safe und erfüllt 1 rps.

**Phase-3 close_fn:** wie Binance — bei 2× failed refresh: `GET positions` + `POST close-position` (optional flag `dms_attempt_position_close`).

**Arm-Reihenfolge** (Binance-Kommentar wörtlich übernehmen): DMS **last to arm, first to disarm** in `open()`/`close()`.

---

## 11. Kill-Switch Recipe

### 11.1 Interface

`IKillSwitch::cancel_all_and_flatten(deadline)` aus `live_safety.h` — return `false` bei Timeout/Fehler (loud).

### 11.2 OKX Sequenz (fail-closed)

```text
1. Bound rest client per-call timeout = min(1500ms, deadline/3)  // wie Binance

2. Cancel open orders for instId:
   a) GET /api/v5/trade/orders-pending?instType=SWAP&instId=BTC-USDT-SWAP
   b) POST /api/v5/trade/cancel-batch-orders  with chunks of ≤20
      [{ "instId":"...", "ordId":"..." }, ...]
   // Empty pending list = success (no-op)
   // Per-item sCode already-canceled → treat as success

3. If now >= deadline → return false

4. Flatten position:
   POST /api/v5/trade/close-position
   {
     "instId": "BTC-USDT-SWAP",
     "mgnMode": "cross",          // must match position; from last known or GET positions
     "autoCxl": true,             // cancel remaining close-related orders
     "clOrdId": "<minter next>"   // ≤32 chars
   }
   // net mode: omit posSide or "net"
   // long_short_mode: would need posSide long/short — but we refuse that mode at open()

5. Optional verify: GET /api/v5/account/positions?instId=...  expect pos≈0
   If still non-zero and time left → retry close-position once (optional; default: fail loud)

6. return true only if cancel ok AND (pos flat OR close accepted with code=="0")
```

**Unterschied zu Binance:** Binance nutzt `DELETE allOpenOrders` + `reduceOnly MARKET`. OKX hat natives **`close-position`** (Market close entire position) — bevorzugen.

**Kein** stilles Retry-Backoff-Loop über den Deadline hinaus (S4).

### 11.3 Reconciler (Startup)

| Check | OKX REST | Local |
|-------|----------|-------|
| Cash | `GET /api/v5/account/balance` → USDT `details[].availBal` or `eq` | `portfolio.get_cash()` |
| Position | `GET /api/v5/account/positions?instId=...` → `pos` | `positions[instId].qty` |

Empty error string = pass; non-empty = refuse start (`IReconciler` contract).

Tolerance: gleiche `within_tolerance` bps-Logik wie `BinanceFuturesReconciler`.

**Kein** testnet-reset-heuristic (Binance-Spot-Hack); Demo-Drift = Operator muss local checkpoint clearen.

---

## 12. Config / CLI / Env

### 12.1 Neue Env Vars

| Env | Zweck |
|-----|-------|
| `TRUETEST_OKX_API_KEY` | API key |
| `TRUETEST_OKX_API_SECRET` | Secret |
| `TRUETEST_OKX_API_PASSPHRASE` | Passphrase (bei Key-Erstellung gesetzt) |

CLI-Fallback: `--api-key`, `--api-secret`, **`--api-passphrase`** (neu in `main.inc`).

Credential-Resolver analog `resolve_exchange_credentials` in `src/bin/main.inc` — **provider-aware** oder separate `resolve_okx_credentials()`; Env gewinnt über argv; argv-Warnung beibehalten.

### 12.2 CLI Flags (reuse wo möglich)

| Flag | Provider key | Notes |
|------|--------------|-------|
| `--provider okx-futures` | | |
| `--symbol BTC-USDT-SWAP` | `symbol` | |
| `--stream trades` | `stream` | |
| `--depth-stream books5` | `depth_stream` | |
| `--testnet` / `--demo` | `demo=1` | OKX Demo, nicht Binance testnet hosts |
| `--td-mode cross` | `td_mode` | |
| `--dead-man-countdown-ms` | | convert → timeOut s |
| `--dead-man-heartbeat-ms` | | ≥1000 |
| `--disarm-deadman` | countdown 0 | |
| `--dms-attempt-position-close` | | |
| `--max-notional-usdt` etc. | | FuturesRiskCheck |
| `--margin-type cross` | advisory | map to mgnMode |

### 12.3 Beispiel-Kommandos

```bash
# Phase-1 market data (shadow)
./build/engine_shadow \
  --provider okx-futures \
  --symbol BTC-USDT-SWAP \
  --stream trades \
  --depth-stream books5 \
  --strategy sma \
  --no-tui --status-format off

# Phase-2+ demo live (attended)
export TRUETEST_OKX_API_KEY=...
export TRUETEST_OKX_API_SECRET=...
export TRUETEST_OKX_API_PASSPHRASE=...
./build/engine_live \
  --provider okx-futures \
  --symbol BTC-USDT-SWAP \
  --stream trades \
  --demo \
  --dead-man-countdown-ms 60000 \
  --max-notional-usdt 50 \
  --strategy sma \
  # ... operator ritual / size caps per governance
```

### 12.4 `provider_config` in `main.inc`

Erweitere `run_provider_mode` pcfg:

```cpp
if (!o.api_passphrase.empty()) pcfg["api_passphrase"] = resolved_passphrase;
if (o.demo || o.testnet) pcfg["demo"] = "1";
if (!o.td_mode.empty()) pcfg["td_mode"] = o.td_mode;
```

`HAS_OKX`-Guards nur in Provider-Dateien, nicht in Engine.

---

## 13. Test Plan

### 13.1 Unit (kein Netzwerk) — mandatory

| Test file | Asserts |
|-----------|---------|
| `test_okx_auth.cpp` | Sign(prehash) Base64 matches fixture; ISO timestamp format |
| `test_okx_endpoints.cpp` | mainnet vs demo hosts; `x-simulated-trading` flag plumbing |
| `test_okx_parser.cpp` | trades canned → tick fields |
| `test_okx_books_parser.cpp` | books5 snapshot; books update |
| `test_okx_combined_parser.cpp` | multi-channel envelope |
| `test_okx_futures_order_encoder.cpp` | market/limit/ioc/fok JSON; cancel body; clOrdId length ≤32 |
| `test_okx_user_data_parser.cpp` | orders states → kind; positions snapshot |
| `test_okx_futures_register.cpp` | registry names; missing symbol throws |
| `test_okx_futures_reconciler.cpp` | cash/pos match & drift refuse (fake rest) |
| `test_okx_futures_kill_switch.cpp` | cancel-batch + close-position call order; deadline |
| `test_okx_futures_dead_mans_switch.cpp` | arm timeOut, disarm 0, heartbeat, start fail |
| `test_okx_futures_bracket_adapter.cpp` | attachAlgoOrds shape; cancel legs |

**Pattern:** Inject `post_fn` / fake REST wie `tests/providers/binance/test_binance_futures_dead_mans_switch.cpp`.

### 13.2 Integration (gated, `HAS_OKX` + env)

| Test | Gate |
|------|------|
| `test_okx_demo_live.cpp` | `TRUETEST_OKX_*` + opt-in env `TRUETEST_OKX_DEMO_LIVE=1` |
| Public WS smoke | network |

### 13.3 Nach jedem Edit unter `src/`

```bash
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

### 13.4 ASAN

Bei Transport/DMS-Threads: `cmake --preset linux-asan` + DMS/Kill-Tests.

---

## 14. Acceptance Criteria

### Phase 0

- [ ] `ENABLE_OKX=ON` baut `engine_*` ohne Link-Fehler
- [ ] `ProviderRegistry` listet `okx` und `okx-futures`
- [ ] Layer-deps clean; keine `HAS_OKX` in `engine/`/`core/`

### Phase 1

- [ ] Public trades → engine ticks; books5 → L2 path wenn `supports_event_stream()`
- [ ] Parser unit tests ≥ canned fixtures aus echten Demo-Captures
- [ ] Shadow-run mit SMA startet und verarbeitet Events

### Phase 2

- [ ] Demo: place limit + cancel via ExecutionBridge
- [ ] Orders WS → `parsed_exec` → portfolio fill
- [ ] `open()` refuses `long_short_mode`
- [ ] `open()` refuses unknown `instId`
- [ ] Clock skew gate

### Phase 3

- [ ] Reconciler refuses cash/pos drift
- [ ] Kill-switch cancels + close-position within deadline (fake + demo)
- [ ] DMS: initial arm fail → live refuse; disarm on close
- [ ] Fatal private WS → halt callback fired (unit or integration)
- [ ] `FuturesRiskCheck` blocks over-notional

### Phase 4

- [ ] Bracket place SL+TP (attachAlgoOrds or dual algo); cancel on teardown
- [ ] Operator checklist Demo documented
- [ ] No invented API fields; all endpoints match OKX v5 docs

### Global

- [ ] Hot path: no nlohmann in okx parsers on event loop
- [ ] Live orders impossible in `engine_backtest` / `engine_shadow` (TT_TARGET unchanged)
- [ ] German comments only where non-obvious invariants; identifiers English

---

## 15. Quirks & Pitfalls

### 15.1 Passphrase

- Dritter Secret neben Key/Secret.
- Im REST-Header `OK-ACCESS-PASSPHRASE` **plaintext** (nicht gehasht) — so Doku.
- Bei manchen älteren Keys: Passphrase wurde serverseitig mit Secret gehasht — **neue Keys** nutzen den Passphrase wie eingegeben; bei 401 sign/passphrase systematisch testen.
- Nie loggen; redact in error paths (wie `binance::redact_for_log`).

### 15.2 `instId` Format

| Venue | Symbol |
|-------|--------|
| Binance USDT-M | `BTCUSDT` |
| OKX SWAP linear | `BTC-USDT-SWAP` |

- Kein stilles Uppercase-Strip der Bindestriche.
- CLI-User, die `btcusdt` eingeben → fail loud mit Hint `BTC-USDT-SWAP`.

### 15.3 Position Modes

| `posMode` | Order `posSide` | TrueTest |
|-----------|-----------------|----------|
| `net_mode` | omit / `net` | **only supported** |
| `long_short_mode` | required `long`/`short` | **refuse at open()** |

Analog Binance hedge-mode gate (`/fapi/v1/positionSide/dual`).

### 15.4 `tdMode` vs Margin

- SWAP: `cross` oder `isolated` (nicht `cash`).
- Kill-switch `close-position` braucht matching `mgnMode`.
- Advisory: expected margin type from CLI.

### 15.5 Quantity = Contracts

- `sz` ist **Kontraktanzahl**, nicht BTC.
- `ctVal` aus instruments (z.B. BTC-USDT-SWAP oft `0.01` BTC/contract).
- Notional ≈ `sz * ctVal * markPx` (linear).
- `FuturesRiskCheck` notional cap muss dieselbe Einheit nutzen — Provider sollte mark/notional in USDT rechnen.

### 15.6 VIP Book Channels

| Channel | VIP / Login |
|---------|-------------|
| `books5`, `books` | public, no VIP |
| `books50-l2-tbt` | VIP4 + login |
| `books-l2-tbt` | VIP5 + login |
| `fills` private | VIP4+; sonst Orders-Channel nutzen |

Default depth: **`books5`**.

### 15.7 Auth Differences vs Binance

| | Binance | OKX |
|--|---------|-----|
| Sign encoding | Hex lowercase | **Base64** |
| Timestamp REST | ms epoch in query | **ISO8601** `2020-12-08T09:08:57.715Z` header |
| Prehash | query string | `timestamp + METHOD + path + body` |
| Passphrase | no | **yes** |
| Body | form query | **JSON** |
| WS auth | listenKey URL | **login message** after connect |
| WS login ts | — | Unix **seconds** string; sign `ts + 'GET' + '/users/self/verify'` |

### 15.8 DMS Scope

- OKX `cancel-all-after` ist **nicht symbol-scoped** (anders als Binance `countdownCancelAll?symbol=`).
- Multi-symbol Provider später: ein DMS schützt **alle** open orders des Accounts/tags.
- `tag` nutzen, um TrueTest-Heartbeats von anderen Bots zu trennen (Rate limit pro tag).

### 15.9 `clOrdId` Length

- OKX: ≤32 alphanumeric.
- `ClientOrderIdMinter` (Binance ~30 chars design) — Prefix `tt` + kürzeres encoding; Unit-test max length.

### 15.10 Response Success

Niemals nur HTTP 200 prüfen — immer `code=="0"` und trade `sCode`.

### 15.11 Demo

| | Mainnet | Demo |
|--|---------|------|
| REST host | `https://www.okx.com` or `https://openapi.okx.com` | **same** + header `x-simulated-trading: 1` |
| Public WS | `wss://ws.okx.com:8443/ws/v5/public` | `wss://wspap.okx.com:8443/ws/v5/public` |
| Private WS | `wss://ws.okx.com:8443/ws/v5/private` | `wss://wspap.okx.com:8443/ws/v5/private` |
| Business WS | `wss://ws.okx.com:8443/ws/v5/business` | `wss://wspap.okx.com:8443/ws/v5/business` |
| API keys | Production keys | **Separate** Demo Trading API keys (UI: Demo Trading → API) |

### 15.12 Candles sitzen auf Business-WS

Nicht public-WS — sonst silent subscribe error.

### 15.13 Rate Limits (Auswahl, offiziell)

| Resource | Limit (typisch) |
|----------|-----------------|
| WS connect | 3/s per IP |
| WS sub/login | 480/hour per connection |
| Place order | 60 / 2s (UID+instId) |
| Cancel-all-after | 1 / s (UID+tag) |
| Close-position | 20 / 2s (UID+instId) |
| Account balance/positions | 10 / 2s |
| Error 50011 | rate limit |

`TokenBucketRateLimiter` am Bridge: konservativ z.B. capacity 30, refill 15/s (unter Place-Limit).

---

## 16. Official References

Primär: **[OKX API v5 Docs](https://www.okx.com/docs-v5/en/)**

| Topic | Section (docs-v5) |
|-------|-------------------|
| Overview / REST base / Demo header | Getting Started |
| Auth headers + sign | Overview → REST Authentication |
| WS public/private/business URLs | Overview → WebSocket |
| WS login | Account / Trade WS login |
| Trades channel | Market Data → WS → Trades |
| Books / books5 / books-l2-tbt | Market Data → WS → Order book |
| Candles | Market Data → WS → Candlesticks (business) |
| Place/amend/cancel/batch | Trade → REST |
| Cancel-all-after | Trade → Cancel All After |
| Close-position | Trade → Close Position |
| Orders / fills / positions / account channels | Trade & Account WS |
| Instruments `ctVal`/`lotSz`/`tickSz` | Public Data → Get instruments |
| Account config `posMode` | Account → Get account configuration |
| Set position mode | Account → Set position mode |
| Rate limits | Per-endpoint + overview |
| Error codes | Error Code appendix |
| Best practices | “How to place orders / reconcile” trick guide |

### TrueTest Code References (absolute)

| Path | Why |
|------|-----|
| `/home/leonard/work/projects/truetest/core/src/providers/provider.h` | `IProvider` + safety hooks |
| `/home/leonard/work/projects/truetest/core/src/providers/transport.h` | `IDataTransport` |
| `/home/leonard/work/projects/truetest/core/src/providers/parser.h` | `IDataParser` |
| `/home/leonard/work/projects/truetest/core/src/providers/provider_event.h` | event variant |
| `/home/leonard/work/projects/truetest/core/src/providers/provider_registry.h` | `REGISTER_PROVIDER` |
| `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_provider.h` | **Golden composition root** |
| `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_dead_mans_switch.h` | DMS pattern |
| `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_kill_switch.h` | Kill pattern |
| `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_reconciler.h` | Reconcile pattern |
| `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_order_encoder.h` | Encoder pattern |
| `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_user_data_parser.h` | Fill parser pattern |
| `/home/leonard/work/projects/truetest/core/src/providers/binance/binance_futures_register.cpp` | Registry factory |
| `/home/leonard/work/projects/truetest/core/src/execution/live_safety.h` | `IReconciler` / `IKillSwitch` |
| `/home/leonard/work/projects/truetest/core/src/execution/execution_bridge.h` | Live bridge deps |
| `/home/leonard/work/projects/truetest/core/src/risk/futures_risk_check.h` | Pre-trade caps |
| `/home/leonard/work/projects/truetest/core/cmake/Dependencies.cmake` | `ENABLE_*` wiring |
| `/home/leonard/work/projects/truetest/core/cmake/Sources.cmake` | Test registration |
| `/home/leonard/work/projects/truetest/core/AGENTS.md` | Hot path + safety red lines |
| `/home/leonard/work/projects/truetest/core/docs/architecture/01-target-architecture.md` | Layer model |

---

## Appendix A — `okx_endpoints.h` Skeleton (concrete constants)

```cpp
namespace okx {
struct endpoints {
    std::string rest_host;   // "www.okx.com" or "openapi.okx.com"
    std::string rest_port;   // "443"
    std::string ws_public;   // host only: "ws.okx.com"
    std::string ws_private;
    std::string ws_business;
    std::string ws_port;     // "8443"
    bool is_demo = false;
};

inline endpoints mainnet() {
    return {
        "www.okx.com", "443",
        "ws.okx.com", "ws.okx.com", "ws.okx.com", "8443",
        false
    };
}

inline endpoints demo() {
    return {
        "www.okx.com", "443",          // same REST + x-simulated-trading
        "wspap.okx.com", "wspap.okx.com", "wspap.okx.com", "8443",
        true
    };
}

// Paths
inline constexpr const char* k_ws_public_path   = "/ws/v5/public";
inline constexpr const char* k_ws_private_path  = "/ws/v5/private";
inline constexpr const char* k_ws_business_path = "/ws/v5/business";
inline constexpr const char* k_time             = "/api/v5/public/time";
inline constexpr const char* k_instruments      = "/api/v5/public/instruments";
inline constexpr const char* k_place_order      = "/api/v5/trade/order";
inline constexpr const char* k_cancel_order     = "/api/v5/trade/cancel-order";
inline constexpr const char* k_batch_orders     = "/api/v5/trade/batch-orders";
inline constexpr const char* k_cancel_batch     = "/api/v5/trade/cancel-batch-orders";
inline constexpr const char* k_cancel_all_after = "/api/v5/trade/cancel-all-after";
inline constexpr const char* k_close_position   = "/api/v5/trade/close-position";
inline constexpr const char* k_orders_pending   = "/api/v5/trade/orders-pending";
inline constexpr const char* k_balance          = "/api/v5/account/balance";
inline constexpr const char* k_positions        = "/api/v5/account/positions";
inline constexpr const char* k_account_config   = "/api/v5/account/config";
}
```

---

## Appendix B — Auth Prehash (copy into tests)

**REST:**

```text
prehash = ISO8601_timestamp + "POST" + "/api/v5/trade/order" + json_body
sign    = Base64( HMAC_SHA256(secret, prehash) )
```

**WS login:**

```text
prehash = unix_seconds + "GET" + "/users/self/verify"
sign    = Base64( HMAC_SHA256(secret, prehash) )
payload = { "op":"login", "args":[{ "apiKey", "passphrase", "timestamp", "sign" }] }
```

Reuse OpenSSL `EVP_MAC` / `HMAC` patterns from `binance_auth.h` (`HmacSha256Signer`), aber Output **Base64** statt Hex.

---

## Appendix C — Implementation Order Checklist (Agent)

1. CMake `ENABLE_OKX` + empty provider + register  
2. `okx_auth` + tests  
3. `okx_endpoints` + tests  
4. Public transport + trades parser + combined books5  
5. Shadow smoke  
6. REST client + time sync + instruments probe  
7. Order encoder + order transport + private WS login + user data parser  
8. Wire `ExecutionBridge` in live `open()`  
9. Reconciler + kill switch + DMS  
10. Brackets + demo SOP  
11. Scripts: hotpath-json / layer-deps / freeze  
12. Thematic commit(s) — kein Freeze-Touch ohne Token  

---

*Guide grounded in TrueTest `core/` sources (2026-07) and OKX API v5 public documentation. Do not invent endpoints, fields, or VIP bypasses.*
