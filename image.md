# TrueTest Core — Architecture Diagram

**Status**: Read-only survey of the tree as of 2026-08-10.  
**Scope**: C++ engine package `core/` only (not monorepo `backend/` / `UI/`).  
**Sources**: `src/**`, `scripts/check-layer-deps.sh`, `docs/architecture/*`, `docs/reference/02-user-manual.md`, `AGENTS.md`.

---

## 1. One-line model

One C++23 codebase → three compile-time binaries (`TT_TARGET`) that share the **same hot path** (strategy → risk → execution → portfolio → rings → workers), with live-order code **physically eliminated** outside `engine_live`.

```mermaid
flowchart LR
  subgraph tree["Single source tree"]
    SRC["src/**"]
  end

  subgraph bins["Compile-time targets"]
    BT["engine_backtest<br/>TT_TARGET=BACKTEST"]
    SH["engine_shadow<br/>TT_TARGET=SHADOW"]
    LV["engine_live<br/>TT_TARGET=LIVE"]
  end

  SRC --> BT
  SRC --> SH
  SRC --> LV

  BT -.->|live orders| X1["DCE / impossible"]
  SH -.->|live orders| X2["DCE / impossible"]
  LV -->|live orders| OK["allowed + gated"]
```

| Binary | `TT_TARGET` | Live orders | Primary use |
|--------|-------------|-------------|-------------|
| `engine_backtest` | `BACKTEST` | Impossible (DCE) | Historical replay, Monte Carlo |
| `engine_shadow` | `SHADOW` | Impossible | Real-time paper vs exchange tape |
| `engine_live` | `LIVE` | Allowed (gated) | Real money — experimental, attended |

Gate: `src/core/tt_target.h` → `constexpr target_allows_live_orders()`.

---

## 2. Layer graph (enforced)

Dependency direction is enforced by `scripts/check-layer-deps.sh`. Edges may only point to allowed lower/peer modules — never upward into composition roots from leaves.

```mermaid
flowchart TB
  subgraph leaves["Leaf / foundation"]
    CORE["core<br/>events, TT_TARGET, event_log"]
    TYPES["types<br/>ObjectPool, price, symbol_table"]
    INDICATOR["indicator<br/>SMA/EMA/RSI/..."]
    UTILS["utils"]
  end

  subgraph mid["Domain mid-layers"]
    OB["orderbook"]
    TH["threading<br/>SPSC RingBuffer, Worker, Watchdog"]
    EXEC["execution<br/>IExecutionAdapter, Portfolio, realism"]
    EXITS["exits<br/>ExitManager, DefaultExitPolicy, IBracketAdapter"]
    RISK["risk<br/>RiskManager, FuturesRiskCheck"]
    STRAT["strategy<br/>IStrategy + registry"]
    DATA["data<br/>MarketSeries, DataWrapper, QuestDB"]
    ANALYTICS["analytics<br/>metrics, footprint, reports"]
    MM["market_maker"]
  end

  subgraph edge["Composition / edge"]
    PROV["providers<br/>IProvider sole venue extension"]
    ENG["engine<br/>composition root"]
    API["api<br/>C embed"]
    WEB["web<br/>read-only serializers + SPA"]
    UI["ui<br/>TUI / ImGui desk / snapshots"]
    SIM["simulation<br/>Monte Carlo"]
  end

  CORE --> OB
  TYPES --> OB
  CORE --> TH
  CORE --> EXEC
  TYPES --> EXEC
  OB --> EXEC

  CORE --> RISK
  EXEC --> RISK
  ANALYTICS --> RISK

  CORE --> STRAT
  TYPES --> STRAT
  INDICATOR --> STRAT
  EXEC --> STRAT
  EXITS --> STRAT

  CORE --> DATA
  TYPES --> DATA
  EXEC --> DATA

  CORE --> ANALYTICS
  TH --> ANALYTICS
  RISK --> ANALYTICS
  TYPES --> ANALYTICS

  CORE --> MM
  OB --> MM
  TH --> MM

  CORE --> PROV
  DATA --> PROV
  EXEC --> PROV
  RISK --> PROV
  EXITS --> PROV
  SIM --> PROV

  ENG --> CORE
  ENG --> TH
  ENG --> EXEC
  ENG --> RISK
  ENG --> STRAT
  ENG --> DATA
  ENG --> PROV
  ENG --> EXITS
  ENG --> UI
  ENG --> ANALYTICS
  ENG --> MM

  API --> ENG
  WEB --> UI
  WEB --> ANALYTICS
```

**Invariant**: Provider is the **sole venue extension point**. No `HAS_BINANCE` / venue leakage into `core`, `engine` generics, `threading`, or `risk` layers beyond the abstract `IRiskCheck` port.

---

## 3. Runtime system context

```mermaid
flowchart TB
  subgraph inputs["Ingress"]
    CSV["CSV bars / ticks"]
    SYN["Synthetic / GBM"]
    WS["Venue WS MD<br/>trade · kline · depth"]
    REST["Venue REST<br/>orders · positionRisk · listenKey"]
    REPLAY["zstd event log replay"]
  end

  subgraph bin["Binary entry"]
    MAIN["bin/main.inc<br/>CLI · config · factories"]
  end

  subgraph core_loop["Engine core"]
    ENG["engine<br/>event loop · pools · halt"]
    STRAT["IStrategy(+s)"]
    RISK["RiskManager + IRiskCheck"]
    ROUTER["ExecutionRouter"]
    ADAPT["IExecutionAdapter"]
    PORT["Portfolio + OrderTracker"]
    EXIT["ExitManager + DefaultExitPolicy"]
    AUDIT["IOrderAuditSink"]
  end

  subgraph async["Async workers via SPSC rings"]
    LOGW["LoggingWorker"]
    RISKW["RiskWorker / RiskStatsWorker"]
    STATW["StatsWorker"]
    OBSW["ObserverWorker"]
    MMW["MarketMakerWorker"]
  end

  subgraph egress["Egress / observability"]
    TUI["ncurses TUI / ANSI dash"]
    DESK["ImGui desk optional"]
    WEB["Embedded web UI optional"]
    QDB["QuestDB ILP optional"]
    ELOG["Binary event log"]
    RPT["JSON report"]
  end

  CSV --> MAIN
  SYN --> MAIN
  WS --> MAIN
  REST --> MAIN
  REPLAY --> MAIN

  MAIN --> ENG
  ENG --> STRAT
  STRAT --> RISK
  RISK --> ROUTER
  ROUTER --> ADAPT
  ADAPT --> PORT
  PORT --> EXIT
  ENG --> AUDIT

  ENG --> LOGW
  ENG --> RISKW
  ENG --> STATW
  ENG --> OBSW
  ENG --> MMW

  LOGW --> ELOG
  LOGW --> QDB
  AUDIT --> QDB
  OBSW --> TUI
  OBSW --> DESK
  ENG --> WEB
  ENG --> RPT
```

---

## 4. Hot-path data flow (event loop)

Zero-alloc discipline: acquire from pre-warmed `ObjectPool`s → strategy/risk/execution → `publish_event` fan-out to SPSC rings. No heap grow, no JSON, no multi-producer rings.

```mermaid
sequenceDiagram
  participant SRC as IProvider / MarketSeries
  participant ENG as engine loop
  participant ST as IStrategy
  participant RM as RiskManager + IRiskCheck
  participant XR as ExecutionRouter
  participant XA as IExecutionAdapter
  participant PF as Portfolio
  participant XM as ExitManager
  participant AN as Analytics
  participant RB as SPSC Rings → Workers

  SRC->>ENG: bar / tick / l2 / provider::event
  ENG->>ENG: acquire_pooled(event)
  ENG->>ST: on_market / on_tick / on_l2
  ST-->>ENG: optional order_event + exit_intent(s)
  ENG->>RM: pre-trade checks
  alt rejected
    ENG->>ENG: rejection_event + audit
  else accepted
    ENG->>XR: submit / route
    XR->>XA: submit_order
    XA-->>ENG: poll_fills → fill_event
    ENG->>PF: apply fill
    ENG->>XM: brackets / protective SL·TP
    ENG->>AN: on_event
    ENG->>ST: on_fill
  end
  ENG->>RB: publish_event (logging, risk, stats, observer, mm)
```

### Event types (`src/core/event.h`)

| Type | Role |
|------|------|
| `market` | OHLCV bar |
| `tick` | Trade / tick print |
| `l2_snapshot` / `l2_update` | Depth book |
| `order` | Strategy/platform intent |
| `fill` | Simulated or venue fill |
| `cancel` / `amend` / `rejection` | Order lifecycle |
| `funding` | Futures funding cash delta |
| `signal` | Reserved / legacy |

---

## 5. Mode × adapter matrix

```mermaid
flowchart LR
  subgraph backtest["BACKTEST"]
    B1["LocalBookAdapter"]
    B2["QueueAwareBookAdapter"]
    B3["HybridPaperAdapter<br/>limits→queue · mkt/stop→local"]
    B4["Orderbook + FillModel<br/>+ latency/impact/fee"]
  end

  subgraph shadow["SHADOW"]
    S1["TradeTapeShadowAdapter"]
    S2["Hybrid / paper executors"]
    S3["shadow_tracker divergence"]
    S4["Real MD · simulated fills"]
  end

  subgraph live["LIVE only"]
    L1["ExecutionBridge"]
    L2["HybridExecutor / venue executor"]
    L3["REST order transport"]
    L4["User-data WS = truth"]
  end
```

| Concern | Backtest | Shadow | Live |
|---------|----------|--------|------|
| Market data | CSV / synthetic / replay | Live venue WS | Live venue WS |
| Fills | Simulated (book + models) | Simulated vs real tape | Venue fills |
| Realism models | Active (if flagged) | Active (if flagged) | **Bypassed** |
| Live orders | Impossible | Impossible | Gated + safety stack |
| Protective exits | `ExitManager` engine-side | Engine-side | Engine-side + optional `IBracketAdapter` on venue |

Realism knobs (backtest/shadow only): latency, impact, walked-book, queue position, probabilistic fills, fees — see `docs/architecture/03-realism.md`.

---

## 6. Provider architecture (venue boundary)

`IProvider` is the **only** place venue knowledge may live. Engine consumes abstract ports.

```mermaid
flowchart TB
  REG["ProviderRegistry<br/>REGISTER_PROVIDER"]

  subgraph ports["Ports returned by IProvider"]
    TR["IDataTransport"]
    PR["IDataParser&lt;T&gt;"]
    XA["IExecutionAdapter"]
    RC["IReconciler"]
    KS["IKillSwitch"]
    RISK["IRiskCheck"]
    BR["IBracketAdapter"]
    SPEC["instrument_spec"]
    LIVE["liveness_source(s)"]
  end

  REG --> LOCAL["local<br/>CSV / file"]
  REG --> SYN["synthetic / montecarlo"]
  REG --> BIN["binance · binance-futures"]
  REG --> BG["bitget · bitget-futures"]
  REG --> BU["bitunix · bitunix-futures"]

  LOCAL --> ports
  SYN --> ports
  BIN --> ports
  BG --> ports
  BU --> ports

  ports --> ENG["engine wires adapters,<br/>safety, watchdog, halt cb"]
```

### Provider stack pattern

```
IProvider
  ├─ IDataTransport  (file / WS / combined / replay / synthetic)
  ├─ IDataParser<T>  (bar_record | tick_record | provider::event | venue payloads)
  ├─ DataBridge<T>   (batch load_into + run_streaming)
  └─ IExecutionAdapter (+ safety hooks)
```

### Futures safety stack (venue-owned, engine-wired)

```mermaid
flowchart LR
  START["engine start"] --> REC["IReconciler<br/>default-refuse if mismatch"]
  REC --> RUN["event loop"]
  RUN --> DMS["Dead-man's switch<br/>heartbeat → Watchdog"]
  RUN --> KS["IKillSwitch<br/>cancel + flatten"]
  RUN --> VR["IRiskCheck pre-trade<br/>notional · leverage · liq distance"]
  RUN --> UD["User-data stream<br/>source of truth"]
  DMS -->|silent| HALT["halt_flag_ terminal"]
  KS -->|deadline miss| HALT
  UD -->|fatal disconnect| HALT
```

**Halt is write-once terminal** — process restart only; no auto-clear, no retry-with-backoff on safety paths.

---

## 7. Engine internals (composition root)

```mermaid
flowchart TB
  subgraph engine_mod["src/engine/"]
    E["engine.{h,cpp}<br/>loop · pools · halt · run*"]
    CFG["engine_config"]
    XR["ExecutionRouter<br/>submit / poll / L2 / advance"]
    AUD["IOrderAuditSink<br/>Noop | QuestDB"]
    SNAP["DashboardSnapshotBuilder"]
    ISC["InstrumentSpecCache"]
    CKP["checkpoint"]
    W_LOG["logging_worker"]
    W_RISK["risk_worker · risk_stats_worker"]
    W_STAT["stats_worker"]
    W_OBS["observer_worker"]
    W_MM["market_maker_worker"]
  end

  E --> CFG
  E --> XR
  E --> AUD
  E --> SNAP
  E --> ISC
  E --> CKP
  E --> W_LOG
  E --> W_RISK
  E --> W_STAT
  E --> W_OBS
  E --> W_MM
```

### Run paths

| Method | When |
|--------|------|
| `run()` | Batch bars from `MarketSeries` |
| `run_tick_data()` | Batch ticks |
| `run_streaming(DataBridge<…>)` | Live/shadow stream (bar, tick, or unified `provider::event`) |
| `run_replay(path)` | Deterministic zstd event-log replay |

### Decomposition seams (in progress)

Cold collaborators already extracted: `ExecutionRouter`, `IOrderAuditSink`, `DashboardSnapshotBuilder`, `InstrumentSpecCache`, `OrderAuditSink`. Further waves: `docs/internal/engine-decomposition.md`.

---

## 8. Threading model

```mermaid
flowchart LR
  subgraph hot["Hot path — sole producer"]
    EL["Event loop thread<br/>pin Core 0 typically"]
  end

  EL -->|SPSC 64k| R1["logging_ring"]
  EL -->|SPSC 64k| R2["risk_ring"]
  EL -->|SPSC 64k| R3["stats_ring"]
  EL -->|SPSC 64k| R4["observer_ring"]
  EL -->|SPSC 64k| R5["risk_stats_ring"]
  EL -->|SPSC 64k| R6["mm_ring"]

  R1 --> LW["LoggingWorker"]
  R2 --> RW["RiskWorker"]
  R3 --> SW["StatsWorker"]
  R4 --> OW["ObserverWorker → TUI"]
  R5 --> RSW["RiskStatsWorker"]
  R6 --> MMW["MarketMakerWorker"]

  WD["WorkerWatchdog"] -.->|3× heartbeat miss| H["trigger_halt"]
```

### Thread presets (`--thread-preset`)

| Preset | Typical cores | Workers |
|--------|---------------|---------|
| `inline` | 1–2 | None (everything on event loop) — required for `--mc-parallel` |
| `light` | 3 | Combined observer |
| `standard` | 4–5 | Logging + RiskStats |
| `full` | 6–7 | Logging + Risk + Stats |
| `extended` | 8+ | + MarketMakerWorker |

**Rules**: SPSC only; **exactly one producer** per ring; safety rings use `halt_on_drop` in shadow/live; spin policy `adaptive|spin|yield`.

### Memory discipline

```
prewarm ObjectPools + ControlBlockPool
  → forbid_runtime_grow = true
  → acquire_pooled on hot path
  → DeferredReturnQueue drain on engine thread
  → pool_exhausted → trigger_halt (fail closed)
```

---

## 9. Strategy & exits

```mermaid
flowchart LR
  REG["StrategyRegistry<br/>REGISTER_STRATEGY"] --> ST["IStrategy"]
  ST -->|on_market/tick/l2| ORD["order_event?"]
  ST -->|take_pending_exit_intents| EI["exit_intent[]"]
  ST -->|on_fill| LOT["lot tracking"]

  ORD --> ENG["engine"]
  EI --> XM["ExitManager"]
  DEF["DefaultExitPolicy<br/>--exit-policy / --sl / --tp"] --> XM
  XM --> BR["IBracketAdapter<br/>live venue resting brackets"]
  XM --> PLAT["engine-side protective exits<br/>backtest/shadow always"]
```

**Built-in strategies** (self-registering):  
`sma` · `ma-crossover` · `mean-reversion` · `breakout` · `coiled-spring` · `larry_connor` · `hedge-demo` · `adaptive-hybrid` · `structure-continuation`

**Indicators** (`src/indicator/`): SMA, EMA, RSI, Stochastic, Bollinger, ATR, swing detector, rolling extremes, EMA regime.

Platform protective SL/TP is **default** — strategies need not implement stops; `exit_intent`s refine.

---

## 10. Data pipeline

```mermaid
flowchart TB
  subgraph cold["Cold path — load / decode"]
    DW["DataWrapper<br/>from_path / from_paths / from_uri / from_source"]
    SRC["IMarketSource / IDataSource"]
    BR["DataBridge&lt;T&gt;"]
  end

  subgraph store["Format-agnostic store"]
    MS["MarketSeries<br/>SoA bars · AoS ticks<br/>typedef data_handler"]
  end

  subgraph stream["Streaming"]
    TR["IDataTransport"]
    PA["IDataParser"]
    SN["IMarketSink / sink_fn"]
  end

  DW --> SRC
  SRC --> MS
  BR --> MS
  TR --> PA --> SN --> MS
  MS --> ENG["engine batch iteration"]
  BR --> ENG2["engine run_streaming"]
```

QuestDB (`src/data/questdb/`) is **audit egress** (ILP), not market-data ingress.

---

## 11. Monte Carlo research path

```mermaid
flowchart LR
  CLI["--monte-carlo --mc-trials N<br/>--provider synthetic"] --> MCC["MonteCarloController"]
  MCC --> GEN["IMonteCarloGenerator<br/>e.g. GBM"]
  MCC --> ENG["engine per trial<br/>deterministic seed = base + trial"]
  ENG --> ST["strategy.reset(seed)"]
  ENG --> RE["realism + ExitManager reuse"]
  MCC --> REP["MonteCarloReporter<br/>trials[] + aggregates"]
```

- Optional `--mc-reuse-objects` (reset instead of reconstruct).
- Experimental `--mc-parallel` only with `--thread-preset inline`.
- Stylized L2; research tool — does not relax live Phase 0/1 gates.

---

## 12. Observability surfaces

```mermaid
flowchart TB
  ENG["engine"] --> SNAP["DashboardSnapshotBuilder"]
  SNAP --> ANSI["console_dashboard ANSI"]
  SNAP --> TUI["tabbed_dashboard ncurses"]
  SNAP --> DESK["ui/desk ImGui"]
  SNAP --> WEB["web/snapshot_json + SPA"]

  ENG --> AUD["IOrderAuditSink"]
  AUD --> QDB["QuestDB store"]
  ENG --> ELOG["core/event_log zstd"]
  ENG --> RPT["analytics/report_generator<br/>web/report_json"]
```

| Surface | Build flag / mode | Role |
|---------|-------------------|------|
| ANSI dashboard | always (backtest default) | Lightweight status |
| Rich ncurses TUI | shadow/live (`target_uses_rich_tui`) | Positions, orders, L2, risk, brackets, … |
| ImGui desk | `HAS_IMGUI_DESK` | Research / footprint panels |
| Web UI | `ENABLE_WEB` | Read-only snapshot + report SPA |
| QuestDB | `ENABLE_QUESTDB` | Soft-fail ILP audit (`--persist`) |
| Event log | always | Durable zstd record/replay |

---

## 13. Module map (`src/`)

| Module | Path | Responsibility |
|--------|------|----------------|
| **core** | `src/core/` | Events, event log, `TT_TARGET` |
| **types** | `src/types/` | Object pools, price, symbol table, control blocks |
| **engine** | `src/engine/` | Composition root, loop, workers, router, audit, snapshots |
| **providers** | `src/providers/` | Venue MD+exec: local, synthetic, Binance, Bitget, Bitunix |
| **strategy** | `src/strategy/` | `IStrategy`, registry, built-ins |
| **execution** | `src/execution/` | Adapters, portfolio, realism models, live safety ports |
| **orderbook** | `src/orderbook/` | Price-time book + fill model |
| **risk** | `src/risk/` | RiskManager, FuturesRiskCheck, margin tables |
| **exits** | `src/exits/` | ExitManager, DefaultExitPolicy, bracket adapter port |
| **data** | `src/data/` | MarketSeries, DataWrapper, CSV sources, QuestDB |
| **analytics** | `src/analytics/` | Metrics, adverse selection, footprint, reports |
| **indicator** | `src/indicator/` | Pure indicators |
| **threading** | `src/threading/` | RingBuffer, Worker, presets, watchdog |
| **simulation** | `src/simulation/` | Monte Carlo controller / generators / reporter |
| **market_maker** | `src/market_maker/` | Synthetic liquidity / quote management |
| **ui** | `src/ui/` | Dashboards, panels, ImGui desk |
| **web** | `src/web/` | JSON emit, embedded server, frontend SPA |
| **api** | `src/api/` | C API embed (`libtruetest`) |
| **bin** | `src/bin/` | `engine_{backtest,shadow,live}` + shared `main.inc` |

---

## 14. Safety freeze surface (Phase 1)

Mechanical freeze — edits require `LIVE_SAFETY_CCB_APPROVED` + CCB + protocol (`scripts/check-live-safety-freeze.sh`):

```
src/core/tt_target.h
src/engine/engine.cpp
src/providers/binance/binance_futures_provider.h
src/providers/binance/binance_futures_dead_mans_switch.h
src/providers/binance/binance_futures_kill_switch.h
src/providers/binance/binance_futures_reconciler.h
src/risk/risk_manager.h
src/risk/futures_risk_check.h
src/execution/live_safety.h
src/threading/worker_watchdog.h
```

### Safety red lines (compressed)

1. Compile-time live gate absolute — no runtime bypass.  
2. Halt is terminal (write-once).  
3. Kill / DMS / reconciler / watchdog: loud, non-retrying, fail-closed.  
4. Reconciler default-refuse; user-data stream is truth.  
5. Pre-trade: venue `IRiskCheck` before generic `RiskManager` on futures.  
6. No venue `#ifdef` leakage into generic core layers.

---

## 15. Build & verification topology

```mermaid
flowchart LR
  CMAKE["CMake + cmake/Sources.cmake<br/>no globs"] --> PRE["Presets<br/>linux-tests · asan · venues · web"]
  PRE --> BT["engine_backtest"]
  PRE --> SH["engine_shadow"]
  PRE --> LV["engine_live"]
  PRE --> TST["truetest_tests"]

  TST --> CT["ctest"]
  SRC["src/** edit"] --> G1["check-hotpath-json.sh"]
  SRC --> G2["check-layer-deps.sh"]
  SRC --> G3["check-live-safety-freeze.sh"]
```

Optional features: `ENABLE_BINANCE`, `ENABLE_BITGET`, `ENABLE_BITUNIX`, `ENABLE_QUESTDB`, `ENABLE_WEB`, `ENABLE_BENCHMARKS`, `BUILD_SHARED_LIB`, sanitizers.

---

## 16. End-to-end mental model

```
                    ┌─────────────────────────────────────────┐
   CLI / API / MC   │              main.inc / API             │
                    └───────────────────┬─────────────────────┘
                                        │ wire config, provider, strategy
                                        ▼
   MarketSeries / IProvider ──► engine event loop (sole ring producer)
                                        │
                    ┌───────────────────┼───────────────────┐
                    ▼                   ▼                   ▼
               IStrategy(+)      RiskManager+IRiskCheck   Orderbook/MM
                    │                   │
                    └─────────┬─────────┘
                              ▼
                      ExecutionRouter → IExecutionAdapter
                              │
                    fills / rejects / cancels
                              ▼
                 Portfolio · ExitManager · Analytics · AuditSink
                              │
                              ▼ publish_event
                 SPSC rings → Logging / Risk / Stats / Observer / MM
                              │
                              ▼
                 TUI · Web · QuestDB · EventLog · Report JSON
```

**Same hot path** for backtest, shadow, and live.  
**Different edges**: data source, fill truth, whether live transport code exists after DCE.

---

## 17. Pointers

| Topic | Location |
|-------|----------|
| Agent rules / red lines | `AGENTS.md` |
| Operator architecture prose | `docs/reference/02-user-manual.md` |
| Target architecture extract | `docs/architecture/01-target-architecture.md` |
| Realism models | `docs/architecture/03-realism.md` |
| Performance capacities | `docs/architecture/04-performance.md` |
| Engine decomposition plan | `docs/internal/engine-decomposition.md` |
| Data pipeline plan | `docs/internal/data-pipeline.md` |
| CLI / MC / providers | `docs/reference/01-instructions.md` |
| Layer enforcement | `scripts/check-layer-deps.sh` |
| Live-safety freeze | `scripts/check-live-safety-freeze.sh` |
| Production phases | `docs/governance/01-prod.md` |

---

*Generated from a full read-only pass of the `core/` tree. No source code was modified for this document beyond creating `image.md`.*
