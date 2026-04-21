# TrueTest Engine — Complete Program Flowchart

## 1. Top-Level Entry Flow

```mermaid
flowchart TD
    START([Program Start]) --> DEBUG_INIT{HAS_DEBUG?}
    DEBUG_INIT -->|Yes| ABSL[Parse absl flags<br/>Init debug logging<br/>File sink] --> CLI_PARSE
    DEBUG_INIT -->|No| CLI_PARSE

    CLI_PARSE[Parse CLI flags<br/>--replay, --provider, --strategy,<br/>--seed, --thread-preset, --web-ui,<br/>--symbol, --stream, --live, etc.]

    CLI_PARSE --> MODE_CHECK{Which entry mode?}

    MODE_CHECK -->|--replay set| REPLAY_MODE
    MODE_CHECK -->|--provider set| PROVIDER_MODE
    MODE_CHECK -->|neither| TUI_MODE
```

## 2. Replay Mode

```mermaid
flowchart TD
    REPLAY_MODE([Replay Mode]) --> RM_STRAT[Create default<br/>Mean Reversion strategy]
    RM_STRAT --> RM_CFG[Build engine_config<br/>seed, event_log_path]
    RM_CFG --> RM_DH[Create empty data_handler]
    RM_DH --> RM_ENG[Construct engine<br/>dh, nullptr, strategy, cfg]
    RM_ENG --> RM_RUN["engine::run_replay(path)<br/>Deserialize binary event log<br/>Re-process events through pipeline"]
    RM_RUN --> RM_SUMMARY[engine::print_summary]
    RM_SUMMARY --> EXIT([return 0])
```

## 3. Provider Mode (CLI)

```mermaid
flowchart TD
    PROV_MODE([Provider Mode]) --> PROV_CFG[Build provider_config map<br/>path, symbol, stream, api_key, etc.]
    PROV_CFG --> PROV_CREATE["ProviderRegistry::create(name, config)<br/>Factory lookup via REGISTER_PROVIDER macro"]

    PROV_CREATE --> STRAT_SELECT{--strategy flag?}
    STRAT_SELECT -->|sma| SMA[sma_strategy]
    STRAT_SELECT -->|ma-crossover| MAC[ma_crossover_strategy]
    STRAT_SELECT -->|default| MR[mean_reversion_strategy<br/>with balance, risk, SL/TP]

    SMA & MAC & MR --> ENG_CFG[Build engine_config<br/>seed, threading, fees,<br/>provider, web-ui, mode]

    ENG_CFG --> THREAD_DETECT{--thread-preset?}
    THREAD_DETECT -->|explicit| THREAD_STR[string_to_preset]
    THREAD_DETECT -->|auto| THREAD_AUTO["select_preset(<br/>detect_physical_cores())"]
    THREAD_STR & THREAD_AUTO --> FEE_MODEL

    FEE_MODEL{--fee flag?}
    FEE_MODEL -->|fixed| FIXED_FEE[FixedFeeModel]
    FEE_MODEL -->|tiered| TIERED_FEE[TieredFeeModel]
    FEE_MODEL -->|binance default| BIN_FEE["TieredFeeModel(0.001, 0.001)"]
    FEE_MODEL -->|none| NO_FEE[nullptr = zero fees]

    FIXED_FEE & TIERED_FEE & BIN_FEE & NO_FEE --> MODE_SELECT

    MODE_SELECT{engine_mode?}
    MODE_SELECT -->|shadow| SHADOW[engine_mode::shadow]
    MODE_SELECT -->|live| LIVE_CHECK
    MODE_SELECT -->|default| BACKTEST[engine_mode::backtest]

    LIVE_CHECK{Live safety checks}
    LIVE_CHECK -->|missing --live flag| ABORT1([Exit 1])
    LIVE_CHECK -->|missing API keys| ABORT2([Exit 1])
    LIVE_CHECK -->|user types YES| LIVE[engine_mode::live]
    LIVE_CHECK -->|user declines| ABORT3([Exit 0])

    SHADOW & BACKTEST & LIVE --> PROV_OPEN
    PROV_OPEN{"provider->open()<br/>(connects WebSocket, etc.)"}
    PROV_OPEN -->|fail| ABORT4([Exit 1])
    PROV_OPEN -->|ok| GET_TRANSPORT

    GET_TRANSPORT["Get transport from provider<br/>fallback: FileTransport"]

    GET_TRANSPORT --> RECORD_CHECK{HAS_BINANCE?}
    RECORD_CHECK -->|--record set| WRAP_RECORD[Wrap in RecordingTransport]
    RECORD_CHECK -->|--replay-data set| WRAP_REPLAY[Override with ReplayTransport]
    RECORD_CHECK -->|neither| DETERMINE_FORMAT

    WRAP_RECORD & WRAP_REPLAY --> DETERMINE_FORMAT

    DETERMINE_FORMAT[Determine format:<br/>tick vs bar<br/>streaming vs batch]

    DETERMINE_FORMAT --> IS_STREAMING{is_streaming?}
    IS_STREAMING -->|yes| STREAMING_PATH
    IS_STREAMING -->|no| BATCH_PATH
```

## 4. Streaming Path (Provider Mode)

```mermaid
flowchart TD
    STREAM([Streaming Path]) --> S_ENG["Construct engine(dh, nullptr,<br/>strategy, config)"]
    S_ENG --> S_TICK{is_tick?}

    S_TICK -->|yes| S_TICK_PARSER{Provider type?}
    S_TICK_PARSER -->|binance| S_BTP[BinanceTradeParser]
    S_TICK_PARSER -->|other| S_CTP[CsvTickParser]
    S_BTP & S_CTP --> S_TICK_BRIDGE["DataBridge&lt;tick_record&gt;<br/>(transport, parser, tick_record_sink)"]
    S_TICK_BRIDGE --> S_TICK_RUN["engine::run_streaming(bridge)<br/>↓ see Streaming Engine Loop"]

    S_TICK -->|no| S_BAR_PARSER{Provider type?}
    S_BAR_PARSER -->|binance| S_BKP[BinanceKlineParser]
    S_BAR_PARSER -->|other| S_CBP[CsvBarParser]
    S_BKP & S_CBP --> S_BAR_BRIDGE["DataBridge&lt;bar_record&gt;<br/>(transport, parser, bar_record_sink)"]
    S_BAR_BRIDGE --> S_BAR_RUN["engine::run_streaming(bridge)<br/>↓ see Streaming Engine Loop"]

    S_TICK_RUN & S_BAR_RUN --> S_SUMMARY[engine::print_summary]
    S_SUMMARY --> S_EXIT([return 0])
```

## 5. Batch Path (Provider Mode)

```mermaid
flowchart TD
    BATCH([Batch Path]) --> B_TICK{is_tick?}

    B_TICK -->|yes| B_TICK_BRIDGE["DataBridge&lt;tick_record&gt;"]
    B_TICK_BRIDGE --> B_TICK_LOAD{"bridge->load_data(dh)"}
    B_TICK_LOAD -->|fail| B_ABORT([Exit 1])
    B_TICK_LOAD -->|ok| B_TICK_ENG

    B_TICK -->|no| B_BAR_BRIDGE["DataBridge&lt;bar_record&gt;"]
    B_BAR_BRIDGE --> B_BAR_LOAD{"bridge->load_data(dh)"}
    B_BAR_LOAD -->|fail| B_ABORT2([Exit 1])
    B_BAR_LOAD -->|ok| B_BAR_ENG

    B_TICK_ENG["Construct engine"] --> B_TICK_RUN["engine::run_tick_data()"]
    B_BAR_ENG["Construct engine"] --> B_BAR_RUN["engine::run()"]

    B_TICK_RUN & B_BAR_RUN --> B_SUMMARY[engine::print_summary]
    B_SUMMARY --> B_EXIT([return 0])
```

## 6. TUI Mode (Interactive)

```mermaid
flowchart TD
    TUI([TUI Mode]) --> BANNER["Print TrueTest ASCII banner"]
    BANNER --> T_STRAT{Strategy selection menu}
    T_STRAT -->|1| T_MR[Mean Reversion]
    T_STRAT -->|2| T_SMA[SMA Strategy]
    T_STRAT -->|3| T_MAC[MA Crossover]
    T_STRAT -->|invalid| T_MR

    T_MR & T_SMA & T_MAC --> T_PERIOD[Prompt SMA period]

    T_PERIOD --> T_DATA{Data source menu}
    T_DATA -->|1 + HAS_POSTGRESQL| T_PG["PgDataSource<br/>→ BinaryCacheSource decorator"]
    T_DATA -->|2| T_CSV[CsvDataSource<br/>prompt path]
    T_DATA -->|3| T_TICK_CSV[TickCsvDataSource<br/>prompt path]
    T_DATA -->|invalid| T_ABORT([Exit 1])

    T_PG & T_CSV & T_TICK_CSV --> T_FEE{Fee model menu}
    T_FEE -->|1| T_ZERO[Zero fees]
    T_FEE -->|2| T_FIXED[FixedFeeModel<br/>prompt amount]
    T_FEE -->|3| T_TIERED[TieredFeeModel<br/>prompt maker/taker rates]

    T_ZERO & T_FIXED & T_TIERED --> T_MODE{Engine mode menu}
    T_MODE -->|1| T_BT[backtest]
    T_MODE -->|2| T_SH[shadow]
    T_MODE -->|3| T_LV[live]

    T_BT & T_SH & T_LV --> T_THREAD[Auto-detect thread preset]
    T_THREAD --> T_LOAD["source->load_data(dh)"]
    T_LOAD -->|fail| T_LOAD_FAIL([Exit: Failed to load data])
    T_LOAD -->|ok| T_ENG["Construct engine(dh, nullptr, strategy, config)"]
    T_ENG --> T_RUN["engine::run()"]
    T_RUN --> T_SUMMARY[engine::print_summary]
    T_SUMMARY --> T_EXIT([return 0])
```

## 7. Engine Constructor

```mermaid
flowchart TD
    CTOR(["engine::engine(dh, ob, strategy, config)"]) --> INIT["Initialize members:<br/>portfolio_(initial_balance)<br/>risk_manager_(config.risk)<br/>market_maker_(seed)"]
    INIT --> OB_CHECK{orderbook provided?}
    OB_CHECK -->|yes| OB_REG[Register in OrderbookRegistry]
    OB_CHECK -->|no| SHADOW_CHECK
    OB_REG --> SHADOW_CHECK
    SHADOW_CHECK{mode == shadow?}
    SHADOW_CHECK -->|yes| SHADOW_CREATE["Create ShadowTracker<br/>(simulated vs exchange comparison)"]
    SHADOW_CHECK -->|no| CTOR_DONE([Constructor done])
    SHADOW_CREATE --> CTOR_DONE
```

## 8. engine::run() — Batch Bar Loop

```mermaid
flowchart TD
    RUN(["engine::run()"]) --> RUN_CHECK{has_tick_data?}
    RUN_CHECK -->|yes| DISPATCH_TICK["run_tick_data() — see below"]
    RUN_CHECK -->|no| BAR_CHECK{has_bar_data?}
    BAR_CHECK -->|no| RUN_ERROR([throw: no data loaded])
    BAR_CHECK -->|yes| INIT_LOG{event_log_path set?}

    INIT_LOG -->|yes| CREATE_LOGGER[Create EventLogger]
    INIT_LOG -->|no| DEBUG_MEM
    CREATE_LOGGER --> DEBUG_MEM

    DEBUG_MEM["DEBUG: capture memory baseline"] --> START_WORKERS["start_workers()<br/>↓ see Worker Startup"]

    START_WORKERS --> WS_STATUS["WEB_UI: broadcast 'running' status"]
    WS_STATUS --> BAR_LOOP

    BAR_LOOP["FOR each bar i = 0..N"]
    BAR_LOOP --> HALT_CHK{halt_requested OR<br/>halt_flag_ OR<br/>worker_failed_?}
    HALT_CHK -->|yes| LOOP_END
    HALT_CHK -->|no| CREATE_MKT["Create market_event from<br/>data_handler columns:<br/>symbol, OHLCV, timestamp"]

    CREATE_MKT --> DRAIN_PENDING["Drain pending orders<br/>(priority queue by eligible time)"]
    DRAIN_PENDING --> UPDATE_MID["last_mid_price_ = close"]

    UPDATE_MID --> CHECK_STOPS["Check pending stop orders<br/>Buy stops: high >= stop_price<br/>Sell stops: low <= stop_price<br/>→ Convert to market/limit order"]

    CHECK_STOPS --> REPLENISH["MarketMaker::replenish(orderbook, mid_price)<br/>(skip if MM worker active)"]

    REPLENISH --> PROCESS_MKT["Publish market_event<br/>to rings + analytics"]

    PROCESS_MKT --> STRAT_CALL["strategy->on_market(mkt)<br/>→ optional order_event"]

    STRAT_CALL --> HAS_ORDER{order returned?}
    HAS_ORDER -->|no| MM_ORDERS
    HAS_ORDER -->|yes| ORDER_TYPE{order type?}

    ORDER_TYPE -->|stop / stop_limit| ADD_STOP["Add to pending_stops"]
    ORDER_TYPE -->|market / limit<br/>with latency| QUEUE_PENDING["Push to pending_orders<br/>(min-heap by eligible_ts)"]
    ORDER_TYPE -->|market / limit<br/>no latency| PROCESS_NOW["process_order()<br/>↓ see Order Pipeline"]

    ADD_STOP & QUEUE_PENDING & PROCESS_NOW --> MM_ORDERS

    MM_ORDERS{extended preset?}
    MM_ORDERS -->|yes| MM_DRAIN["Drain mm_order_ring_<br/>Process MM replenish orders"]
    MM_ORDERS -->|no| DAY_CHECK

    MM_DRAIN --> DAY_CHECK
    DAY_CHECK["Check day orders for<br/>end-of-session cancellation"]

    DAY_CHECK --> WS_CMDS["WEB_UI: process_ws_commands<br/>broadcast_orderbook_snapshot<br/>send_state_snapshot"]

    WS_CMDS --> PROGRESS["Print progress every 200ms"]
    PROGRESS --> BAR_LOOP

    LOOP_END["Flush pending orders queue<br/>Cancel open day orders"] --> FLUSH_LOG["Flush event logger"]
    FLUSH_LOG --> DEBUG_REPORT["DEBUG: print stage timer,<br/>memory, ring diagnostics"]
    DEBUG_REPORT --> STOP_WORKERS["stop_workers()<br/>↓ see Worker Shutdown"]
```

## 9. engine::run_tick_data() — Batch Tick Loop

```mermaid
flowchart TD
    TICK_RUN(["engine::run_tick_data()"]) --> TICK_LOG{event_log_path set?}
    TICK_LOG -->|yes| TICK_LOGGER[Create EventLogger]
    TICK_LOG -->|no| TICK_START
    TICK_LOGGER --> TICK_START

    TICK_START["start_workers()"] --> TICK_LOOP

    TICK_LOOP["FOR each tick i = 0..N"]
    TICK_LOOP --> TICK_HALT{halt / worker_failed?}
    TICK_HALT -->|yes| TICK_END
    TICK_HALT -->|no| TICK_PROCESS["process_single_tick(rec)"]

    TICK_PROCESS --> TICK_MKT["Create tick_event<br/>Update last_mid_price_"]
    TICK_MKT --> TICK_OB["Replenish orderbook<br/>(always inline for ticks)"]
    TICK_OB --> TICK_PUB["Publish tick_event to rings"]
    TICK_PUB --> TICK_AGG["WEB_UI: feed tick to<br/>BarAggregator for charting"]
    TICK_AGG --> TICK_STRAT["strategy->on_tick(te)<br/>→ optional order"]
    TICK_STRAT --> TICK_ORDER{order?}
    TICK_ORDER -->|yes| TICK_SUBMIT["Submit to adapter<br/>Poll fills → portfolio update"]
    TICK_ORDER -->|no| TICK_NEXT
    TICK_SUBMIT --> TICK_NEXT[Progress report]
    TICK_NEXT --> TICK_LOOP

    TICK_END["Flush logger"] --> TICK_STOP["stop_workers()"]
```

## 10. Streaming Engine Loop

```mermaid
flowchart TD
    STREAM_RUN(["engine::run_streaming(bridge)"]) --> STREAM_LOG{event_log_path?}
    STREAM_LOG -->|yes| STREAM_LOGGER[Create EventLogger]
    STREAM_LOG -->|no| STREAM_AGG
    STREAM_LOGGER --> STREAM_AGG

    STREAM_AGG["WEB_UI + tick: Create<br/>BarAggregator(interval, callback)"]
    STREAM_AGG --> STREAM_START["start_workers()"]

    STREAM_START --> STREAM_BRIDGE["bridge->run_streaming(dh, callback)<br/>Blocks until transport closes"]

    STREAM_BRIDGE --> STREAM_CB["Per-record callback:"]
    STREAM_CB --> STREAM_TYPE{bar or tick?}
    STREAM_TYPE -->|bar| STREAM_BAR["process_single_bar(rec)"]
    STREAM_TYPE -->|tick| STREAM_TICK["process_single_tick(rec)"]

    STREAM_BAR & STREAM_TICK --> STREAM_WS["WEB_UI: broadcast orderbook<br/>process commands, state snapshots"]
    STREAM_WS --> STREAM_REPORT["Progress every 200ms"]
    STREAM_REPORT -->|next record| STREAM_CB

    STREAM_BRIDGE --> STREAM_DONE["Transport closed / disconnected"]
    STREAM_DONE --> STREAM_FLUSH["Flush tick aggregator<br/>Flush event logger"]
    STREAM_FLUSH --> STREAM_STOP["stop_workers()"]
```

## 11. Order Processing Pipeline (process_order)

```mermaid
flowchart TD
    PO(["process_order(order, event_count, halt)"]) --> RISK_PRE{inline mode?}
    RISK_PRE -->|yes| RISK_CHECK["risk_manager_.check_order<br/>(order, portfolio, analytics snapshot)"]
    RISK_PRE -->|no| LOG_ORDER["(threaded: RiskWorker handles this)"]

    RISK_CHECK --> RISK_RESULT{risk_action?}
    RISK_RESULT -->|halt| PO_HALT([halt_requested = true<br/>return false])
    RISK_RESULT -->|reject| PO_REJECT([drop order, return true])
    RISK_RESULT -->|allow| LOG_ORDER

    LOG_ORDER["log_event(order)<br/>publish_event → ring buffers<br/>inline: analytics_.on_event"]

    LOG_ORDER --> GET_ADAPTER["get_adapter(symbol)<br/>Live mode: provider executor<br/>Else: LocalBookAdapter"]

    GET_ADAPTER --> SET_MID["Set mid_price on adapter"]
    SET_MID --> SUBMIT["adapter->submit_order(order)"]

    SUBMIT --> SHADOW_FWD{shadow mode?}
    SHADOW_FWD -->|yes| SHADOW_SUBMIT["Also submit to<br/>provider's exchange adapter"]
    SHADOW_FWD -->|no| POLL_FILLS

    SHADOW_SUBMIT --> POLL_FILLS["adapter->poll_fills(fills)"]

    POLL_FILLS --> HAS_FILLS{fills?}
    HAS_FILLS -->|no| SHADOW_POLL
    HAS_FILLS -->|yes| FILL_LOOP["FOR each fill:"]

    FILL_LOOP --> FILL_PROCESS["log_event(fill)<br/>portfolio_.on_fill(fill)<br/>strategy->set_position_open<br/>publish_event → rings<br/>inline: analytics_.on_event"]

    FILL_PROCESS --> SHADOW_TRACK{shadow mode?}
    SHADOW_TRACK -->|yes| SHADOW_SIM["shadow_tracker_->on_simulated_fill"]
    SHADOW_TRACK -->|no| POST_RISK

    SHADOW_SIM --> POST_RISK{inline mode?}
    POST_RISK -->|yes| POST_CHECK["risk_manager_.check_post_fill<br/>→ halt if triggered"]
    POST_RISK -->|no| NEXT_FILL
    POST_CHECK --> NEXT_FILL[Next fill]
    NEXT_FILL --> FILL_LOOP

    FILL_LOOP --> SHADOW_POLL{shadow mode?}
    SHADOW_POLL -->|yes| EXCHANGE_FILLS["Poll exchange adapter fills<br/>shadow_tracker_->on_exchange_fill"]
    SHADOW_POLL -->|no| PO_DONE([return true])
    EXCHANGE_FILLS --> PO_DONE
```

## 12. Execution Adapter Resolution

```mermaid
flowchart TD
    GA(["get_adapter(symbol)"]) --> CACHE{adapter cached<br/>for symbol?}
    CACHE -->|yes| RETURN_CACHED([Return cached adapter])
    CACHE -->|no| GET_OB["orderbook_registry_<br/>.get_or_create(symbol)"]

    GET_OB --> LIVE_CHECK{live mode AND<br/>provider has executor?}
    LIVE_CHECK -->|yes| EXCHANGE_ADAPTER["Use provider's<br/>IExecutionAdapter<br/>(e.g. Binance REST)"]
    LIVE_CHECK -->|no| LOCAL_ADAPTER["Create LocalBookAdapter<br/>(orderbook, fee_model,<br/>fill_model, seed)"]

    EXCHANGE_ADAPTER & LOCAL_ADAPTER --> CACHE_IT["Cache in execution_adapters_ map"]
    CACHE_IT --> RETURN_NEW([Return adapter])
```

## 13. Worker Thread Startup

```mermaid
flowchart TD
    SW(["start_workers()"]) --> RESET["halt_flag_ = false<br/>worker_failed_ = false"]

    RESET --> WS_CHECK{WEB_UI enabled?}
    WS_CHECK -->|yes| WS_START["Create ws_ring_<br/>Create WebSocketWorker(port)<br/>Launch thread: ws_worker_->run()"]
    WS_CHECK -->|no| THREADED_CHECK

    WS_START --> THREADED_CHECK{is_threaded?}
    THREADED_CHECK -->|no| SW_DONE([return])
    THREADED_CHECK -->|yes| CORE_MAP["build_core_map()<br/>Detect physical cores"]

    CORE_MAP --> PRESET{threading preset?}

    PRESET -->|inline| SW_DONE

    PRESET -->|light<br/>3 cores| LIGHT["ObserverWorker<br/>(combined log+risk+stats)<br/>1 ring, 1 thread"]

    PRESET -->|standard<br/>4-5 cores| STANDARD["LoggingWorker + RiskStatsWorker<br/>(combined risk+stats)<br/>2 rings, 2 threads"]

    PRESET -->|full<br/>6-7 cores| FULL["LoggingWorker + RiskWorker<br/>+ StatsWorker<br/>3 rings, 3 threads"]

    PRESET -->|extended<br/>8+ cores| EXTENDED["LoggingWorker + RiskWorker<br/>+ StatsWorker + MarketMakerWorker<br/>4 rings + mm_order_ring, 4 threads"]

    LIGHT & STANDARD & FULL & EXTENDED --> PIN["pin_to_core(thread, core_id)<br/>via sched_setaffinity"]
    PIN --> SW_DONE
```

## 14. Worker Thread Shutdown

```mermaid
flowchart TD
    STOP(["stop_workers()"]) --> SIGNAL["Signal all active workers:<br/>observer, logging, risk,<br/>stats, risk_stats, mm, ws<br/>→ worker->stop()"]

    SIGNAL --> JOIN["Join all worker_threads_<br/>Clear thread vector"]

    JOIN --> DRAIN_MM["Drain mm_order_ring_<br/>(avoid use-after-free)"]

    DRAIN_MM --> CHECK_FAIL["Check each worker for exceptions<br/>Print failure messages"]

    CHECK_FAIL --> DROP_REPORT{total ring drops > 0?}
    DROP_REPORT -->|yes| PRINT_DROPS["Print drop counts per ring:<br/>logging, risk, stats,<br/>observer, risk_stats, mm, ws"]
    DROP_REPORT -->|no| STOP_DONE([done])
    PRINT_DROPS --> STOP_DONE
```

## 15. Event Publishing (Hot Path)

```mermaid
flowchart TD
    PUB(["publish_event(ev)"]) --> PRESET{threading preset?}

    PRESET -->|inline| PUB_DONE([no-op])

    PRESET -->|light| L_OBS["observer_ring_->try_push(ev)<br/>drop counter on failure"]

    PRESET -->|standard| S_LOG["logging_ring_->try_push(ev)"]
    S_LOG --> S_RS["risk_stats_ring_->try_push(ev)"]

    PRESET -->|full| F_LOG["logging_ring_->try_push(ev)"]
    F_LOG --> F_RISK["risk_ring_->try_push(ev)"]
    F_RISK --> F_STATS["stats_ring_->try_push(ev)"]

    PRESET -->|extended| E_LOG["logging_ring_->try_push(ev)"]
    E_LOG --> E_RISK["risk_ring_->try_push(ev)"]
    E_RISK --> E_STATS["stats_ring_->try_push(ev)"]
    E_STATS --> E_MM["mm_ring_->try_push(ev)"]

    L_OBS & S_RS & F_STATS & E_MM --> WS_PUB

    WS_PUB{WEB_UI?}
    WS_PUB -->|yes| WS_PUSH["ws_ring_->try_push(ev)"]
    WS_PUB -->|no| PUB_DONE
    WS_PUSH --> PUB_DONE
```

## 16. Print Summary & Exit

```mermaid
flowchart TD
    SUM(["engine::print_summary()"]) --> SUM_PRESET{threading preset?}

    SUM_PRESET -->|inline| SUM_INLINE["analytics_.print_report()<br/>Sharpe, Sortino, max drawdown,<br/>win rate, total PnL"]
    SUM_PRESET -->|light| SUM_LIGHT["observer_worker_->analytics()<br/>.print_report()"]
    SUM_PRESET -->|standard| SUM_STD["risk_stats_worker_->analytics()<br/>.print_report()"]
    SUM_PRESET -->|full / extended| SUM_FULL["stats_worker_->analytics()<br/>.print_report()"]

    SUM_INLINE & SUM_LIGHT & SUM_STD & SUM_FULL --> SHADOW{shadow_tracker_?}
    SHADOW -->|yes| SHADOW_RPT["shadow_tracker_->print_report()<br/>Simulated vs exchange fill comparison"]
    SHADOW -->|no| SUM_DONE([return 0 — program exits])
    SHADOW_RPT --> SUM_DONE
```

## 17. Complete Event Pipeline (Single Bar)

```mermaid
flowchart LR
    subgraph Data
        BAR["bar_record<br/>(OHLCV + symbol)"]
    end

    subgraph Engine Core 0
        MKT["market_event"]
        OB["Orderbook<br/>replenish"]
        STRAT["IStrategy<br/>::on_market()"]
        ORD["order_event"]
        ADAPTER["IExecutionAdapter<br/>::submit_order()"]
        FILL["fill_event"]
        PORT["Portfolio<br/>::on_fill()"]
    end

    subgraph "Worker Threads (SPSC Rings)"
        LOG["LoggingWorker<br/>binary + text log"]
        RISK["RiskWorker<br/>shadow portfolio<br/>halt_flag_"]
        STATS["StatsWorker<br/>Welford online algo<br/>Sharpe/Sortino"]
        MM["MarketMakerWorker<br/>replenish orders"]
        WS["WebSocketWorker<br/>JSON broadcast"]
    end

    subgraph Browser
        UI["web/index.html<br/>equity curve<br/>orderbook depth<br/>fills table"]
    end

    BAR --> MKT
    MKT --> OB
    OB --> STRAT
    STRAT --> ORD
    ORD --> ADAPTER
    ADAPTER --> FILL
    FILL --> PORT

    MKT -.->|publish| LOG & RISK & STATS & MM & WS
    ORD -.->|publish| LOG & RISK & STATS & MM & WS
    FILL -.->|publish| LOG & RISK & STATS & MM & WS

    MM -.->|mm_order_ring_| Engine Core 0
    WS -->|WebSocket JSON| UI
    RISK -.->|halt_flag_| Engine Core 0
```
