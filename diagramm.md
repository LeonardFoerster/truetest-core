# TrueTest HFT-Engine — vollständiges Architekturdiagramm

> Jede Mermaid-Graph unten ist ein eigener Teilausschnitt der Engine. Zusammen decken sie
> **jede Klasse, jeden Worker, jede Ring-Buffer-Verbindung, jeden CLI-Switch, jeden
> Object-Pool, jeden Provider, jeden Fee-/Latency-/Fill-Model-Zweig, jede Risk-Regel
> und jede I/O-Senke** ab, die der Code hergibt.
>
> Zum Betrachten entweder direkt auf GitHub öffnen (Mermaid wird nativ gerendert),
> in VS Code mit Mermaid-Preview-Plugin, oder in einem Browser mit
> `https://mermaid.live/` und dem Inhalt eines Blocks einfügen.

## Inhalt

1. [Top-Level-Datenfluss](#1-top-level-datenfluss)
2. [Build-System & TT_TARGET-Gating](#2-build-system--tt_target-gating)
3. [CLI, Config, Einstiegspunkt `main.inc`](#3-cli-config-einstiegspunkt-maininc)
4. [Engine-Klasse — Membervariablen & Lebenszyklus](#4-engine-klasse--membervariablen--lebenszyklus)
5. [Event-Pipeline & Object-Pools](#5-event-pipeline--object-pools)
6. [Event-Typen (alle Felder)](#6-event-typen-alle-felder)
7. [`run()` — Batch-Mode (Bars + Ticks)](#7-run--batch-mode-bars--ticks)
8. [`run_streaming()` — Live-Mode](#8-run_streaming--live-mode)
9. [`run_replay()` — Event-Log-Wiedergabe](#9-run_replay--event-log-wiedergabe)
10. [Provider-System (Registry, Transport, Parser, Bridge)](#10-provider-system-registry-transport-parser-bridge)
11. [Binance-Provider (Detail, Paper/Hybrid/Live)](#11-binance-provider-detail-paperhybridlive)
12. [Execution-Layer (Adapter, Portfolio, Fees, Latency, Fill-Model)](#12-execution-layer-adapter-portfolio-fees-latency-fill-model)
13. [Orderbuch & Matching](#13-orderbuch--matching)
14. [Strategien & Indikatoren](#14-strategien--indikatoren)
15. [Risk-Manager (alle Regeln)](#15-risk-manager-alle-regeln)
16. [Analytics & Report-Generator](#16-analytics--report-generator)
17. [Threading — Presets, Rings, Spin-Policy, Affinität](#17-threading--presets-rings-spin-policy-affinität)
18. [Worker-Klassen (Detail)](#18-worker-klassen-detail)
19. [Daten-Layer & Persistenz](#19-daten-layer--persistenz)
20. [WebSocket-UI & Kommandos](#20-websocket-ui--kommandos)
21. [Checkpoint-Format & Resume](#21-checkpoint-format--resume)
22. [Binary Event-Log-Format](#22-binary-event-log-format)
23. [C-API (`libtruetest.so`)](#23-c-api-libtruetestso)
24. [Debug-Instrumentierung](#24-debug-instrumentierung)
25. [Speicher-Allokatoren, Queues, Pools — Übersicht](#25-speicher-allokatoren-queues-pools--übersicht)

---

## 1. Top-Level-Datenfluss

```mermaid
flowchart LR
    CLI[[CLI-Argumente<br/>+ JSON-Config-Overlay]] --> TGT{{TT_TARGET<br/>compile-time gate}}
    TGT -->|=1 backtest| BT[engine_backtest]
    TGT -->|=2 shadow| SH[engine_shadow]
    TGT -->|=3 live| LV[engine_live<br/>+ NATIVE_OPT]

    BT --> MAIN[main.inc<br/>1219 LOC]
    SH --> MAIN
    LV --> MAIN

    MAIN -->|--replay| REPL[run_replay_mode]
    MAIN -->|--provider X| PROV[run_provider_mode]
    MAIN -->|CSV / default| BATCH[engine.run]

    REPL --> ENG[[engine-Kern]]
    PROV --> ENG
    BATCH --> ENG

    subgraph IN[Eingänge]
        CSV[(CSV OHLCV)]
        TCSV[(Tick-CSV)]
        BINCACHE[(Binary-Cache)]
        PG[(PostgreSQL<br/>HAS_POSTGRESQL)]
        WS_IN[Binance WS]
        REST[Binance REST]
        RPL[(Event-Log .bin<br/>zstd)]
        CKP[(Checkpoint .bin)]
    end

    IN --> ENG
    CKP -->|resume_checkpoint_path| ENG

    ENG --> CORE[Event-Pipeline<br/>market→strategy→order→book→fill→portfolio]
    CORE --> RINGS((SPSC-Rings))
    RINGS --> WRK[Worker-Pool]

    WRK --> OUT
    CORE --> OUT

    subgraph OUT[Ausgänge]
        STDOUT[stdout / TUI]
        LOG[Log-Datei<br/>rotierend]
        ELG[Binary Event-Log<br/>+ zstd]
        DB[(SQLite<br/>HAS_SQLITE)]
        PG2[(Postgres<br/>HAS_POSTGRESQL)]
        WSUI[WebSocket-UI<br/>HAS_WEB_UI]
        RESTOUT[Binance REST<br/>/api/v3/order]
        CSVEXP[CSV/JSON-Export]
        CKP2[(Checkpoint .bin)]
    end
```

---

## 2. Build-System & TT_TARGET-Gating

```mermaid
flowchart TD
    CMK[CMakeLists.txt] --> CF[cmake/CompilerFlags.cmake<br/>C++23 • per-config opt]
    CMK --> DEP[cmake/Dependencies.cmake<br/>tt_fetch_* • tt_wire_optional_backends]

    CMK --> OBJ[[engine_core<br/>OBJECT library]]

    subgraph FLAGS[ENABLE_* → HAS_* Matrix]
        F1[ENABLE_POSTGRESQL] -->|→| D1[HAS_POSTGRESQL<br/>pg_data_source.cpp<br/>libpqxx, vcpkg]
        F2[ENABLE_LIVE_DATA] -->|→| D2[HAS_LIVE_DATA<br/>websocket_data_source.cpp]
        F3[ENABLE_WEB_UI] -->|→| D3[HAS_WEB_UI<br/>engine/ws_worker.h<br/>Boost.Beast]
        F4[ENABLE_BINANCE] -->|→| D4[HAS_BINANCE<br/>providers/binance/*<br/>Boost.Beast + OpenSSL]
        F5[ENABLE_SQLITE default=ON] -->|→| D5[HAS_SQLITE<br/>data/sqlite_store.cpp]
        F6[ENABLE_DEBUG] -->|→| D6[HAS_DEBUG<br/>debug/* • Abseil]
        F7[ENABLE_BENCHMARKS] -->|→| D7[benchmarks/<br/>Google Benchmark]
        F8[ENABLE_TSAN] -->|xor| S1[-fsanitize=thread]
        F9[ENABLE_ASAN] -->|xor| S2[-fsanitize=address]
        F10[ENABLE_UBSAN] -->|xor| S3[-fsanitize=undefined]
        F11[ENABLE_NATIVE_OPT] --> N1[-march=native<br/>-funroll-loops<br/>nur engine_live Release]
        F12[BUILD_SHARED_LIB] --> SL[libtruetest.so<br/>+ src/api/truetest_api.h]
        F13[BUILD_TESTS] --> TS[truetest_tests<br/>~310 GoogleTest-Fälle]
    end

    DEP -. wires .-> OBJ
    FLAGS -. compile defs .-> OBJ

    OBJ --> B1[engine_backtest<br/>TT_TARGET=1]
    OBJ --> B2[engine_shadow<br/>TT_TARGET=2]
    OBJ --> B3[engine_live<br/>TT_TARGET=3<br/>native-opt]
    OBJ --> SL
    OBJ --> TS

    B1 -.-> TT{core/tt_target.h}
    B2 -.-> TT
    B3 -.-> TT
    TT -->|target_allows_live_orders| GATE{Live-Order<br/>erlaubt?}
    GATE -->|TT_TARGET=3 only| YES[echte REST-Orders]
    GATE -->|sonst| NO[reject --mode=live]
```

---

## 3. CLI, Config, Einstiegspunkt `main.inc`

```mermaid
flowchart TD
    START([argc,argv]) --> PARSE[register_cli_options<br/>~50 CLI11-Optionen]
    PARSE --> CFGFILE{--config-file?}
    CFGFILE -->|ja| OVERLAY[load_config_file<br/>JSON → cli_options]
    CFGFILE -->|nein| SIG
    OVERLAY --> SIG[Signal-Handler<br/>SIGINT/SIGTERM → bridge.stop]

    SIG --> DRY{--dry-run?}
    DRY -->|ja| DUMP[dump_config → exit 0/1]
    DRY -->|nein| MODE{Mode wählen}

    MODE -->|--replay PATH| RP[run_replay_mode]
    MODE -->|--provider X| PR[run_provider_mode]
    MODE -->|sonst| CSV_MODE[CSV / TUI]

    subgraph CLI[cli_options — alle Felder]
        direction LR
        C1[Data/Logging<br/>replay_path<br/>replay_from/to_us<br/>event_log_path<br/>log_file_path<br/>log_max_size_mb<br/>log_keep<br/>compress_log<br/>seed<br/>thread_preset_str<br/>spin_policy_str<br/>no_pin]
        C2[Provider<br/>provider_name<br/>provider_path<br/>symbol<br/>stream<br/>api_key/secret<br/>host/port<br/>record_path<br/>replay_data_path<br/>live / testnet]
        C3[Strategy<br/>strategy<br/>format<br/>sma_period<br/>params k=v]
        C4[Mode<br/>mode<br/>TT_DEFAULT_MODE]
        C5[Fees<br/>fee_model<br/>fee_value<br/>maker_rate<br/>taker_rate]
        C6[UI<br/>enable_web_ui<br/>ws_port<br/>ws_compress]
        C7[Portfolio<br/>balance<br/>risk_fraction<br/>sl_pct<br/>tp_pct]
        C8[DB<br/>db_path<br/>no_db]
        C9[Checkpoint<br/>checkpoint_path<br/>resume_path<br/>checkpoint_interval]
        C10[Backfill<br/>backfill<br/>backfill_interval]
        C11[Execution<br/>market_aggression<br/>qty_scale<br/>fill_rng_seed<br/>spread_step<br/>debug_fills<br/>debug_fills_budget]
        C12[Risk<br/>max_position_value<br/>max_drawdown<br/>max_loss_per_trade<br/>max_open_orders<br/>max_portfolio_exposure<br/>max_daily_loss<br/>daily_reset_hour<br/>max_trades_per_hour<br/>max_orders_per_minute<br/>risk_unwind]
        C13[Analysis<br/>rolling_window<br/>risk_free_rate<br/>output<br/>output_format]
        C14[Meta<br/>config_file<br/>dump_config_flag<br/>dry_run]
    end

    PARSE --> CLI

    RP --> E1[engine.run_replay<br/>path, from_us, to_us]
    PR --> PRV[ProviderRegistry.create]
    PRV --> E2[engine.run_streaming]
    CSV_MODE --> DL[data_handler.load_from_csv]
    DL --> E3{has_tick_data?}
    E3 -->|ja| E3T[engine.run_tick_data]
    E3 -->|nein| E3B[engine.run]

    E1 --> FIN[export_results CSV/JSON<br/>engine.print_summary]
    E2 --> FIN
    E3T --> FIN
    E3B --> FIN
    FIN --> EXIT([exit-code])
```

---

## 4. Engine-Klasse — Membervariablen & Lebenszyklus

```mermaid
classDiagram
    class engine {
        +engine_config config_
        +shared_ptr~data_handler~ data_handler_
        +OrderbookRegistry orderbook_registry_
        +shared_ptr~IStrategy~ strategy_
        +vector~shared_ptr~IStrategy~~ additional_strategies_
        +unordered_map~string,shared_ptr~IExecutionAdapter~~ execution_adapters_
        +portfolio portfolio_
        +OrderTracker order_tracker_
        +Analytics analytics_
        +RiskManager risk_manager_
        +MarketMaker market_maker_
        +double last_mid_price_
        +unique_ptr~BarAggregator~ tick_aggregator_
        +ObjectPool~market_event~ market_pool_
        +ObjectPool~order_event~ order_pool_
        +ObjectPool~fill_event~ fill_pool_
        +ObjectPool~tick_event~ tick_pool_
        +RingBuffer logging_ring_
        +RingBuffer risk_ring_
        +RingBuffer stats_ring_
        +RingBuffer observer_ring_
        +RingBuffer risk_stats_ring_
        +RingBuffer mm_ring_
        +RingBuffer mm_order_ring_
        +unique_ptr~SqliteStore~ store_
        +string current_run_id_
        +unique_ptr~EventLogger~ event_logger_
        +unique_ptr~ShadowTracker~ shadow_tracker_
        +priority_queue~pending_entry~ pending_orders_
        +vector~shared_ptr~order_event~~ pending_stops_
        +atomic~bool~ halt_flag_
        +atomic~bool~ worker_failed_
        +size_t logging_drops_
        +size_t risk_drops_
        +size_t stats_drops_
        +vector~thread~ worker_threads_
        +unique_ptr~LoggingWorker~ logging_worker_
        +unique_ptr~RiskWorker~ risk_worker_
        +unique_ptr~StatsWorker~ stats_worker_
        +unique_ptr~ObserverWorker~ observer_worker_
        +unique_ptr~RiskStatsWorker~ risk_stats_worker_
        +unique_ptr~MarketMakerWorker~ mm_worker_
        +unique_ptr~WebSocketWorker~ ws_worker_
        +shared_ptr~EventRing~ ws_ring_
        +vector~string~ bar_history_
        +mutex switch_mu_
        +string pending_symbol_
        +string pending_strategy_
        +run() void
        +run_streaming(bridge) void
        +run_replay(path,from,to) void
        +run_tick_data() void
        +process_single_bar() void
        +process_single_tick() void
        +route_order(order) void
        +unwind_positions() void
        +write_checkpoint_if_due() void
        +restore_from_checkpoint() void
        +start_workers() void
        +stop_workers() void
        +pin_event_loop_thread() void
        +get_adapter(symbol) shared_ptr
        +check_pending_stops() void
        +dispatch_extras_on_market() void
        +print_summary() void
    }

    class engine_config {
        +string mode
        +string thread_preset
        +string spin_policy
        +double initial_balance
        +double risk_fraction
        +double sl_pct
        +double tp_pct
        +fee_model_type fee_model
        +double fee_value
        +double maker_rate
        +double taker_rate
        +risk_limits risk
        +string db_path
        +bool no_db
        +string event_log_path
        +string checkpoint_path
        +string resume_checkpoint_path
        +size_t checkpoint_interval_events
        +bool enable_web_ui
        +uint16_t ws_port
        +bool ws_compress
        +int backfill_bars
        +string backfill_interval
        +double market_aggression
        +double qty_scale
        +uint64_t fill_rng_seed
        +double spread_step_factor
        +size_t rolling_window
        +double risk_free_rate
        +string output_path
        +string output_format
        +size_t log_max_size_mb
        +size_t log_keep
        +bool compress_log
        +uint64_t seed
    }

    engine --> engine_config
    engine --> portfolio
    engine --> Analytics
    engine --> RiskManager
    engine --> MarketMaker
    engine --> OrderbookRegistry
    engine --> IStrategy
    engine --> IExecutionAdapter
    engine --> EventLogger
    engine --> SqliteStore
    engine --> ShadowTracker
```

---

## 5. Event-Pipeline & Object-Pools

```mermaid
flowchart LR
    subgraph POOLS[ObjectPool T, BlockSize=4096 — src/types/object_pool.h]
        MP[(market_pool)]
        OP[(order_pool)]
        FP[(fill_pool)]
        TP[(tick_pool)]
    end

    IN[Provider-Event<br/>bar / tick / l2] --> CONV[provider_convert.h]
    CONV -->|alloc| MP
    CONV -->|alloc| TP
    MP --> ME[market_event]
    TP --> TE[tick_event]

    ME --> STRAT[strategy.on_market]
    TE --> STRAT2[strategy.on_tick<br/>optional]

    STRAT -->|optional<order_event>| OP
    STRAT2 -->|optional<order_event>| OP
    OP --> OE[order_event]

    OE --> PQ[[priority_queue<br/>pending_orders_<br/>by earliest_eligible_ts, seq]]
    PQ --> RISK1{RiskManager<br/>check_order}
    RISK1 -->|reject| DROP1[drop]
    RISK1 -->|halt| HALT[halt_flag_=true]
    RISK1 -->|pass| ADAPT{get_adapter<br/>per Symbol}

    ADAPT -->|local| LBA[LocalBookAdapter]
    ADAPT -->|binance paper| BE[BinanceExecutor]
    ADAPT -->|binance hybrid| HE[HybridExecutor]
    ADAPT -->|binance live| EB[ExecutionBridge<br/>REST]

    LBA --> OB[(orderbook)]
    HE -->|market| BE
    HE -->|limit| OB
    EB -->|REST POST| BN[Binance /api/v3/order]

    OB --> TRD[trades]
    TRD -->|alloc| FP
    BE --> FP
    EB -->|user-data WS| FP
    FP --> FE[fill_event]

    FE --> RISK2{RiskManager<br/>check_post_fill}
    RISK2 -->|halt+unwind| UNW[unwind_positions<br/>market-sell alle]
    RISK2 -->|pass| PF[portfolio.on_fill<br/>cash, positions]
    PF --> AN[analytics.on_fill<br/>Welford, PnL]
    AN --> PUB[Publish in alle Rings]
    PUB --> R1[[logging_ring]]
    PUB --> R2[[risk_ring]]
    PUB --> R3[[stats_ring]]
    PUB --> R4[[observer_ring]]
    PUB --> R5[[ws_ring]]
```

---

## 6. Event-Typen (alle Felder)

```mermaid
classDiagram
    class event {
        <<abstract>>
        +event_type type
        +time_point timestamp
        +uint64_t recv_ns_
        +uint64_t latency_ns_
        +to_string() string
    }

    class event_type {
        <<enum>>
        market
        signal
        order
        fill
        tick
        l2_snapshot
        l2_update
        cancel
        amend
        rejection
    }

    class market_event {
        +string symbol
        +double open
        +double high
        +double low
        +double close
        +int64_t volume
    }

    class signal_event {
        +string symbol
        +signal_type signal
        +double strength
    }

    class order_event {
        +uint64_t order_id
        +string symbol
        +order_type type
        +order_side side
        +double quantity
        +double price
        +tif_type tif
        +double stop_price
        +uint64_t earliest_eligible_ts
        +string strategy_name
    }

    class fill_event {
        +uint64_t order_id
        +string symbol
        +order_side side
        +double filled_quantity
        +double fill_price
        +double commission
        +double remaining_qty
        +uint64_t fill_id
        +fill_source source
        +get_total_cost() double
        +is_partial() bool
    }

    class tick_event {
        +string symbol
        +double price
        +int64_t quantity
        +tick_side side
    }

    class l2_snapshot_event {
        +string symbol
        +vector~l2_level~ bids
        +vector~l2_level~ asks
    }

    class l2_update_event {
        +string symbol
        +side_t side
        +double price
        +double new_quantity
    }

    class cancel_event {
        +uint64_t order_id
        +string symbol
    }
    class amend_event {
        +uint64_t order_id
        +double new_price
        +double new_qty
    }
    class rejection_event {
        +uint64_t order_id
        +string reason
    }

    event <|-- market_event
    event <|-- signal_event
    event <|-- order_event
    event <|-- fill_event
    event <|-- tick_event
    event <|-- l2_snapshot_event
    event <|-- l2_update_event
    event <|-- cancel_event
    event <|-- amend_event
    event <|-- rejection_event
```

---

## 7. `run()` — Batch-Mode (Bars + Ticks)

```mermaid
flowchart TD
    S([engine.run]) --> SW[start_workers<br/>pro thread_preset]
    SW --> PIN[pin_event_loop_thread<br/>sched_setaffinity]
    PIN --> SQL[SqliteStore.record_run_begin<br/>HAS_SQLITE]
    SQL --> LOAD{has_bar_data<br/>or has_tick_data?}

    LOAD --> LOOP{while not halt_flag_<br/>and data verfügbar}
    LOOP -->|bar vorhanden| BAR[dequeue bar_record]
    BAR --> PSB[process_single_bar]
    PSB --> PSB1[update last_mid_price_]
    PSB1 --> PSB2[analytics_.on_market]
    PSB2 --> PSB3[strategy_.on_market]
    PSB3 --> PSB4{optional order?}
    PSB4 -->|ja| PSB5[queue in pending_orders_<br/>earliest_eligible_ts + seq]
    PSB4 -->|nein| PSB6
    PSB5 --> PSB6[dispatch_extras_on_market<br/>shadow + WS broadcast]
    PSB6 --> PSB7[BarAggregator update<br/>tick→bar]

    LOOP -->|tick vorhanden| TICK[process_single_tick]
    TICK --> TCK1[apply L2 updates]
    TCK1 --> TCK2[strategy_.on_tick]

    PSB7 --> PEND[Pending Orders prüfen]
    TCK2 --> PEND
    PEND --> PPOP{pop eligible<br/>by ts,seq}
    PPOP --> ROUTE[route_order]
    ROUTE --> RC1{check_order<br/>pre-fill}
    RC1 -->|reject/halt| SKIP
    RC1 -->|pass| ADP[adapter.submit_order]
    ADP --> PUBO[publish order_event in Rings]
    PUBO --> CPS[check_pending_stops<br/>high/low/ts]

    CPS --> POLL[poll_fills für jedes Adapter]
    SKIP --> POLL
    POLL --> FEACH{für jede fill_event}
    FEACH --> FRC[risk_manager_.check_post_fill]
    FRC -->|halt| SETHALT[halt_flag_=true]
    FRC -->|pass| PFILL[portfolio_.on_fill]
    PFILL --> AFILL[analytics_.on_fill]
    AFILL --> PUBF[publish fill_event in Rings]

    PUBF --> UNW{risk_unwind<br/>und halt?}
    SETHALT --> UNW
    UNW -->|ja| UNWD[unwind_positions]
    UNW -->|nein| CKP
    UNWD --> CKP[write_checkpoint_if_due<br/>alle checkpoint_interval_events]
    CKP --> WSCMD[WebSocket-Kommandos verarbeiten]
    WSCMD --> INC[event_count++]
    INC --> LOOP

    LOOP -->|fertig| STOP[stop_workers<br/>running_=false]
    STOP --> DRAIN[alle Rings drainen]
    DRAIN --> END[SqliteStore.record_run_end]
    END --> REP[analytics_.generate_report<br/>equity curve, Sharpe, DD]
    REP --> EXP[export CSV/JSON]
    EXP --> DONE([return])
```

---

## 8. `run_streaming()` — Live-Mode

```mermaid
flowchart LR
    BRIDGE[DataBridge~T~<br/>transport+parser] --> STR[engine.run_streaming]
    STR --> ATTACH[attach sink-callback]
    ATTACH --> BLOCK[bridge.run_streaming<br/>blocking loop]

    BLOCK --> FRAME[transport.read_frame_blocking]
    FRAME --> PARSE[parser.parse_record]
    PARSE --> PE{provider_event-Variant}
    PE -->|bar| B[provider::bar]
    PE -->|tick| T[provider::tick]
    PE -->|l2_snapshot| LS[provider::l2_snapshot]
    PE -->|l2_update| LU[provider::l2_update]
    PE -->|status| ST[provider::status<br/>connected/disconnected/error]

    B --> CONV[provider_convert<br/>→ market_event]
    T --> CONV2[provider_convert<br/>→ tick_event]
    LS --> OB1[orderbook.apply_l2_snapshot]
    LU --> OB2[orderbook.apply_l2_update]
    ST --> LOG[event_log + UI]

    CONV --> HP[Hot-Path wie run]
    CONV2 --> HP

    HP --> SWITCH{switch_mu_<br/>pending_symbol/strategy?}
    SWITCH -->|ja| SW1[re-instanziere Strategy/Symbol]
    SWITCH -->|nein| NEXT[nächster Frame]
    SW1 --> NEXT
    NEXT --> FRAME

    SIGINT[SIGINT/SIGTERM] -.-> RQST[bridge.stop<br/>request_stop]
    RQST -.-> BLOCK
```

---

## 9. `run_replay()` — Event-Log-Wiedergabe

```mermaid
flowchart TD
    RS([engine.run_replay<br/>path, from_us, to_us]) --> OPEN[EventLogger.load]
    OPEN --> DECOMP{compress_log?}
    DECOMP -->|ja| ZSTD[zstd-Dekompression<br/>ZSTD_DCtx]
    DECOMP -->|nein| RAW
    ZSTD --> RAW[Raw-Bytes]
    RAW --> BR[BufReader<br/>read_u8/u16/u32/u64<br/>read_i64/f64/str/ts]

    BR --> ITR[iterate Events]
    ITR --> FILT{timestamp in<br/>from_us..to_us?}
    FILT -->|nein| SKIP1[skip]
    FILT -->|ja| DECODE{event_type}

    DECODE -->|market| RM[re-inject in strategy+analytics]
    DECODE -->|tick| RT[re-inject in strategy]
    DECODE -->|order| RO[re-inject in portfolio+adapter]
    DECODE -->|fill| RF[portfolio.on_fill + analytics.on_fill]
    DECODE -->|l2_snapshot| RLS[orderbook.apply_l2_snapshot]
    DECODE -->|l2_update| RLU[orderbook.apply_l2_update]
    DECODE -->|cancel| RC[adapter.cancel_order]
    DECODE -->|amend| RA[adapter.modify_order]
    DECODE -->|rejection| RJ[log only]

    RM --> NEXT[next event]
    RT --> NEXT
    RO --> NEXT
    RF --> NEXT
    RLS --> NEXT
    RLU --> NEXT
    RC --> NEXT
    RA --> NEXT
    RJ --> NEXT
    SKIP1 --> NEXT
    NEXT --> ITR

    ITR -->|EOF| FIN[analytics.generate_report]
    FIN --> EXIT([Ende])
```

---

## 10. Provider-System (Registry, Transport, Parser, Bridge)

```mermaid
flowchart TB
    subgraph IF[Interfaces]
        IP[IProvider<br/>name / open / close<br/>has_data_feed / has_execution<br/>configure / on_mid_price<br/>lifecycle_state<br/>get_transport / get_execution_adapter]
        IDT[IDataTransport<br/>open / close / is_open<br/>read_line / read_line_blocking<br/>read_frame / read_frame_blocking<br/>is_streaming / request_stop]
        IDP[IDataParser~T~<br/>parse_header<br/>parse_record → optional~T~]
        IEA[IExecutionAdapter<br/>submit_order<br/>poll_fills<br/>cancel_order<br/>modify_order]
    end

    IF --> REG[ProviderRegistry<br/>Singleton • REGISTER_PROVIDER macro]
    REG -->|factory lookup| IP

    subgraph EVT[provider_event-Variant]
        PEB[provider::bar<br/>date,sym,O,H,L,C,V]
        PET[provider::tick<br/>ts,sym,price,qty,side]
        PELS[provider::l2_snapshot<br/>bids,asks]
        PELU[provider::l2_update<br/>side,price,qty]
        PES[provider::status<br/>connected/disconn/error/info]
    end

    IDP -->|yields| EVT

    DB[DataBridge~T~<br/>transport + parser + sink] --> IDT
    DB --> IDP
    DB -->|batch| BATCH[load_data<br/>alle Records → data_handler]
    DB -->|streaming| STREAM[run_streaming<br/>blocking read-loop]

    PT[PrependTransport<br/>Decorator<br/>yields Backfill-Zeilen vor Live] --> IDT

    subgraph LP[LocalProvider — always-on]
        LPV[LocalProvider]
        FT[FileTransport<br/>std::ifstream]
        CSVP[CsvParser~bar_record~]
    end

    subgraph BP["BinanceProvider (HAS_BINANCE)"]
        BPV[BinanceProvider]
        BTR[BinanceTransport<br/>WS Boost.Beast]
        BCT[BinanceCombinedTransport<br/>multi-stream]
        BPR[BinanceParser<br/>JSON handrolled]
        BDP[BinanceDepthParser]
        BUDP[BinanceUserDataParser]
        BCP[BinanceCombinedParser]
    end

    IP -.implement.-> LPV
    IP -.implement.-> BPV
    IDT -.impl.-> FT
    IDT -.impl.-> BTR
    IDT -.impl.-> BCT
    IDT -.impl.-> PT
    IDP -.impl.-> CSVP
    IDP -.impl.-> BPR
    IDP -.impl.-> BDP
    IDP -.impl.-> BUDP
    IDP -.impl.-> BCP

    REG -. REGISTER_PROVIDER 'local' .-> LPV
    REG -. REGISTER_PROVIDER 'binance' .-> BPV
```

---

## 11. Binance-Provider (Detail, Paper/Hybrid/Live)

```mermaid
flowchart TD
    OP[BinanceProvider.open] --> EP[binance::endpoints<br/>from_host → mainnet/testnet]
    EP --> WS[BinanceTransport<br/>stream_type:<br/>trade / kline_1m / depth]

    WS --> BF{backfill_bars > 0<br/>und kline-Stream?}
    BF -->|ja| BFT[BinanceBackfill<br/>GET /api/v3/klines<br/>limit=1000, paginate]
    BFT --> ENC[encode als synthetisches kline-JSON]
    ENC --> PT[PrependTransport<br/>inject vor Live-Stream]
    PT --> OUT1[transport_ final]
    BF -->|nein| OUT1

    OP --> MODECHK{mode?}
    MODECHK -->|live + api_key| LIVE[LIVE-PFAD]
    MODECHK -->|paper/shadow/backtest| PAPER[PAPER-PFAD]

    subgraph LIVE[LIVE - echte REST-Orders]
        CLK[binance::verify_clock_skew<br/>±1000ms, server_time_ms]
        REST[BinanceRestClient<br/>GET/POST/DEL<br/>HMAC-SHA256 signed<br/>weight_cap 1200/min]
        AUTH[binance_auth.h<br/>sign_query]
        ORDTX[BinanceRestOrderTransport<br/>POST /api/v3/order]
        OENC[BinanceOrderEncoder<br/>BTCUSDT encoding]
        UDT[BinanceUserDataTransport<br/>listenKey<br/>WS /ws/<listenKey><br/>PUT alle 30min]
        UDP[BinanceUserDataParser<br/>executionReport]
        EB[ExecutionBridge<br/>order_tx + fill_tx + encoder + parser]

        CLK --> REST
        AUTH --> REST
        REST --> ORDTX
        REST --> UDT
        OENC --> EB
        UDP --> EB
        ORDTX --> EB
        UDT --> EB
    end

    subgraph PAPER[PAPER/HYBRID]
        LB[LocalBookAdapter<br/>orderbook-Matching]
        PX[BinanceExecutor<br/>PAPER-Fills am last_price]
        HX[HybridExecutor<br/>market→paper<br/>limit→book<br/>on_mid_price seedet 10 Levels je Seite]
        HX --> PX
        HX --> LB
    end

    LIVE --> EXEC[executor_ = ExecutionBridge]
    PAPER --> EXEC2[executor_ = HybridExecutor]

    subgraph REC[Record/Replay]
        REC1[BinanceRecorder<br/>Live-WS → Datei]
        REC2[BinanceReplayTransport<br/>Datei → Transport-Interface]
    end

    WS -.-> REC1
    REC2 -.-> BPV[BinanceProvider]

    subgraph STREAMS[Unterstützte Streams]
        S1[<sym>@trade]
        S2[<sym>@kline_1m..1M]
        S3[<sym>@depth]
        S4[combined /stream?streams=...]
    end
```

---

## 12. Execution-Layer (Adapter, Portfolio, Fees, Latency, Fill-Model)

```mermaid
flowchart LR
    subgraph IFACE[Interfaces]
        IEA[IExecutionAdapter]
        IFM[IFeeModel<br/>compute_commission<br/>side,qty,price,is_taker]
        ILM[ILatencyModel<br/>get_order_latency<br/>get_market_data_latency]
        IFILL[IFillModel<br/>get_fade_rate<br/>get_fill_probability]
    end

    subgraph FEE[Fee-Impls]
        F1[ZeroFeeModel<br/>→ 0]
        F2[FixedFeeModel<br/>fee_per_trade]
        F3[TieredFeeModel<br/>notional × maker/taker-Rate]
    end
    IFM --> FEE

    subgraph LAT[Latency-Impls]
        L1[ZeroLatencyModel]
        L2[FixedLatencyModel<br/>order_lat, md_lat]
        L3[StochasticLatencyModel<br/>normal_distribution]
    end
    ILM --> LAT

    subgraph FILL[Fill-Impls]
        FI1[PerfectFillModel<br/>fade=0, prob=1]
        FI2[RealisticFillModel<br/>fade_rate<br/>base_fill_prob<br/>distance_decay<br/>prob = base × exp-decay × abs-dist]
    end
    IFILL --> FILL

    subgraph ADPT[Adapter]
        A1[LocalBookAdapter<br/>ob_<br/>fee_model_<br/>fill_model_<br/>mt19937 fill_rng_<br/>uniform_real_distribution<br/>mid_price_<br/>market_aggression_<br/>qty_scale_<br/>debug_fills_<br/>next_fill_id_]
        A2[BinanceExecutor<br/>PAPER]
        A3[HybridExecutor<br/>market→paper, limit→book<br/>mid-seed L2]
        A4[ExecutionBridge<br/>LIVE REST + user-data]
    end
    IEA --> ADPT

    A1 --> OB[(orderbook)]
    A3 --> A2
    A3 --> OB
    A4 --> REST2[Binance REST]

    subgraph PF[portfolio]
        PFA[initial_balance_<br/>cash_<br/>positions_ map symbol→position<br/>total_trades_<br/>total_fills_]
        PFM1[on_fill<br/>buy: cash -= qty×px+com<br/>sell: cash += qty×px-com]
        PFM2[position_open / position_open symbol]
        PFM3[can_afford side,qty,price]
        PFM4[compute_quantity<br/>cash × risk_fraction / price]
        PFM5[get_equity last_price<br/>cash + Σ qty × last_price]
        PFM6[restore_state<br/>checkpoint resume]
    end

    A1 -->|fill_event| PF
    A2 -->|fill_event| PF
    A3 -->|fill_event| PF
    A4 -->|fill_event| PF

    subgraph OT[OrderTracker]
        OT1[orders_map: live Orders per Symbol]
        OT2[state: new, partial, filled, canceled, rejected]
    end

    A1 -.-> OT
    A2 -.-> OT
    A3 -.-> OT
    A4 -.-> OT
```

---

## 13. Orderbuch & Matching

```mermaid
flowchart TD
    subgraph OBReg[OrderbookRegistry]
        REG[unordered_map<br/>symbol → shared_ptr-orderbook]
        REG_GOC[get_or_create symbol]
    end

    subgraph OB[orderbook — price-time priority]
        BL[bid_levels_<br/>sortierte Vektoren]
        AL[ask_levels_<br/>sortierte Vektoren]
        OM[order_map_<br/>order_id → order_node*<br/>O(1) lookup]
        NP[node_pool<br/>vector<unique_ptr-node_block><br/>4096 nodes/block]

        PL[price_level<br/>{Price, total_qty,<br/>order_node* head/tail}<br/>doubly-linked list]
        ON[order_node<br/>{shared_ptr-order, next, prev}]

        BL --> PL
        AL --> PL
        PL --> ON
        NP --> ON
        OM --> ON
    end

    subgraph METHODS[Methoden]
        M1[add_order → trades<br/>matched immediately]
        M2[cancel_order id]
        M3[match_order amend → trades]
        M4[modify_order id,px,qty]
        M5[apply_l2_snapshot bids,asks<br/>ganzes Buch ersetzen]
        M6[apply_l2_update side,px,qty<br/>qty=0 → level löschen]
        M7[get_order_infos → Snapshot]
    end

    subgraph ORD[order]
        O1[order_id]
        O2[side: buy/sell]
        O3[Price fixed-point]
        O4[quantity initial/remaining/filled]
        O5[order_type: GTC/FOK/IOC]
        O6[fill qty]
        O7[is_filled]
    end

    subgraph TR[Trade-Struktur]
        TR1[bid_trade: order_id, Price, quantity]
        TR2[ask_trade: order_id, Price, quantity]
    end

    REG --> OB
    M1 --> TR
    M3 --> TR
    OB --> METHODS
    OB --> ORD

    subgraph FILLMDL[fill_model.h — Wahrscheinlichkeits-Gating]
        FM1[get_fill_probability<br/>side,distance_from_mid]
        FM2[decide(rng): fill/no-fill]
    end
    METHODS -.wird aus LocalBookAdapter gerufen.-> FILLMDL
```

---

## 14. Strategien & Indikatoren

```mermaid
flowchart LR
    subgraph IS[IStrategy — Interface]
        S1[on_market event → optional-order_event]
        S2[on_tick event → optional-order_event]
        S3[on_l2_update → optional-order_event]
        S4[set_position_open symbol,open]
        S5[set_stops symbol,sl,tp,qty]
        S6[check_stops symbol,price,ts → optional-order]
        S7[get_param_schema → vector-param_def]
        S8[set_param key,value]
        S9[get_indicator_values symbol → pairs]
    end

    subgraph REG[StrategyRegistry — Singleton]
        R1[register_strategy name,factory]
        R2[create name → shared_ptr-IStrategy]
        R3[available]
        R4[has name]
        R5[REGISTER_STRATEGY macro<br/>statische Init-Zeit]
    end

    REG --> IS

    subgraph IMPL[Konkrete Strategien]
        I1[sma_strategy<br/>period=20<br/>buy: close > SMA<br/>sell: close < SMA]
        I2[mean_reversion_strategy<br/>RSI oversold/overbought<br/>+ SMA-Trendbestätigung<br/>dynamisches Sizing]
        I3[ma_crossover_strategy<br/>SMA fast / slow<br/>buy: fast > slow<br/>sell: Crossunder]
    end

    IS -.implement.-> IMPL

    subgraph IND[Indikatoren — src/indicator/]
        N1[simple_moving_average<br/>queue<double> window<br/>double sum<br/>update→optional<double>]
        N2[exponential_moving_average<br/>alpha]
        N3[rsi<br/>14-period default<br/>avg_gain / avg_loss]
        N4[bollinger<br/>SMA + stddev bands<br/>upper/lower]
    end

    IMPL --> IND
    I1 --> N1
    I2 --> N1
    I2 --> N3
    I3 --> N1

    REG -->|"main.inc: --strategy a,b,c"| MULTI[Multi-Strategy-Mode<br/>primary + additional_strategies_]
```

---

## 15. Risk-Manager (alle Regeln)

```mermaid
flowchart TD
    subgraph LIMITS[risk_limits]
        L1[max_position_value<br/>default 1e9]
        L2[max_drawdown<br/>default 0.30]
        L3[max_loss_per_trade<br/>default 10000]
        L4[max_open_orders<br/>default 1000]
        L5[max_portfolio_exposure<br/>default 5e9]
        L6[max_daily_loss<br/>0=off]
        L7[daily_reset_hour UTC<br/>default 0]
        L8[max_trades_per_hour<br/>0=off]
        L9[max_orders_per_minute<br/>0=off]
    end

    subgraph ACTION[risk_action — enum]
        A1[pass]
        A2[reject]
        A3[halt]
        A4[unwind]
    end

    subgraph RM[RiskManager]
        RMC[check_order<br/>pre-fill]
        RMP[check_post_fill]
        RMF[on_fill → update counters]
        RMS[rolling daily/hourly/minute<br/>Counter-Reset nach Fenster]
    end

    ORDER[order_event] --> RMC
    RMC --> CHKS{Checks}
    CHKS -->|position_value > max| A2
    CHKS -->|open_orders > max| A2
    CHKS -->|exposure > max| A2
    CHKS -->|loss/trade > max| A2
    CHKS -->|trades/h > max| A2
    CHKS -->|orders/min > max| A2
    CHKS -->|alles OK| A1

    FILL[fill_event] --> RMP
    RMP --> CHKS2{Checks post-fill}
    CHKS2 -->|drawdown > max| A3
    CHKS2 -->|daily_loss > max| A3
    CHKS2 -->|risk_unwind gesetzt| A4
    CHKS2 -->|sonst| A1

    A3 --> FLAG[halt_flag_=true]
    A4 --> UNW[unwind_positions<br/>Market-Sell alle]
    A2 --> DROP[order verworfen]

    FILL --> RMF
    RMF --> RMS
```

---

## 16. Analytics & Report-Generator

```mermaid
flowchart LR
    subgraph IN[Inputs]
        ME[market_event]
        OE[order_event]
        FE[fill_event]
    end

    IN --> ANA[Analytics : Worker]

    subgraph ANA[Analytics]
        A1[initial_equity]
        A2[vector-equity_point]
        A3[vector-trade_record]
        A4[vector-double returns]
        A5[Welford<br/>mean, variance, stddev]
        A6[on_event dispatcher]
        A7[snapshot → AnalyticsReport]
        A8[generate_report]
        A9[print_report]
        A10[export_csv / export_json]
    end

    subgraph REP[AnalyticsReport]
        R1[initial/final_equity<br/>cumulative_return]
        R2[equity_curve ts,eq]
        R3[sharpe_ratio<br/>sortino_ratio<br/>max_drawdown<br/>calmar_ratio]
        R4[total_trades/orders/fills<br/>win_rate<br/>avg_win/loss<br/>profit_factor]
        R5[slippage<br/>avg/fav/adv<br/>counts]
        R6[latency<br/>tick-to-trade<br/>min/max/avg_ns]
        R7[time_in_market<br/>avg_holding]
        R8[per-symbol<br/>win_rate, total_pnl, PF]
        R9[per-strategy<br/>Aufschlüsselung]
        R10[benchmark<br/>buy-and-hold<br/>alpha, beta<br/>information_ratio<br/>tracking_error]
        R11[trades-vector<br/>order_id, side, qty, fill_price,<br/>commission, intended_price,<br/>ts, pnl, symbol, strategy]
    end

    ANA --> REP

    subgraph HELP[Hilfsklassen]
        BA[BarAggregator<br/>tick→bar mit Intervall]
        ST[ShadowTracker<br/>reale vs. simulierte Fills<br/>PnL-Drift-Forensik]
    end

    BA --> ANA
    ST --> ANA
```

---

## 17. Threading — Presets, Rings, Spin-Policy, Affinität

```mermaid
flowchart TB
    subgraph PRE[thread_preset — auto-select]
        P0[≤2 cores → inline_mode]
        P1[3 cores → light]
        P2[4-5 cores → standard]
        P3[6-7 cores → full]
        P4[8+ cores → extended]
    end

    subgraph MAP[Worker-Mapping]
        M0[inline: alles im Event-Loop]
        M1[light: ObserverWorker + stats_ring + observer_ring]
        M2[standard: LoggingWorker + RiskStatsWorker]
        M3[full: LoggingWorker + RiskWorker + StatsWorker]
        M4[extended: + MarketMakerWorker]
    end

    PRE --> MAP

    subgraph SP[spin_policy]
        SP1[spin — tight loop]
        SP2[yield — std::this_thread::yield]
        SP3[adaptive — 64 spin → 256 _mm_pause → yield]
    end

    subgraph RB["RingBuffer T, N=65536, Policy"]
        RB1[SPSC lock-free]
        RB2[capacity=power of 2]
        RB3[Policies:<br/>SpinWait<br/>DropOldest<br/>AssertFull]
        RB4[acquire/release mem-order]
        RB5[high_watermark tracking]
        RB6[drop_count metrics]
        RB7[try_push/try_pop<br/>push<br/>size/full/empty<br/>occupancy<br/>on_watermark threshold,cb]
    end

    subgraph RINGS[Alle Rings im Engine]
        RG1[[logging_ring]]
        RG2[[risk_ring]]
        RG3[[stats_ring]]
        RG4[[observer_ring]]
        RG5[[risk_stats_ring]]
        RG6[[mm_ring]]
        RG7[[mm_order_ring]]
        RG8[[ws_ring]]
    end

    RB -.-> RINGS

    subgraph AFF[thread_config — CPU-Affinität]
        AF1[detect_physical_cores]
        AF2[pin_current_thread core_id]
        AF3[sched_setaffinity Linux]
        AF4[Event-Loop → Core 0]
        AF5[Worker → Core 1..N]
        AF6[--no-pin überspringt Pinning]
    end

    MAP --> RINGS
    SP --> RB
    AFF --> RINGS

    subgraph W[Worker-Basisklasse]
        W1[virtual on_event event_pointer]
        W2[run RingBuffer&:<br/>while running_: try_pop<br/>on_event<br/>max_consecutive_errors<br/>backoff per spin_policy<br/>drain on exit]
        W3[Metriken<br/>error_count<br/>exception_ptr<br/>idle_ns / busy_ns<br/>poll_hits<br/>events_processed]
    end
```

---

## 18. Worker-Klassen (Detail)

```mermaid
flowchart LR
    subgraph LW[LoggingWorker]
        LW1[consume: logging_ring<br/>alle Events]
        LW2[produce: EventLogger<br/>binary+zstd]
        LW3[produce: Text-Log<br/>stdout/file]
        LW4[batch: 100 Events<br/>ostringstream flush]
        LW5[rotieren ab log_max_size_mb<br/>log_keep Dateien]
    end

    subgraph RW[RiskWorker]
        RW1[consume: risk_ring]
        RW2[shadow portfolio + Analytics]
        RW3[RiskManager.check_order/check_post_fill]
        RW4[on halt → halt_flag_]
    end

    subgraph SW[StatsWorker]
        SW1[consume: stats_ring]
        SW2[Analytics aufbauen]
        SW3[alle N Events: snapshot]
    end

    subgraph OW[ObserverWorker — light preset]
        OW1[consume: observer_ring]
        OW2[Events für UI filtern]
        OW3[broadcast via WebSocketWorker]
    end

    subgraph RSW[RiskStatsWorker — standard preset]
        RSW1[kombiniert RiskWorker + StatsWorker]
        RSW2[consume: risk_stats_ring]
    end

    subgraph MMW[MarketMakerWorker — extended]
        MMW1[consume: mm_ring market events]
        MMW2[market_maker_.replenish]
        MMW3[produce: mm_order_ring<br/>Orders an LocalBookAdapter]
    end

    subgraph WSW[WebSocketWorker — HAS_WEB_UI]
        WSW1[Boost.Beast WS-Server<br/>ws_port default 8765]
        WSW2[per-message deflate optional]
        WSW3[broadcast JSON an alle Clients]
        WSW4[Kommandos empfangen<br/>→ engine.pending_symbol/strategy]
        WSW5[Orderbook-Snapshots<br/>alle N ms]
        WSW6[Bar-History max 1000]
        WSW7[Prometheus-Metrik-Endpoint]
    end

    subgraph MM[MarketMaker]
        M1[mt19937 gen_<br/>uniform_real_distribution dis_]
        M2[deque<double> price_history_]
        M3[levels_per_side_, base_depth_,<br/>base_spread_pct_, vol_spread_mult_]
        M4[add_orders init-Seed]
        M5[replenish current_price]
        M6[compute_replenish:<br/>σ aus 50 Bars<br/>N Levels × Seite<br/>px = px ± L × spread+σ·mult]
    end

    MMW --> MM
```

---

## 19. Daten-Layer & Persistenz

```mermaid
flowchart TD
    subgraph IF[IDataSource]
        IFX[load_data data_handler → bool]
        NOTE[chainbar: Decorators erlaubt]
    end

    subgraph DS[Konkrete Sources]
        D1[csv_data_source<br/>OHLCV CSV]
        D2[tick_csv_data_source<br/>timestamp,sym,px,qty,side]
        D3[binary_cache_source<br/>Decorator → binär-Cache]
        D4[pg_data_source HAS_POSTGRESQL<br/>libpqxx]
        D5[websocket_data_source HAS_LIVE_DATA<br/>Boost.Beast Live-Feed]
    end

    IF --> DS

    subgraph DH[data_handler]
        DH1[db_data_date]
        DH2[db_data_symbol]
        DH3[db_data_open/high/low/close/volume]
        DH4[tick_data vector-tick_record]
        DH5[load_from_csv / load_into_queue<br/>add_tick / sort_by_date<br/>has_bar_data / has_tick_data]
        DH6[CopyTracker HAS_DEBUG]
    end

    DS --> DH

    subgraph STORE["SqliteStore (HAS_SQLITE)"]
        SQ1[Tabellen: runs, trades, equity_snapshots, portfolio]
        SQ2[record_run_begin/end]
        SQ3[append trades / equity]
        SQ4[current_run_id_ als PK]
    end

    DH --> ENG[engine]
    STORE --> ENG

    subgraph TR[tick_record]
        TR1[timestamp]
        TR2[symbol]
        TR3[price]
        TR4[quantity]
        TR5[side: bid/ask/unknown]
    end

    TR -.-> DH4
```

---

## 20. WebSocket-UI & Kommandos

```mermaid
flowchart LR
    subgraph ENG2[Engine-Seite]
        ER[ws_ring EventRing]
        WSM[switch_mu_ mutex<br/>pending_symbol_<br/>pending_strategy_]
        BH[bar_history_ max 1000]
        LOBS[last_ob_snapshot_time_]
    end

    subgraph WSW[WebSocketWorker Boost.Beast]
        SRV[Listener ws_port 8765]
        DEFL[per-message deflate negotiation]
        CLNTS[std::set connected clients]
        BRC[broadcast JSON]
    end

    ER --> WSW
    BH --> WSW

    subgraph OUT[Broadcast-Kanäle]
        O1[market/tick events JSON]
        O2[fill events JSON]
        O3[orderbook snapshot<br/>alle 250ms default]
        O4[indicator-Werte per Symbol]
        O5[analytics snapshot]
        O6[status connected/disconnected]
    end

    WSW --> OUT

    subgraph CMD[ws_command vom Client]
        K1[start]
        K2[pause]
        K3[stop]
        K4[order manual]
        K5[set_timeframe]
        K6[set_symbol]
        K7[set_strategy]
    end

    CMD --> WSW
    WSW -->|schreibt| WSM
    WSM -->|Engine pollt| NEXTITER[nächste Iteration<br/>re-wire Strategy/Symbol]

    subgraph FE[Frontend web/ — React 19 + Vite + TS]
        FE1[WebSocketContext]
        FE2[Chart — lightweight-charts]
        FE3[Sidebar / BottomPanel / TopBar]
        FE4[Toast]
        FE5[Stores: Engine/Market/OrderBook/Portfolio/Fill/Analytics]
    end

    WSW <-.-> FE1
```

---

## 21. Checkpoint-Format & Resume

```mermaid
flowchart TD
    WRT[write_checkpoint_if_due<br/>alle N Events] --> HDR[Header schreiben]
    HDR --> H1[Magic 0x43484b50]
    HDR --> H2[Version 1]
    HDR --> H3[event_count u64]
    HDR --> H4[wall_ms u64]
    HDR --> H5[cash f64]
    HDR --> H6[initial_balance f64]
    HDR --> H7[total_trades u64]

    H7 --> POS[Position-Count N u32]
    POS --> LOOP{für jede Position}
    LOOP --> P1[u16 slen]
    LOOP --> P2[symbol bytes]
    LOOP --> P3[qty f64]
    LOOP --> P4[cost_basis f64]

    LOOP --> FILE[(checkpoint.bin)]

    subgraph RES[Resume bei ctor]
        R1[resume_checkpoint_path gesetzt]
        R2[restore_from_checkpoint]
        R3[Magic+Version prüfen]
        R4[portfolio.restore_state<br/>cash, total_trades, positions]
        R5[event_count wiederherstellen]
    end

    FILE -.-> RES
```

---

## 22. Binary Event-Log-Format

```mermaid
flowchart LR
    EL[EventLogger] --> OPEN[open path]
    OPEN --> Z{compress_log?}
    Z -->|ja| ZC[ZSTD_CCtx]
    Z -->|nein| NOC[raw]

    subgraph RECORD[Pro Event]
        REC1[type u8]
        REC2[timestamp ts]
        REC3[per-type felder<br/>u8/u16/u32/u64/i64/f64/str]
    end

    EL --> RECORD
    RECORD --> ZC
    ZC --> FILE[(event_log.bin)]
    NOC --> FILE

    subgraph READER[BufReader — Replay]
        R1[read_u8/u16/u32/u64]
        R2[read_i64]
        R3[read_f64]
        R4[read_str u16-length-prefix]
        R5[read_ts]
    end

    FILE --> ZD{komprimiert?}
    ZD -->|ja| ZDCTX[ZSTD_DCtx]
    ZD -->|nein| RAW
    ZDCTX --> READER
    RAW --> READER

    READER --> DESER[deserialize events]
    DESER --> RUNRPL[engine.run_replay Pipeline]
```

---

## 23. C-API (`libtruetest.so`)

```mermaid
flowchart TD
    subgraph API[src/api/truetest_api.h]
        F1[tt_version → const char*]
        F2[tt_create_engine config_json → handle]
        F3[tt_run handle → int]
        F4[tt_get_results handle → const char* JSON]
        F5[tt_free_string char*]
        F6[tt_destroy handle]
        F7[tt_last_error → const char*]
    end

    API --> CBIND[extern C<br/>visibility default<br/>__declspec dllexport]

    subgraph HOST[Host-Prozesse]
        H1[Python — ctypes/cffi]
        H2[Node.js — ffi-napi]
        H3[Rust — bindgen]
    end

    CBIND --> HOST

    API -->|intern| WRAP[Wrapper um engine<br/>JSON → engine_config<br/>engine.run<br/>results → JSON]
```

---

## 24. Debug-Instrumentierung

```mermaid
flowchart LR
    subgraph DBG[src/debug/ HAS_DEBUG]
        D1[StageTimer<br/>high_resolution_clock<br/>latencies per named stage]
        D2[MemorySampler<br/>malloc-Hooks<br/>Heap-Allocations]
        D3[hardware_info<br/>CPU, cores, L1/L2/L3]
        D4[thread_stats<br/>idle_ns, busy_ns,<br/>poll_attempts/hits,<br/>events_processed]
        D5[ring_stats<br/>push count, drop count,<br/>high_watermark]
        D6[CopyTracker~T~<br/>Template, zählt Kopien]
        D7[debug_log<br/>strukturierte Logs]
        D8[debug_report<br/>Aggregations-Report am Laufende]
    end

    ENG2[engine] -.HAS_DEBUG.-> DBG
    DBG --> REP[Debug-Report am Ende]
```

---

## 25. Speicher-Allokatoren, Queues, Pools — Übersicht

```mermaid
flowchart TB
    subgraph OP[ObjectPool T, BlockSize=4096]
        OP1[free_head: intrusive stack]
        OP2[pop → placement-new<br/>shared_ptr mit custom deleter]
        OP3[push via deleter → Destruktor + Rückgabe]
        OP4[Blöcke wachsen on-demand]
    end

    subgraph INST[Instanzen im Engine]
        I1[market_pool → market_event]
        I2[order_pool → order_event]
        I3[fill_pool → fill_event]
        I4[tick_pool → tick_event]
    end

    OP -.spezialisiert.-> INST

    subgraph RINGQ[Ring-Queues — SPSC]
        RQ1[logging_ring 65536]
        RQ2[risk_ring 65536]
        RQ3[stats_ring 65536]
        RQ4[observer_ring 65536]
        RQ5[risk_stats_ring 65536]
        RQ6[mm_ring 65536]
        RQ7[mm_order_ring 65536]
        RQ8[ws_ring 65536]
    end

    subgraph OTH[Sonstige Container]
        O1[pending_orders_<br/>priority_queue pending_entry<br/>min-heap nach ts,seq]
        O2[pending_stops_<br/>vector<shared_ptr<order_event>>]
        O3[day_order_ids_<br/>vector pair<string,u64>]
        O4[bar_history_<br/>vector<string>, max 1000]
        O5[price_history_<br/>deque double in MarketMaker]
        O6[orderbook node_pool<br/>unique_ptr-node_block<br/>4096 Nodes/Block]
        O7[order_map_<br/>unordered_map order_id→node*]
    end

    subgraph ALLOC[std heap]
        AH1[nlohmann::json nur Config/API<br/>nicht Hot-Path]
        AH2[BufReader/BufWriter-Puffer]
        AH3[ZSTD_CCtx/DCtx]
        AH4[Boost.Beast buffers]
    end

    INST --> RINGQ
    RINGQ --> OTH
    OTH --> ALLOC
```

---

## Legende / Konventionen

- `A -->|label| B` = Datenfluss mit Beschriftung
- `A -.-> B` = schwache / optionale Beziehung (z. B. compile-time gated)
- `[(X)]` = Datei / persistente Ressource
- `[[X]]` = Subsystem-Start
- `((X))` = Ring-Buffer / Queue
- `{X}` = Entscheidung / Zustand
- Alles in `HAS_*`-Klammern existiert nur bei gesetztem CMake-Flag.
