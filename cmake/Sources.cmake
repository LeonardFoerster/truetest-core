# cmake/Sources.cmake
#
# Canonical (still fully explicit) registration of first-party sources.
# This is the single obvious place to add a new core .cpp or its test.
#
# Invariants preserved:
# - 100% explicit lists (no GLOB).
# - ENGINE_CORE_SOURCES feeds the engine_core OBJECT library.
# - TEST_SOURCES (with conditional appends) feeds truetest_tests.
# - Behavior for all ENABLE_* combinations and the three TT_TARGET binaries is unchanged.
#
# Adding a strategy, provider piece, or simulation component:
#   1. Append its .cpp in the appropriate section below (under ENGINE_CORE_SOURCES).
#   2. Append the matching tests/test_*.cpp in the TEST_SOURCES section.
#   (Update docs if the addition changes any documented build surface.)
#
# The LOC regression guard for engine.cpp lives here so it is co-located with
# the source lists that are affected by engine decomposition work.

# ── ENGINE_CORE_SOURCES (identical across the three binaries) ────────────────
# Every source file that is identical across the three binaries lives here.
# main.cpp is intentionally NOT part of engine_core — each executable owns
# its own copy, compiled with a distinct TT_TARGET define.
set(ENGINE_CORE_SOURCES
    # --- Engine core + supporting ---
    src/engine/engine.cpp
    src/engine/execution_router.cpp
    src/engine/order_audit_sink.cpp
    src/engine/instrument_spec_cache.cpp
    src/engine/checkpoint.cpp

    # --- Analytics & reporting ---
    src/analytics/adverse_selection_tracker.cpp
    src/analytics/analytics.cpp
    src/analytics/ascii_widgets.cpp
    src/analytics/report_generator.cpp

    # --- Data sources & loading ---
    src/data/data_loader.cpp
    src/data/csv_data_source.cpp
    src/data/binary_cache_source.cpp
    src/data/tick_csv_data_source.cpp
    src/execution/portfolio.cpp
    src/orderbook/orderbook.cpp

    # --- Strategies ---
    src/strategy/mean_reversion_strategy.cpp
    src/strategy/sma_strategy.cpp
    src/strategy/ma_crossover_strategy.cpp
    src/strategy/hedge_demo_strategy.cpp
    src/strategy/breakout_strategy.cpp
    src/strategy/larry_connor_strategy.cpp
    src/strategy/coiled_spring_strategy.cpp
    src/strategy/sma.cpp
    src/strategy/adaptive_hybrid_config.cpp
    src/strategy/adaptive_hybrid_strategy.cpp
    src/strategy/structure_continuation_strategy.cpp

    # --- Market maker + risk ---
    src/market_maker/market_maker.cpp
    src/risk/risk_manager.cpp
    src/risk/maintenance_margin_table.cpp

    # --- Local + synthetic providers ---
    src/providers/local/local_register.cpp

    # --- Console UI (backtest) ---
    src/ui/console_dashboard.cpp
    src/ui/console_format.cpp

    # --- Exits ---
    src/exits/exit_manager.cpp

    # --- Monte Carlo / synthetic simulation (integrated from monte-carlo branch) ---
    src/simulation/imonte_carlo_generator.cpp
    src/simulation/monte_carlo_controller.cpp
    src/simulation/monte_carlo_reporter.cpp
    src/simulation/generators/gbm_generator.cpp
    src/providers/synthetic/synthetic_transport.cpp
    src/providers/synthetic/synthetic_provider.cpp
    src/providers/synthetic/synthetic_register.cpp
)

# ── TEST_SOURCES (base + conditionals) ───────────────────────────────────────
# Populated when BUILD_TESTS=ON. Conditional appends for optional backends
# are kept here so the "one file to edit for tests" rule is easy to follow.
set(TEST_SOURCES
    # --- Core / helpers ---
    tests/test_main.cpp
    tests/helpers/alloc_counter.cpp

    # --- Basic types & low-level ---
    tests/test_price.cpp
    tests/test_order_id.cpp
    tests/test_order_types.cpp
    tests/test_object_pool.cpp
    tests/test_control_block_pool.cpp
    tests/test_symbol_table.cpp
    tests/test_orderbook_order_pool.cpp
    tests/test_deferred_return_queue.cpp
    tests/test_ring_buffer.cpp

    # --- Data & parsing ---
    tests/test_data_handler.cpp
    tests/test_date_parse.cpp
    tests/test_data_bridge.cpp

    # --- Orderbook & execution model ---
    tests/test_orderbook.cpp
    tests/test_portfolio.cpp
    tests/test_fee_model.cpp
    tests/test_latency_model.cpp
    tests/test_fill_model.cpp
    tests/test_impact_model.cpp
    tests/test_queue_model.cpp
    tests/test_queue_aware_adapter.cpp
    tests/test_queue_position_model.cpp
    tests/test_walked_book_impact.cpp
    tests/test_realistic_fills.cpp
    tests/test_bridge_unknown_fill.cpp

    # --- Events, engine, streaming ---
    tests/test_events.cpp
    tests/test_event_log.cpp
    tests/test_engine.cpp
    tests/test_engine_async_support.cpp
    tests/test_engine_streaming.cpp
    tests/test_engine_integration.cpp
    tests/test_engine_lookahead.cpp
    tests/test_engine_instrument_spec.cpp
    tests/test_instrument.cpp
    tests/test_engine_l2_ingestion.cpp
    tests/test_engine_venue_risk_check.cpp
    tests/test_engine_brackets.cpp
    tests/test_orderbook_registry.cpp
    tests/test_worker_watchdog.cpp

    # --- Risk & safety ---
    tests/test_risk_manager.cpp
    tests/test_futures_risk_check.cpp
    tests/test_live_safety.cpp

    # --- Exits / brackets ---
    tests/test_exit_manager.cpp
    tests/test_bracket_adapter.cpp
    tests/test_binance_oco_bracket_adapter.cpp

    # --- Indicators ---
    tests/test_sma.cpp
    tests/test_ema.cpp
    tests/test_rsi.cpp
    tests/test_stochastic.cpp
    tests/test_swing_detector.cpp
    tests/test_ema_regime.cpp
    tests/test_bollinger.cpp

    # --- Strategies ---
    tests/test_strategies.cpp
    tests/test_adaptive_hybrid.cpp
    tests/test_monte_carlo_generators.cpp
    tests/test_monte_carlo_controller.cpp

    # --- Analytics & reporting ---
    tests/test_adverse_selection.cpp
    tests/test_analytics.cpp
    tests/test_report_generator.cpp
    tests/test_bar_aggregator.cpp

    # --- Market maker ---
    tests/test_market_maker.cpp

    # --- Threading & pools ---
    tests/test_thread_preset.cpp
    tests/test_threading_correctness.cpp
    tests/test_hotpath_allocs.cpp
    tests/test_hotpath_alloc_matrix.cpp
    tests/test_hotpath_pool_prewarm.cpp

    # --- Execution / transport / provider core ---
    tests/test_execution_adapter.cpp
    tests/test_execution_bridge.cpp
    tests/test_provider_event.cpp
    tests/test_provider_event_stream_contract.cpp
    tests/test_provider_transport.cpp
    tests/test_provider_registry.cpp
    tests/test_provider_engine_wiring.cpp
    tests/test_trade_tape_shadow_adapter.cpp
    tests/test_trade_tape_shadow_queue.cpp
    tests/test_hybrid_executor.cpp
    tests/test_cancel_race.cpp

    # --- Order / client id ---
    tests/test_client_order_id.cpp
    tests/test_rate_limiter.cpp
    tests/test_order_audit_sink.cpp

    # --- UI (console / toast / overlays) ---
    tests/test_console_format.cpp
    tests/test_toast.cpp
    tests/test_tui_prefs.cpp
    tests/test_overlays.cpp

    # --- Golden regression ---
    tests/test_golden_regression.cpp

    # --- Binance (always built; many are gated by HAS_BINANCE at runtime) ---
    tests/test_binance_parser.cpp
    tests/test_binance_order_encoder.cpp
    tests/test_binance_rest_order_transport.cpp
    tests/test_binance_user_data_parser.cpp
    tests/test_binance_user_data_transport.cpp
    tests/test_binance_rest_client_time.cpp
    tests/test_binance_rest_client_rate_limit.cpp
    tests/test_binance_rest_client_keepalive_timeout.cpp
    tests/test_binance_user_data_transport_keepalive.cpp
    tests/test_binance_user_data_transport_reconnect.cpp
    tests/test_binance_endpoints.cpp
    tests/test_binance_testnet_live.cpp
    tests/test_binance_combined_parser.cpp
    tests/test_binance_futures_order_encoder.cpp
    tests/test_binance_futures_user_data_parser.cpp
    tests/test_binance_futures_register.cpp
    tests/test_binance_futures_reconciler.cpp
    tests/test_binance_futures_kill_switch.cpp
    tests/test_binance_futures_bracket_adapter.cpp
    tests/test_binance_futures_testnet_live.cpp
    tests/test_binance_futures_safety.cpp
    tests/test_binance_futures_dead_mans_switch.cpp

    # --- CLI integration (separate binary) ---
    # (see truetest_cli_tests in root)
)

if(ENABLE_QUESTDB)
    list(APPEND TEST_SOURCES
        tests/test_questdb_http_client.cpp
        tests/test_questdb_ilp_writer.cpp
        tests/test_questdb_schema.cpp
        tests/test_questdb_run_tag.cpp
        tests/test_questdb_store.cpp
        tests/test_questdb_integration.cpp)
endif()

if(ENABLE_WEB)
    list(APPEND TEST_SOURCES
        tests/test_web_server_auth.cpp)
endif()

# ── LOC regression guard ────────────────────────────────────────────────────
# (introduced during 2026-07-16 engine god-class decomposition)
# Fails configure if src/engine/engine.cpp exceeds limit without explicit waiver comment.
# See engine-decomposition skill for background.
#
# Note: guard is evaluated whenever Sources.cmake is included (i.e. on every
# configure). This is a minor improvement over the previous location (only
# under BUILD_TESTS) while preserving the core failure behavior and limit.
file(STRINGS src/engine/engine.cpp _engine_lines)
list(LENGTH _engine_lines _engine_loc)
set(ENGINE_LOC_MAX 4300)  # current ~4272 + buffer; tighten after further cleanup
if(_engine_loc GREATER ENGINE_LOC_MAX)
    file(STRINGS src/engine/engine.cpp _waiver_lines REGEX "ENGINE_LOC_WAIVER:")
    if(NOT _waiver_lines)
        message(FATAL_ERROR
            "engine.cpp has ${_engine_loc} lines (limit ${ENGINE_LOC_MAX}). "
            "Add a comment containing 'ENGINE_LOC_WAIVER: <reason>' or reduce LOC. "
            "The limit and guard were introduced as part of the 2026-07-16 engine decomposition (see engine-decomposition skill).")
    endif()
endif()
