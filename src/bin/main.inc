#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <map>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "engine/live_safety_session.h"
#include "engine/deterministic_lifecycle_sink.h"
#include "engine/deterministic_order_id_scope.h"
#include "core/event_log.h"
#include "core/tt_target.h"
#include "reproducibility/deterministic_seed.h"
#include "reproducibility/run_manifest.h"
#include "reproducibility/sha256.h"
#include "data/data_source.h"
#include "data/csv_data_source.h"
#include "data/tick_csv_data_source.h"
#include "data/data_wrapper.h"
#include "data/market_series.h"
#include "execution/fee_model.h"
#include "execution/latency_model.h"
#include "orderbook/fill_model.h"
#include "execution/queue_position_model.h"
#include "execution/queue_model.h"
#include "execution/impact_model.h"
#include "orderbook/orderbook.h"
#include "strategy/strategy_registry.h"
#include "strategy/apply_execution_cost_params.h"
#include "strategy/deterministic_parameter_config.h"
#include "strategy/mean_reversion/mean_reversion_strategy.h"
#include "strategy/sma/sma_strategy.h"
#include "strategy/ma_crossover/ma_crossover_strategy.h"
#include "market_maker/market_maker.h"
#include "threading/thread_config.h"
#include "utils/log/logger.h"
#include <tt/truetest_version.h>

#include "presets/preset.h"

#include "providers/provider_registry.h"
#include "bin/live_operator_challenge.h"
#include "bin/provider_open_policy.h"
#include "providers/data_bridge.h"
#include "providers/provider_sink.h"
#include "providers/local/csv_parser.h"
#include "providers/local/file_transport.h"

// Monte Carlo support (Phase 2+)
#include "simulation/monte_carlo_controller.h"
#include "simulation/monte_carlo_reporter.h"
#include "simulation/deterministic_mc_manifest.h"
#include "simulation/deterministic_trial_artifacts.h"

#ifdef HAS_QUESTDB
#include "data/questdb/store.h"
#include "data/questdb/run_tag.h"
#endif
#include "ui/console_dashboard.h"
#include "ui/operator_actions.h"
#if defined(HAS_RICH_TUI) || defined(HAS_IMGUI_DESK) || defined(HAS_WEB)
#include "ui/dashboard_snapshot.h"
#endif
#ifdef HAS_RICH_TUI
#include "ui/tabbed_dashboard.h"
#endif
#ifdef HAS_IMGUI_DESK
#include "ui/desk/desk_app.h"
#endif

#ifdef HAS_BINANCE
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_combined_parser.h"
#include "providers/binance/binance_recorder.h"
#include "providers/binance/binance_replay_transport.h"
#endif

#ifdef HAS_IMGUI_DESK
#include "providers/footprint/footprint_venue_capabilities.h"
#include "ui/desk/footprint_live_source.h"
#endif

#ifdef HAS_DEBUG
#include "debug/debug_log.h"
#endif

#ifdef HAS_WEB
#include "ui/dashboard_snapshot.h"
#include "web/web_server.h"
#include "web/report_json.h"
#endif

#ifdef HAS_WEB
// Set by SIGINT/SIGTERM so a post-backtest web session can serve the final
// report until the operator exits.
static std::atomic<bool> g_web_stop{false};
#endif
static void install_shutdown_handler()
{
    truetest::bin::reset_shutdown_signal();
    std::signal(SIGINT, truetest::bin::mark_shutdown_signal);
    std::signal(SIGTERM, truetest::bin::mark_shutdown_signal);
}

// Renders a multi-line breakdown of threading: preset, OS-thread count,
// physical/logical core counts, per-role core pins, and spin policy. Used
// by the dry-run and run banners so the operator sees the same layout
// everywhere and can tell at a glance that "3 workers" ≠ "3 threads".
static void print_threading_details(std::ostream& os,
                                    const std::string& indent,
                                    thread_preset preset,
                                    spin_policy spin)
{
    const bool tty = ::isatty(fileno(stdout)) != 0;
    const char* CYN = tty ? "\033[36m" : "";
    const char* DIM = tty ? "\033[2m"  : "";
    const char* BLD = tty ? "\033[1m"  : "";
    const char* OFF = tty ? "\033[0m"  : "";

    const std::size_t phys = detect_physical_cores();
    const unsigned    hw   = std::thread::hardware_concurrency();
    const int         workers = preset_worker_count(preset);
    const int         threads = workers + 1;  // +1 = event_loop (main thread)

    os << indent << "Threading:  " << BLD << preset_to_string(preset) << OFF
       << " preset - " << BLD << threads << OFF << " thread"
       << (threads == 1 ? "" : "s")
       << " on " << phys << " physical core" << (phys == 1 ? "" : "s");
    if (hw > phys)
        os << " " << DIM << "(" << hw << " logical w/ SMT - siblings left idle)" << OFF;
    os << "\n";

    auto cmap = build_core_map();
    auto core_of = [&](core_role r) -> int {
        for (const auto& a : cmap) if (a.role == r) return a.core_id;
        return -1;
    };
    auto print_role = [&](const char* label, core_role r) {
        int c = core_of(r);
        os << indent << "  " << CYN << label << OFF << " ";
        if (c < 0) os << DIM << "→ (unpinned)" << OFF;
        else       os << "→ core " << c;
        os << "\n";
    };

    print_role("event_loop ", core_role::event_loop);
    switch (preset) {
    case thread_preset::inline_mode:
        break;
    case thread_preset::logging_only:
        print_role("logging    ", core_role::logging);
        break;
    case thread_preset::light:
        print_role("observer   ", core_role::logging);
        break;
    case thread_preset::standard:
        print_role("logging    ", core_role::logging);
        print_role("risk+stats ", core_role::risk);
        break;
    case thread_preset::full:
        print_role("logging    ", core_role::logging);
        print_role("risk       ", core_role::risk);
        print_role("stats      ", core_role::stats);
        break;
    case thread_preset::extended:
        print_role("logging    ", core_role::logging);
        print_role("risk       ", core_role::risk);
        print_role("stats      ", core_role::stats);
        print_role("market_make", core_role::market_maker);
        break;
    }
    os << indent << "  " << DIM << "spin: " << spin_policy_to_string(spin)
       << OFF << "\n";
}

struct cli_options
{
    std::string replay_path;
    int64_t replay_from_us = 0;
    int64_t replay_to_us = INT64_MAX;
    bool replay_from_explicitly_set = false;
    bool replay_to_explicitly_set = false;
    std::string event_log_path;
    std::string log_file_path;
    std::uint64_t log_max_size_mb = 0;
    int log_keep = 5;
    bool compress_log = true;
    uint64_t seed = 0;
    bool seed_explicitly_set = false;
    std::string write_run_manifest_path;
    std::string replay_run_manifest_path;
    std::string reproducibility_artifacts_directory;
    bool verify_hashes = false;
    bool allow_dataset_mismatch = false;
    std::size_t deterministic_trial_index = 0;
    bool deterministic_trial_index_explicitly_set = false;
    bool deterministic_replay_input_override_requested = false;
    bool deterministic_local_replay = false;
    std::string deterministic_run_fingerprint;
    std::string deterministic_expected_dataset_identity;
    std::vector<std::string> deterministic_dataset_mismatches;
    std::size_t deterministic_lifecycle_record_capacity = 0;
    std::map<std::string, std::string, std::less<>>
        deterministic_expected_hashes;
    std::string thread_preset_str;
    std::string spin_policy_str;
    bool no_pin = false;

    // Named preset / profile (see presets/preset.h and apply_preset logic).
    // Applied as a base layer: explicit CLI > --config JSON > preset > defaults.
    std::string preset;

    std::string provider_name;
    std::string provider_path;
    std::string strategy;
    std::string format;

    // Monte Carlo / synthetic provider (Phase 1+)
    std::string mc_params;
    bool mc_params_explicitly_set = false;

    // Phase 3: Full Monte Carlo campaign mode
    bool monte_carlo = false;
    std::size_t mc_trials = 0;
    bool mc_trials_explicitly_set = false;
    std::string mc_model;           // gbm, ou, etc.
    bool mc_model_explicitly_set = false;

    // Phase 5: Experimental parallel MC trials (see warnings in controller)
    bool mc_parallel = false;
    unsigned mc_workers = 0;
    bool mc_workers_explicitly_set = false;

    // Phase A (MC object reuse): reuse data_handler + strategy (and prepared reset for others)
    bool mc_reuse_objects = false;

    std::size_t sma_period = 20;
    std::vector<std::string> params;
    std::string mode;

    // Conservative, venue-agnostic crypto baseline (10 bps per side).
    // `--fee zero` is an explicit research opt-out; venue-specific configs
    // should override these rates with their documented schedule.
    std::string fee_model{"tiered"};
    double fee_value = 0.0;
    double maker_rate = 0.001;
    double taker_rate = 0.001;
    // Defaults are acceptable for reproducible backtests only. Protected
    // provider modes require the operator/config to state their actual fee
    // schedule and period basis explicitly.
    bool fee_model_explicit = false;
    bool maker_rate_explicit = false;
    bool taker_rate_explicit = false;

    std::string symbol;
    std::string stream;
    std::string api_key;
    std::string api_secret;
    std::string api_passphrase;  // Bitget (and similar) signed-REST passphrase
    std::string host;
    std::string port;
    std::string depth_stream;
    std::string record_path;
    std::string replay_data_path;
    bool live = false;
    bool testnet = false;
    bool demo = false;  // Bitget demo/paptrading (also accepted via --testnet for bitget*)

    // Futures-only advisory inputs. Sentinel -1 = operator didn't set,
    // use provider default (5%). Empty margin_type = check disabled.
    std::string margin_type;
    double      liquidation_warn_pct = -1.0;
    bool        margin_type_strict   = false;

    // Futures-only pre-trade risk caps. Sentinel <0 = operator didn't
    // set; the provider treats zero as disabled and won't construct
    // a FuturesRiskCheck unless at least one cap is positive.
    double      max_notional_usdt            = -1.0;
    double      max_leverage                 = -1.0;
    double      min_liquidation_distance_pct = -1.0;

    // Futures-only dead-man's switch. Default 30000ms (30s); set to 0
    // (or pass --disarm-deadman) to disable for a specific run.
    // heartbeat_ms == 0 means "use countdown / 3" inside the provider.
    long        dead_man_countdown_ms = 30000;
    long        dead_man_heartbeat_ms = 0;
    bool        disarm_deadman = false;

    double balance = 10000.0;
    double risk_fraction = 0.02;
    double sl_pct = 0.003;   // platform default stop-loss (0 = off / strategy-defined)
    double tp_pct = 0.01;    // platform default take-profit (0 = off / strategy-defined)
    double trail_pct = 0.0;  // manifest-bound engine default; no CLI override
    // Platform default exit policy: floor|strategy_only|engine_only|union
    std::string exit_policy = "floor";

    std::string checkpoint_path;
    std::string resume_path;
    std::size_t checkpoint_interval = 10000;

    int backfill = 500;
    std::string backfill_interval;

    double market_aggression = 1.1;
    double qty_scale = 1e8;
    unsigned fill_rng_seed = 42;
    bool fill_rng_seed_explicitly_set = false;
    double spread_step = 0.0001;
    bool spread_step_explicitly_set = false;

    bool debug_fills = false;
    int debug_fills_budget = 20;

    std::string config_file;
    bool dump_config_flag = false;
    bool dry_run = false;

    std::size_t rolling_window = 252;
    double risk_free_rate = 0.0;
    std::size_t periods_per_year = 525600; // 1-minute 24/7 crypto calendar
    bool periods_per_year_explicit = false;
    std::size_t max_equity_points = 100000;
    std::size_t exec_bar_delay = 1;
    int wire_latency_us = 0;
    int order_latency_us = 0;
    int order_latency_stddev_us = 0;
    double impact_k_bps = 0.0;
    double impact_adv = 0.0;
    bool realistic_fills = false;     // deprecated, warn-noop
    double bar_spread_bps = 0.0;      // deprecated, warn-noop
    bool walked_book_impact = false;
    double fill_prob = 0.0;           // 0 = fill model off
    double fill_fade = 0.0;
    double fill_decay = 10.0;
    int mm_levels = 10;
    int mm_base_depth = 100;
    double mm_spread_pct = 0.002;
    double mm_vol_mult = 0.25;
    double mm_max_spread_pct = 0.05;
    std::string queue_model = "none";
    std::string maker_queue_model = "none";
    std::vector<std::string> instrument_specs;
    double reconcile_tolerance_bps = 10.0;
    long kill_switch_deadline_ms = 5000;
    std::string output;
    std::string output_format = "json";

    // Dashboard: auto (tui if tty, else plain), or force a mode.
    std::string status_format = "auto";
    bool no_tui = false;
    // Forces ConsoleDashboard's own ncurses-free ANSI-box TUI
    // (render_tui()) over the rich ncurses TabbedDashboard on
    // engine_shadow/engine_live, when both would otherwise be eligible.
    // Without this flag there is no way to reach ConsoleDashboard's TUI on
    // those binaries - TabbedDashboard always wins whenever both are
    // constructed. See tt_wire_rich_tui/rich_tui construction below.
    bool simple_tui = false;

    // ImGui personal strategy desk (HAS_IMGUI_DESK). Mutually exclusive with
    // rich ncurses TUI when both are available — desk wins if --desk is set.
    bool desk = false;
    // Starts the desk with deterministic "DEMO DATA" research panels already
    // enabled (same effect as the menu toggle) - lets headless visual QA
    // (footprint.md §3) and manual smoke checks reach the research surfaces
    // without a mouse click.
    bool desk_demo_data = false;

    // footprint.md §1: "Automatic activation with --desk in HAS_IMGUI_DESK
    // builds; --no-footprint will disable collection." Collection never
    // affects trading behavior either way - it only feeds the desk's
    // Orderflow footprint panel.
    bool no_footprint = false;
    // Exact-decimal tick-size override for the active symbol
    // (footprint.md §2.1 footprint.tick_size_overrides.<venue>.<symbol> /
    // --footprint-tick-size). Kept as a STRING end to end so the whole
    // price-bucketing chain stays exact - never parsed to a double.
    std::string footprint_tick_size;

    // Embedded web UI (HAS_WEB). Read-only; off by default.
    bool web = false;
    int web_port = 8080;
    std::string web_bind = "127.0.0.1";
    std::string web_token;
    std::string web_assets;

    double risk_max_position_value = 1e9;
    double risk_max_drawdown = 0.30;
    double risk_max_loss_per_trade = 10000.0;
    int risk_max_open_orders = 1000;
    double risk_max_portfolio_exposure = 5e9;
    // F-05b: negative = "not set", so the spot default can be applied without
    // overriding an explicit --max-leverage 0 (meaning: disable the cap).
    double risk_max_gross_leverage = -1.0;
    double risk_max_daily_loss = 0.0;

    int risk_daily_reset_hour = 0;
    int risk_max_trades_per_hour = 0;
    int risk_max_orders_per_minute = 0;
    // R3 — authoritative risk accounting.
    double risk_max_inventory_qty = 0.0;      // hard per-symbol inventory cap (base qty)
    double risk_max_funding_8h_rate = 0.0;    // funding circuit breaker (fraction)
    long long risk_max_mark_age_ms = 0;       // 0 = staleness classification off
    bool risk_require_fresh_mark = false;     // stale/missing mark blocks new inventory
    bool risk_unwind = false;
    std::optional<bool> risk_soft_portfolio_limits;

#ifdef HAS_QUESTDB
    bool persist = false;
    std::string run_tag;
    std::string run_notes;
    std::string questdb_host = "127.0.0.1";
    std::uint16_t questdb_ilp_port = 9009;
    std::uint16_t questdb_http_port = 9000;
    int questdb_flush_ms = 150;   // time-based ILP flush cadence (Phase 1)
    bool questdb_strict = false;  // Phase 2: hard-fail on QuestDB problems + local fallback
#endif
};

// "binance" and "binance-futures" are two distinct, non-aliased providers
// (spot vs. USD-M futures - see binance_register.cpp / binance_futures_
// register.cpp), unlike bitget/bitunix which register both their bare and
// "-futures" names against the same factory. So this must stay an exact
// match, never a prefix match, or it would also catch spot "binance".
static bool provider_is_binance_futures(const std::string& name)
{
    return name == "binance-futures";
}

// F-10b (docs/todos/11-F-forensic-lifecycle-audit.md): generate the
// --strategy help from StrategyRegistry — the registry that actually
// validates and constructs the flag. The hardcoded list this replaces named
// six strategies and omitted every one registered after it was written,
// including ema-rsi-atr-pullback, so `--help` told operators a constructible
// strategy did not exist.
static std::string strategy_help_text()
{
    auto names = StrategyRegistry::instance().available();
    std::sort(names.begin(), names.end());
    std::string out = "Strategy: ";
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (i) out += ", ";
        out += names[i];
    }
    if (names.empty()) out += "(none registered)";
    return out;
}

// F-07b (docs/todos/11-F-forensic-lifecycle-audit.md): bind --symbol to a

// symbol-less source at load. The Binance kline CSV carries no symbol column,
// so every bar arrives with symbol "" and nothing downstream binds to it —
// --symbol does not match, per-symbol attribution is blank, and --instrument
// filters are inert. Only unbound rows are rewritten, so a real multi-symbol
// load is never touched.
//
// Decision (plan open question 5): an unset --symbol on symbol-less data is
// NOT an error. Plenty of single-instrument research runs never name the
// instrument and do not care. It is a loud warning instead, because the
// consequences (blank attribution, inert instrument filters) are silent
// otherwise — and engine::validate_instrument_overrides turns it into a hard
// failure the moment an --instrument spec is actually configured.
static void bind_symbol_to_series(MarketSeries& series, const std::string& symbol)
{
    if (!series.has_unbound_symbols()) return;

    if (symbol.empty())
    {
        std::cerr << "  ! Data source carries no symbol column and --symbol is "
                     "unset: per-symbol attribution stays blank and any "
                     "--instrument spec cannot bind.\n";
        return;
    }

    const auto bound = series.bind_unset_symbols(symbol);
    if (bound.bars > 0 || bound.ticks > 0)
    {
        std::cout << "    Bound --symbol " << symbol << " to "
                  << bound.bars << " bar(s) and " << bound.ticks
                  << " tick(s) with no symbol of their own\n";
    }
}

// F-05b: spot accounts cannot borrow, so gross exposure is bounded by equity.

// Futures venues keep the cap off — FuturesRiskCheck already owns notional,
// leverage and liquidation distance there, and stacking both would
// double-charge the same exposure. A negative CLI value means "unset".
static double resolve_max_gross_leverage(double cli_value,
                                         const std::string& provider_name)
{
    if (cli_value >= 0.0) return cli_value;   // explicit operator choice wins
    const bool futures =
        provider_name.find("futures") != std::string::npos ||
        provider_name.rfind("bitget", 0) == 0;
    return futures ? 0.0 : 1.0;
}

// Phase-0 live discipline for futures venues (Binance USDT-M + Bitget UTA).

// Mainnet/live without sandbox flags: refuse if no venue caps or daily-loss.
// Sandbox (Binance --testnet; Bitget --demo|--testnet): warn only.
static bool is_futures_live_provider(const std::string& name)
{
    if (provider_is_binance_futures(name))
        return true;
    // bitget / bitget-futures (spot Bitget is not in scope)
    return name.rfind("bitget", 0) == 0;
}

static bool futures_live_sandbox_flags(const cli_options& o)
{
    // Bitget paptrading accepts --demo or --testnet; Binance futures only --testnet.
    if (o.provider_name.rfind("bitget", 0) == 0)
        return o.demo || o.testnet;
    return o.testnet;
}

static bool validate_binance_futures_live_phase0(const cli_options& o,
                                                 std::ostream& errors,
                                                 std::ostream& warnings)
{
    if (!std::isfinite(o.min_liquidation_distance_pct)
        || (o.min_liquidation_distance_pct < 0.0
            && o.min_liquidation_distance_pct != -1.0)
        || o.min_liquidation_distance_pct > 1.0)
    {
        errors << "  ! --min-liq-distance-pct must be a finite fraction in "
                  "[0,1] (use 0.07 for 7%).\n";
        return false;
    }
    if (!std::isfinite(o.max_notional_usdt)
        || !std::isfinite(o.max_leverage)
        || !std::isfinite(o.risk_max_daily_loss))
    {
        errors << "  ! --max-notional, --max-leverage, and "
                  "--max-daily-loss must be finite values.\n";
        return false;
    }
    if (o.dead_man_countdown_ms < 0 || o.dead_man_heartbeat_ms < 0
        || (o.dead_man_countdown_ms == 0 && o.dead_man_heartbeat_ms > 0)
        || (o.dead_man_countdown_ms > 0 && o.dead_man_heartbeat_ms > 0
            && o.dead_man_heartbeat_ms >= o.dead_man_countdown_ms))
    {
        errors << "  ! Dead-man timing must be non-negative; an explicit "
                  "heartbeat requires a positive countdown and must be "
                  "strictly shorter than it.\n";
        return false;
    }

    const bool resolved_live = (o.mode == "live" || o.live);
    if (!is_futures_live_provider(o.provider_name) || !resolved_live)
        return true;

    const bool sandbox = futures_live_sandbox_flags(o);

    const bool has_venue_caps =
        o.max_notional_usdt > 0.0 ||
        o.max_leverage > 0.0 ||
        o.min_liquidation_distance_pct > 0.0;

    if (!has_venue_caps)
    {
        if (!sandbox)
        {
            errors << "  ! Refusing mainnet futures live mode without venue risk caps.\n"
                      "    Set at least one of --max-notional, --max-leverage, "
                      "or --min-liq-distance-pct.\n";
            return false;
        }
        warnings << "  [Phase 0] WARNING: No venue risk caps set "
                    "(--max-notional / --max-leverage / --min-liq-distance-pct).\n"
                    "           These are strongly recommended before mainnet validation runs.\n";
    }

    if (o.risk_max_daily_loss <= 0.0)
    {
        if (!sandbox)
        {
            errors << "  ! Refusing mainnet futures live mode with --max-daily-loss disabled.\n"
                      "    Set a conservative positive --max-daily-loss.\n";
            return false;
        }
        warnings << "  [Phase 0] WARNING: --max-daily-loss is 0 (disabled). "
                    "Set a conservative daily loss limit before mainnet.\n";
    }

    if (!sandbox && o.event_log_path.empty())
    {
        errors << "  ! Refusing mainnet futures live mode without a durable "
                  "binary event log.\n"
                  "    Set --log-events to an operator-controlled path.\n";
        return false;
    }
    // H-07 (docs/todos/08-H-persistence-observability.md): a plain
    // std::ofstream::open() succeeds silently against /dev/null, a FIFO, or
    // a device node, so the emptiness check above is not sufficient to
    // guarantee the "mandatory durable truth" the mainnet ritual promises.
    if (!sandbox && !o.event_log_path.empty()
        && !is_acceptable_durable_log_target(o.event_log_path))
    {
        errors << "  ! Refusing mainnet futures live mode: --log-events target "
                  "is not a regular file (rejects /dev/null, FIFOs, device "
                  "nodes, and non-regular symlinks).\n"
                  "    Point --log-events at an operator-controlled regular "
                  "file path.\n";
        return false;
    }
    if (!sandbox && o.log_max_size_mb > 0)
    {
        errors << "  ! Refusing mainnet futures live mode with binary event "
                  "log rotation enabled.\n"
                  "    Use --log-max-size 0 until rotated segments have an "
                  "authoritative stitching manifest.\n";
        return false;
    }

    return true;
}

static std::vector<std::string> split_csv(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void apply_strategy_params(IStrategy& strategy,
                                  const std::vector<std::string>& params)
{
    for (const auto& p : params)
    {
        auto eq = p.find('=');
        if (eq == std::string::npos)
        {
            std::cerr << "  ! Invalid --param format (expected key=value): " << p << "\n";
            continue;
        }
        std::string key = p.substr(0, eq);
        double value = std::stod(p.substr(eq + 1));
        strategy.set_param(key, value);
    }
}

// Platform DefaultExitPolicy: applies to every strategy via engine_config.
// Strategies may still declare richer exit_intents; --exit-policy controls merge.
static void apply_exit_defaults(engine_config& cfg, const cli_options& o)
{
    auto mode = truetest::exits::parse_exit_policy_mode(o.exit_policy);
    if (!mode)
    {
        std::cerr << "  ! Unknown --exit-policy '" << o.exit_policy
                  << "' (use floor|strategy_only|engine_only|union); defaulting to floor\n";
        mode = truetest::exits::exit_policy_mode::floor;
    }
    cfg.exit_defaults.mode = *mode;
    cfg.exit_defaults.sl_pct = o.sl_pct;
    cfg.exit_defaults.tp_pct = o.tp_pct;
    cfg.exit_defaults.trail_pct = o.trail_pct;
}

// Push CLI fee / spread realism into strategy sizing params when supported.
// Market entries and stop exits are treated as taker.
//
// F-04: the adverse-slip estimate comes from --mm-spread-pct, the parameter
// that actually sets the synthetic book's half spread and therefore the price
// a market entry fills at. It used to come from --bar-spread-bps, which the
// CLI documents as having no effect and which defaults to zero — so sizing
// saw no execution cost at all and overshot by ≈4.9×.
static void apply_execution_cost_params(IStrategy& strategy, const cli_options& o)
{
    ::apply_execution_cost_params(strategy, o.maker_rate, o.taker_rate,
                                  o.bar_spread_bps, o.fee_model, o.fee_value,
                                  o.mm_spread_pct);
}


// Parse one --instrument entry (e.g. "BTCUSDT:tick=0.01,lot=0.00001,minn=5").
// Unknown fields are ignored so the syntax stays extensible.
static bool parse_instrument_arg(const std::string& arg, instrument_spec& out)
{
    auto colon = arg.find(':');
    if (colon == std::string::npos) return false;
    out.symbol = arg.substr(0, colon);
    std::string rest = arg.substr(colon + 1);
    std::size_t pos = 0;
    while (pos < rest.size())
    {
        auto next = rest.find(',', pos);
        if (next == std::string::npos) next = rest.size();
        auto kv = rest.substr(pos, next - pos);
        auto eq = kv.find('=');
        if (eq != std::string::npos)
        {
            std::string k = kv.substr(0, eq);
            double v = std::stod(kv.substr(eq + 1));
            if      (k == "tick")  out.tick_size = v;
            else if (k == "lot")   out.lot_size = v;
            else if (k == "minq")  out.min_qty = v;
            else if (k == "minn")  out.min_notional = v;
            else if (k == "maker") out.maker_rate = v;
            else if (k == "taker") out.taker_rate = v;
        }
        pos = next + 1;
    }
    return !out.symbol.empty();
}

static std::unordered_map<std::string, instrument_spec>
parse_instrument_specs(const std::vector<std::string>& args)
{
    std::unordered_map<std::string, instrument_spec> out;
    for (const auto& a : args)
    {
        instrument_spec s;
        if (parse_instrument_arg(a, s))
            out.emplace(s.symbol, std::move(s));
        else
            std::cerr << "  ! Invalid --instrument arg (expected SYM:k=v[,k=v]): " << a << "\n";
    }
    return out;
}

static void export_results(const Analytics& analytics,
                           const std::string& output_path,
                           const std::string& format)
{
    if (output_path.empty()) return;

    if (format == "csv")
    {
        std::string equity_path = output_path;
        std::string trades_path = output_path;
        auto dot = trades_path.rfind('.');
        if (dot != std::string::npos)
            trades_path.insert(dot, "_trades");
        else
            trades_path += "_trades";
        analytics.export_csv(equity_path, trades_path);
        std::cout << "  Results exported to: " << equity_path << " + " << trades_path << "\n";
    }
    else
    {
        analytics.export_json(output_path);
        std::cout << "  Results exported to: " << output_path << "\n";
    }
}

static void load_config_file(const std::string& path, cli_options& o)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        std::cerr << "  ! Cannot open config file: " << path << "\n";
        std::exit(1);
    }

    nlohmann::json j;
    try { j = nlohmann::json::parse(f); }
    catch (const nlohmann::json::parse_error& e)
    {
        std::cerr << "  ! Config JSON parse error: " << e.what() << "\n";
        std::exit(1);
    }

    auto get_str = [&](const char* k, std::string& out) {
        if (j.contains(k) && j[k].is_string()) out = j[k].get<std::string>();
    };
    auto get_double = [&](const char* k, double& out) {
        if (j.contains(k) && j[k].is_number()) out = j[k].get<double>();
    };
    auto get_size = [&](const char* k, std::size_t& out) {
        if (j.contains(k) && j[k].is_number_unsigned()) out = j[k].get<std::size_t>();
    };
    auto get_int = [&](const char* k, int& out) {
        if (j.contains(k) && j[k].is_number_integer()) out = j[k].get<int>();
    };
    auto get_bool = [&](const char* k, bool& out) {
        if (j.contains(k) && j[k].is_boolean()) out = j[k].get<bool>();
    };
#ifdef HAS_QUESTDB
    auto get_u16 = [&](const char* k, std::uint16_t& out) {
        if (j.contains(k) && j[k].is_number_unsigned())
            out = static_cast<std::uint16_t>(j[k].get<unsigned>());
    };
#endif

    get_str("replay", o.replay_path);
    get_str("log_events", o.event_log_path);
    get_str("write_run_manifest", o.write_run_manifest_path);
    get_str("replay_run_manifest", o.replay_run_manifest_path);
    get_str("artifacts_dir", o.reproducibility_artifacts_directory);
    get_bool("verify_hashes", o.verify_hashes);
    get_bool("allow_dataset_mismatch", o.allow_dataset_mismatch);
    if (j.contains("seed"))
    {
        if (!j["seed"].is_number_unsigned())
        {
            std::cerr << "  ! Config field 'seed' must be an unsigned integer\n";
            std::exit(1);
        }
        o.seed = j["seed"].get<std::uint64_t>();
        o.seed_explicitly_set = true;
    }
    get_str("thread_preset", o.thread_preset_str);
    get_str("preset", o.preset);
    get_bool("no_pin", o.no_pin);
    get_str("provider", o.provider_name);
    get_str("path", o.provider_path);
    get_bool("monte_carlo", o.monte_carlo);
    get_size("mc_trials", o.mc_trials);
    o.mc_trials_explicitly_set =
        j.contains("mc_trials") && j["mc_trials"].is_number_unsigned();
    get_str("mc_model", o.mc_model);
    o.mc_model_explicitly_set = j.contains("mc_model");
    get_str("mc_params", o.mc_params);
    o.mc_params_explicitly_set = j.contains("mc_params");
    get_bool("mc_parallel", o.mc_parallel);
    if (j.contains("mc_workers") && j["mc_workers"].is_number_unsigned())
    {
        o.mc_workers = j["mc_workers"].get<unsigned>();
        o.mc_workers_explicitly_set = true;
    }
    get_bool("mc_reuse_objects", o.mc_reuse_objects);
    get_str("strategy", o.strategy);
    if (j.contains("params"))
    {
        if (!j["params"].is_array())
        {
            std::cerr << "  ! Config field 'params' must be an array of strings\n";
            std::exit(1);
        }
        o.params.clear();
        for (const auto& parameter : j["params"])
        {
            if (!parameter.is_string())
            {
                std::cerr << "  ! Config field 'params' must contain only strings\n";
                std::exit(1);
            }
            o.params.push_back(parameter.get<std::string>());
        }
    }
    get_str("format", o.format);
    get_size("sma_period", o.sma_period);
    get_str("fee", o.fee_model);
    get_double("fee_value", o.fee_value);
    get_double("maker_rate", o.maker_rate);
    get_double("taker_rate", o.taker_rate);
    o.fee_model_explicit = j.contains("fee") && j["fee"].is_string();
    o.maker_rate_explicit = j.contains("maker_rate") && j["maker_rate"].is_number();
    o.taker_rate_explicit = j.contains("taker_rate") && j["taker_rate"].is_number();
    get_str("symbol", o.symbol);
    get_str("stream", o.stream);
    get_str("api_key", o.api_key);
    get_str("api_secret", o.api_secret);
    get_str("api_passphrase", o.api_passphrase);
    get_str("host", o.host);
    get_str("port", o.port);
    get_str("record", o.record_path);
    get_str("replay_data", o.replay_data_path);
    get_bool("live", o.live);
    get_bool("testnet", o.testnet);
    get_bool("demo", o.demo);
    get_str("mode", o.mode);
    get_double("balance", o.balance);
    get_double("risk_fraction", o.risk_fraction);
    get_double("sl", o.sl_pct);
    get_double("tp", o.tp_pct);
    get_str("exit_policy", o.exit_policy);
    get_int("backfill", o.backfill);
    get_str("backfill_interval", o.backfill_interval);
    get_double("market_aggression", o.market_aggression);
    get_double("quantity_scale", o.qty_scale);
    if (j.contains("fill_rng_seed")
        && j["fill_rng_seed"].is_number_unsigned())
    {
        o.fill_rng_seed = j["fill_rng_seed"].get<unsigned>();
        o.fill_rng_seed_explicitly_set = true;
    }
    if (j.contains("spread_step_factor")
        && j["spread_step_factor"].is_number())
    {
        o.spread_step = j["spread_step_factor"].get<double>();
        o.spread_step_explicitly_set = true;
    }

    if (j.contains("risk") && j["risk"].is_object())
    {
        auto& r = j["risk"];
        auto rd = [&](const char* k, double& out) {
            if (r.contains(k) && r[k].is_number()) out = r[k].get<double>();
        };
        auto ri = [&](const char* k, int& out) {
            if (r.contains(k) && r[k].is_number_integer()) out = r[k].get<int>();
        };
        rd("max_position_value", o.risk_max_position_value);
        rd("max_drawdown", o.risk_max_drawdown);
        rd("max_loss_per_trade", o.risk_max_loss_per_trade);
        ri("max_open_orders", o.risk_max_open_orders);
        rd("max_portfolio_exposure", o.risk_max_portfolio_exposure);
        rd("max_gross_leverage", o.risk_max_gross_leverage);
        rd("max_daily_loss", o.risk_max_daily_loss);
        ri("daily_reset_hour", o.risk_daily_reset_hour);
        ri("max_trades_per_hour", o.risk_max_trades_per_hour);
        ri("max_orders_per_minute", o.risk_max_orders_per_minute);
        rd("max_symbol_inventory_qty", o.risk_max_inventory_qty);
        rd("max_funding_8h_rate", o.risk_max_funding_8h_rate);
        if (r.contains("max_mark_age_ms") && r["max_mark_age_ms"].is_number_integer())
            o.risk_max_mark_age_ms = r["max_mark_age_ms"].get<long long>();
        if (r.contains("require_fresh_mark") && r["require_fresh_mark"].is_boolean())
            o.risk_require_fresh_mark = r["require_fresh_mark"].get<bool>();
        if (r.contains("soft_portfolio_limits") && r["soft_portfolio_limits"].is_boolean())
            o.risk_soft_portfolio_limits = r["soft_portfolio_limits"].get<bool>();
        if (r.contains("unwind") && r["unwind"].is_boolean())
            o.risk_unwind = r["unwind"].get<bool>();
    }

    get_size("rolling_window", o.rolling_window);
    get_double("risk_free_rate", o.risk_free_rate);
    get_size("periods_per_year", o.periods_per_year);
    o.periods_per_year_explicit =
        j.contains("periods_per_year") && j["periods_per_year"].is_number_unsigned();
    get_size("max_equity_points", o.max_equity_points);
    get_size("exec_bar_delay", o.exec_bar_delay);
    get_int("wire_latency_us", o.wire_latency_us);
    get_int("order_latency_us", o.order_latency_us);
    get_int("order_latency_stddev_us", o.order_latency_stddev_us);
    get_double("impact_k_bps", o.impact_k_bps);
    get_double("impact_adv", o.impact_adv);
    get_bool("realistic_fills", o.realistic_fills);
    get_double("bar_spread_bps", o.bar_spread_bps);
    get_bool("walked_book_impact", o.walked_book_impact);
    get_double("fill_prob", o.fill_prob);
    get_double("fill_fade", o.fill_fade);
    get_double("fill_decay", o.fill_decay);
    get_int("mm_levels", o.mm_levels);
    get_int("mm_base_depth", o.mm_base_depth);
    get_double("mm_spread_pct", o.mm_spread_pct);
    get_double("mm_vol_mult", o.mm_vol_mult);
    get_double("mm_max_spread_pct", o.mm_max_spread_pct);
    get_str("queue_model", o.queue_model);
    get_str("maker_queue_model", o.maker_queue_model);
    if (j.contains("instrument"))
    {
        if (!j["instrument"].is_array())
        {
            std::cerr << "  ! Config field 'instrument' must be an array of strings\n";
            std::exit(1);
        }
        o.instrument_specs.clear();
        for (const auto& instrument : j["instrument"])
        {
            if (!instrument.is_string())
            {
                std::cerr << "  ! Config field 'instrument' must contain only strings\n";
                std::exit(1);
            }
            o.instrument_specs.push_back(instrument.get<std::string>());
        }
    }
    get_str("output", o.output);
    get_str("output_format", o.output_format);
#ifdef HAS_QUESTDB
    get_bool("persist", o.persist);
    get_str("run_tag", o.run_tag);
    get_str("run_notes", o.run_notes);
    get_str("questdb_host", o.questdb_host);
    get_u16("questdb_ilp_port", o.questdb_ilp_port);
    get_u16("questdb_http_port", o.questdb_http_port);
    get_int("questdb_flush_ms", o.questdb_flush_ms);
    get_bool("questdb_strict", o.questdb_strict);
#endif
}

static void dump_config(const cli_options& o)
{
    nlohmann::json j;
    j["replay"] = o.replay_path;
    j["log_events"] = o.event_log_path;
    j["write_run_manifest"] = o.write_run_manifest_path;
    j["replay_run_manifest"] = o.replay_run_manifest_path;
    j["artifacts_dir"] = o.reproducibility_artifacts_directory;
    j["verify_hashes"] = o.verify_hashes;
    j["seed"] = o.seed;
    j["thread_preset"] = o.thread_preset_str;
    j["preset"] = o.preset;
    j["no_pin"] = o.no_pin;
    j["provider"] = o.provider_name;
    j["path"] = o.provider_path;
    j["monte_carlo"] = o.monte_carlo;
    j["mc_trials"] = o.mc_trials;
    j["mc_model"] = o.mc_model;
    j["mc_params"] = o.mc_params;
    j["mc_parallel"] = o.mc_parallel;
    j["mc_workers"] = o.mc_workers;
    j["mc_reuse_objects"] = o.mc_reuse_objects;
    j["periods_per_year"] = o.periods_per_year;
    j["strategy"] = o.strategy.empty() ? "mean-reversion" : o.strategy;
    j["params"] = o.params;
    j["format"] = o.format;
    j["sma_period"] = o.sma_period;
    j["fee"] = o.fee_model;
    j["fee_value"] = o.fee_value;
    j["maker_rate"] = o.maker_rate;
    j["taker_rate"] = o.taker_rate;
    j["symbol"] = o.symbol;
    j["stream"] = o.stream;
    j["host"] = o.host;
    j["port"] = o.port;
    j["record"] = o.record_path;
    j["replay_data"] = o.replay_data_path;
    j["live"] = o.live;
    j["testnet"] = o.testnet;
    j["demo"] = o.demo;
    j["mode"] = o.mode.empty() ? TT_DEFAULT_MODE : o.mode;
    j["balance"] = o.balance;
    j["risk_fraction"] = o.risk_fraction;
    j["min_liquidation_distance_pct"] = o.min_liquidation_distance_pct;
    j["sl"] = o.sl_pct;
    j["tp"] = o.tp_pct;
    j["exit_policy"] = o.exit_policy;
    j["backfill"] = o.backfill;
    j["backfill_interval"] = o.backfill_interval;
    j["market_aggression"] = o.market_aggression;
    j["quantity_scale"] = o.qty_scale;
    j["fill_rng_seed"] = o.fill_rng_seed;
    j["spread_step_factor"] = o.spread_step;
    j["wire_latency_us"] = o.wire_latency_us;
    j["order_latency_us"] = o.order_latency_us;
    j["order_latency_stddev_us"] = o.order_latency_stddev_us;
    j["impact_k_bps"] = o.impact_k_bps;
    j["impact_adv"] = o.impact_adv;
    j["realistic_fills"] = o.realistic_fills;
    j["bar_spread_bps"] = o.bar_spread_bps;
    j["walked_book_impact"] = o.walked_book_impact;
    j["fill_prob"] = o.fill_prob;
    j["fill_fade"] = o.fill_fade;
    j["fill_decay"] = o.fill_decay;
    j["mm_levels"] = o.mm_levels;
    j["mm_base_depth"] = o.mm_base_depth;
    j["mm_spread_pct"] = o.mm_spread_pct;
    j["mm_vol_mult"] = o.mm_vol_mult;
    j["mm_max_spread_pct"] = o.mm_max_spread_pct;
    j["queue_model"] = o.queue_model;
    j["maker_queue_model"] = o.maker_queue_model;
    j["instrument"] = o.instrument_specs;
    j["risk"] = {
        {"max_position_value", o.risk_max_position_value},
        {"max_drawdown", o.risk_max_drawdown},
        {"max_loss_per_trade", o.risk_max_loss_per_trade},
        {"max_open_orders", o.risk_max_open_orders},
        {"max_portfolio_exposure", o.risk_max_portfolio_exposure},
        {"max_gross_leverage", o.risk_max_gross_leverage},
        {"max_daily_loss", o.risk_max_daily_loss},
        {"daily_reset_hour", o.risk_daily_reset_hour},
        {"max_trades_per_hour", o.risk_max_trades_per_hour},
        {"max_orders_per_minute", o.risk_max_orders_per_minute},
        {"max_symbol_inventory_qty", o.risk_max_inventory_qty},
        {"max_funding_8h_rate", o.risk_max_funding_8h_rate},
        {"max_mark_age_ms", o.risk_max_mark_age_ms},
        {"require_fresh_mark", o.risk_require_fresh_mark},
        {"soft_portfolio_limits", o.risk_soft_portfolio_limits.value_or(true)},
        {"unwind", o.risk_unwind}
    };
#ifdef HAS_QUESTDB
    j["persist"] = o.persist;
    j["run_tag"] = o.run_tag;
    j["run_notes"] = o.run_notes;
    j["questdb_host"] = o.questdb_host;
    j["questdb_ilp_port"] = o.questdb_ilp_port;
    j["questdb_http_port"] = o.questdb_http_port;
    j["questdb_flush_ms"] = o.questdb_flush_ms;
    j["questdb_strict"] = o.questdb_strict;
#endif
    std::cout << j.dump(2) << "\n";
}

static void register_cli_options(CLI::App& app, cli_options& o)
{
    app.add_option("--replay", o.replay_path, "Path to event log for replay mode");
    app.add_option("--replay-from", o.replay_from_us,
                   "Replay lower bound (microseconds since epoch; nonzero is "
                   "currently refused because ledger prefix state is unavailable)");
    app.add_option("--replay-to", o.replay_to_us,
                   "Replay upper bound (microseconds since epoch; non-default "
                   "is currently refused because append order need not follow event time)");
    app.add_option("--log-events", o.event_log_path, "Path to write binary event log");
    app.add_option("--write-run-manifest", o.write_run_manifest_path,
                   "Write a canonical deterministic-run manifest (backtest MC)");
    app.add_option("--replay-run-manifest", o.replay_run_manifest_path,
                   "Replay a deterministic run from a canonical manifest");
    app.add_option("--artifacts-dir", o.reproducibility_artifacts_directory,
                   "Root for deterministic receipt/report/per-trial artifacts");
    app.add_flag("--verify-hashes", o.verify_hashes,
                 "Compare replay outputs with hashes stored in the manifest");
    app.add_flag("--allow-dataset-mismatch", o.allow_dataset_mismatch,
                 "Explicitly continue after dataset mismatch; receipt marks replay non-exact");
    app.add_option("--trial", o.deterministic_trial_index,
                   "Replay only this stable Monte Carlo trial index");
    app.add_option("--log-file", o.log_file_path, "Path to write operational log (L1, default: stderr)");
    app.add_option("--log-max-size", o.log_max_size_mb, "Max size (MB) per event/text log before rotation (L3, 0 = no rotation)");
    app.add_option("--log-keep", o.log_keep, "Number of rotated log files to retain (L3, default: 5)");
    app.add_flag("--compress-log,!--no-compress-log", o.compress_log, "Compress binary event logs with zstd (default: on)");
    app.add_option("--seed", o.seed,
                   "Required deterministic master seed for backtest/Monte Carlo; 0 is valid");
    app.add_option("--thread-preset", o.thread_preset_str,
                   "Threading: inline, logging, light, standard, full, extended");
    app.add_option("--preset", o.preset, "Named run preset/profile (e.g. futures-phase0, mc-robustness, backtest-l2, shadow-tape). Provides bundles of realistic defaults for common scenarios; explicit flags and --config override the preset. See --help or docs for the list.");
    app.add_flag("--no-pin", o.no_pin, "Disable CPU affinity pinning");
    app.add_option("--spin-policy", o.spin_policy_str, "Worker spin policy: spin, yield, adaptive (default: adaptive)");
    app.add_option("--provider", o.provider_name,
                   "Provider name: local, binance, binance-futures, bitget, "
                   "bitget-futures, bitunix, bitunix-futures, synthetic");
    app.add_option("--path", o.provider_path, "Data file path (for local provider)");
    app.add_option("--mc-params", o.mc_params, "Synthetic/Monte-Carlo params (e.g. \"mu=0.0,sigma=0.65,n_steps=5000\")");
    app.add_flag("--monte-carlo", o.monte_carlo, "Run Monte Carlo campaign (use with --mc-trials)");
    app.add_option("--mc-trials", o.mc_trials, "Number of independent Monte Carlo trials");
    app.add_option("--mc-model", o.mc_model, "Generator model for Monte Carlo (gbm only; default: gbm)");
    app.add_flag("--mc-parallel", o.mc_parallel, "EXPERIMENTAL: Run MC trials in parallel (Phase 5). Strong warnings apply - see controller docs. Only safe with inline threading preset.");
    app.add_option("--mc-workers", o.mc_workers,
                   "Explicit maximum parallel MC workers (required for deterministic parallel manifests)");
    app.add_flag("--mc-reuse-objects", o.mc_reuse_objects, "Phase A: Reuse data_handler/strategy (and reset other heavy objects) between MC trials instead of full reconstruction each time.");
    app.add_option("--strategy", o.strategy, strategy_help_text());

    app.add_option("--format", o.format, "Data format: tick, bar");
    app.add_option("--sma-period", o.sma_period, "SMA indicator period")->default_val(20);
    app.add_option("--param", o.params, "Strategy parameter: key=value (repeatable)");
    app.add_option("--mode", o.mode, "Engine mode: backtest, shadow, live");

    app.add_option("--fee", o.fee_model, "Fee model: zero, fixed, tiered")
       ->default_val("tiered");
    app.add_option("--fee-value", o.fee_value, "Fixed fee per trade");
    app.add_option("--maker-rate", o.maker_rate, "Maker fee rate (tiered)")->default_val(0.001);
    app.add_option("--taker-rate", o.taker_rate, "Taker fee rate (tiered)")->default_val(0.001);

    app.add_option("--symbol", o.symbol, "Trading symbol (e.g., BTCUSDT)");
    app.add_option("--stream", o.stream, "Stream type: trade, kline, or interval");
    app.add_option("--depth-stream", o.depth_stream,
                   "Optional L2 depth stream subscribed alongside --stream "
                   "on a single combined WebSocket. Example: depth20@100ms. "
                   "When set, the provider's orderbook_registry_ is driven "
                   "by real exchange levels (LocalBookAdapter in shadow "
                   "stops matching against paper-seeded MM liquidity).");
    app.add_option("--margin-type", o.margin_type,
        "(futures providers) Expected margin type for advisory "
        "comparison: ISOLATED or CROSSED. Empty = no check.");
    app.add_option("--liquidation-warn-pct", o.liquidation_warn_pct,
        "(futures providers) Warn at startup if any open position "
        "is within this fraction of liquidation. 0.05 = 5%. Set to 0 "
        "to disable. Default: 0.05.");
    app.add_flag("--margin-type-strict", o.margin_type_strict,
        "(futures providers) Escalate margin-mode mismatch from "
        "advisory (warning) to refusal. Requires --margin-type to be "
        "set; otherwise has no effect.");
    app.add_option("--max-notional", o.max_notional_usdt,
        "(futures providers) Pre-trade notional cap in USDT: "
        "|post_qty| * mark_price <= cap. 0 disables. Default: 0.");
    app.add_option("--max-leverage", o.max_leverage,
        "(futures providers) Pre-trade implied-leverage cap: "
        "post_notional / marked account equity <= cap. 0 disables. Default: 0.");
    app.add_option("--min-liq-distance-pct", o.min_liquidation_distance_pct,
        "(futures providers) Minimum projected post-trade buffer "
        "to liquidation, as a fraction (0.05 = 5%). 0 disables. "
        "Default: 0. See docs/operations/02-futures-testnet.md for caveats on the "
        "approximation.");
    app.add_option("--dead-man-countdown-ms", o.dead_man_countdown_ms,
        "(futures providers) Server-side dead-man's-switch countdown "
        "in milliseconds (Bitget UTA clamps to seconds [5,60]). "
        "On expiry the venue auto-cancels open orders: Binance futures "
        "is symbol-scoped; Bitget UTA is ACCOUNT-WIDE (all UTA open "
        "orders on the API key). 0 disables. Default: 30000 (30s).");
    app.add_option("--dead-man-heartbeat-ms", o.dead_man_heartbeat_ms,
        "(futures providers) Heartbeat interval for the dead-man's "
        "switch. Smaller = tighter recovery from server-side timer "
        "expiry; larger = fewer API calls and more tolerance for "
        "transient network jitter. Must stay strictly below countdown "
        "(Bitget also floors HB at 1000ms for the 1/s rate limit). "
        "0 means use countdown / 3. Default: 0.");
    app.add_flag("--disarm-deadman", o.disarm_deadman,
        "(futures providers) Don't arm the dead-man's switch for "
        "this run. Equivalent to --dead-man-countdown-ms 0 but "
        "intent-revealing for debug / deploy workflows where the "
        "operator deliberately wants to pause without venue-side "
        "auto-cancel.");

    app.add_option("--api-key", o.api_key, "API key for exchange");
    app.add_option("--api-secret", o.api_secret, "API secret for exchange");
    app.add_option("--api-passphrase", o.api_passphrase,
                   "API passphrase (Bitget and similar venues)");
    app.add_option("--host", o.host, "WebSocket host");
    app.add_option("--port", o.port, "Port number");
    app.add_option("--record", o.record_path, "Record market data to file");
    app.add_option("--replay-data", o.replay_data_path, "Replay recorded market data");
    app.add_flag("--live", o.live, "Safety flag for live (real money) mode");
    app.add_flag("--testnet", o.testnet,
                 "Sandbox routing: Binance → spot/futures testnet hosts; "
                 "Bitget → demo/paptrading (same as --demo)");
    app.add_flag("--demo", o.demo,
                 "Bitget demo/paptrading endpoints (also set by --testnet "
                 "when provider is bitget*)");

    app.add_option("--balance", o.balance, "Initial account balance")->default_val(10000.0);
    app.add_option("--risk-fraction", o.risk_fraction, "Position size as equity fraction")->default_val(0.02);
    app.add_option("--sl", o.sl_pct,
                   "Platform stop-loss fraction of entry (DefaultExitPolicy; 0=off)")
        ->default_val(0.003);
    app.add_option("--tp", o.tp_pct,
                   "Platform take-profit fraction of entry (DefaultExitPolicy; 0=off)")
        ->default_val(0.01);
    app.add_option("--exit-policy", o.exit_policy,
                   "Platform exit policy: floor (default, ensure SL/TP), strategy_only, "
                   "engine_only, union")
        ->default_val("floor");

    app.add_option("--checkpoint", o.checkpoint_path,
                   "Write periodic portfolio checkpoint to this binary file");
    app.add_option("--checkpoint-interval", o.checkpoint_interval,
                   "Write a checkpoint every N events (default: 10000)")
       ->default_val(10000);
    app.add_option("--resume", o.resume_path,
                   "Unavailable: v1 checkpoints are diagnostic-only and cannot be resumed");

    app.add_option("--backfill", o.backfill, "Historical bars to fetch before streaming")->default_val(500);
    app.add_option("--backfill-interval", o.backfill_interval, "Kline interval for backfill");

    app.add_option("--aggression", o.market_aggression, "Market order aggression factor")->default_val(1.1);
    app.add_option("--qty-scale", o.qty_scale, "Quantity scale (fractional → integer)")->default_val(1e8);
    app.add_option("--fill-rng-seed", o.fill_rng_seed, "RNG seed for fill model")->default_val(42);
    app.add_option("--spread-step", o.spread_step, "Spread step factor (mid * factor)")->default_val(0.0001);

    app.add_flag("--debug-fills", o.debug_fills,
                 "Log the first N fills via LocalBookAdapter with book state (intended/book/fill, best bid/ask)");
    app.add_option("--debug-fills-budget", o.debug_fills_budget,
                   "How many fills to log when --debug-fills is on")->default_val(20);

    app.add_option("--config", o.config_file, "Load configuration from JSON file");
    app.add_flag("--dump-config", o.dump_config_flag, "Print resolved config as JSON and exit");

    app.add_flag("--dry-run", o.dry_run, "Validate config, print summary, and exit");

    app.add_option("--rolling-window", o.rolling_window, "Rolling metrics window size (bars)")->default_val(252);
    app.add_option("--risk-free-rate", o.risk_free_rate, "Annual risk-free rate for Sharpe/Sortino")->default_val(0.0);
    app.add_option("--periods-per-year", o.periods_per_year,
                   "Bars per year for Sharpe/Sortino/Calmar annualization "
                   "(252 daily, 252*390 US-equities minute, 525600 crypto minute, 31536000 crypto second)")
        ->default_val(525600);
    app.add_option("--max-equity-points", o.max_equity_points,
                   "Max points retained in equity/benchmark curves before decimation")
        ->default_val(100000);
    app.add_option("--exec-bar-delay", o.exec_bar_delay,
                   "Same-symbol price events of simulated execution delay for "
                   "backtest/shadow. 1 (default) parks orders until the next "
                   "observation for that symbol and prevents same-bar execution. "
                   "0 disables. Ignored when --mode live or a latency model is configured.")
        ->default_val(1);
    app.add_option("--wire-latency-us", o.wire_latency_us,
                   "Extra wire + exchange-ingest latency (microseconds) applied by the "
                   "execution adapter on top of any engine-side latency model. "
                   "Shadow mode: gates real trade prints matching open orders. "
                   "Backtest with paper-hybrid exec: delays fill visibility by this window. "
                   "0 (default) = none.")
        ->default_val(0);
    app.add_option("--order-latency-us", o.order_latency_us,
                   "Strategy → order-eligible delay (microseconds). The engine routes "
                   "orders through pending_orders_ with bumped earliest_eligible_ts and "
                   "releases them on a bar/tick crossing that ts. With stddev=0 a fixed "
                   "delay is used; with stddev>0 a normal distribution is sampled. "
                   "Bar-mode caveat: latencies smaller than the bar interval are invisible. "
                   "0 (default) = none. Ignored in --mode live.")
        ->default_val(0);
    app.add_option("--order-latency-stddev-us", o.order_latency_stddev_us,
                   "Standard deviation in microseconds for stochastic order latency. "
                   "Requires --order-latency-us > 0. 0 (default) = fixed delay.")
        ->default_val(0);
    app.add_option("--impact-k-bps", o.impact_k_bps,
                   "Square-root market impact coefficient in basis points. Effective "
                   "price = ref × (1 ± k·sqrt(qty/adv)/1e4). Typical crypto k ~ O(10) "
                   "for qty ≈ 1% of ADV. Requires --impact-adv > 0. 0 (default) = no "
                   "impact applied. Ignored in --mode live.")
        ->default_val(0.0);
    app.add_option("--impact-adv", o.impact_adv,
                   "Reference average daily volume (in base units, same scale as order "
                   "qty) used as the denominator for the impact model. Set to a per-"
                   "symbol value calibrated from prod tape. Required when --impact-k-bps "
                   "is set; 0 (default) disables.")
        ->default_val(0.0);
    app.add_flag("--realistic-fills", o.realistic_fills,
                 "DEPRECATED, no effect: passive-side fill pricing is always on — fills "
                 "record the resting counterparty's price, one event per walked level. "
                 "The flag is accepted with a warning so existing scripts keep running.");
    app.add_option("--bar-spread-bps", o.bar_spread_bps,
                   "DEPRECATED, no effect: recorded fill prices always come from the "
                   "resting book, so a parametric spread shift no longer applies. "
                   "Calibrate --mm-spread-pct instead. Accepted with a warning.")
        ->default_val(0.0);
    app.add_option("--fill-prob", o.fill_prob,
                   "Base fill probability for limit orders at the mid (RealisticFillModel). "
                   "Each limit submit is accepted with probability prob × exp(-decay × "
                   "distance-from-mid); rejected submits never reach the book — models "
                   "adverse selection / being skipped in the queue. 0 (default) = model "
                   "off, perfect fills. Ignored in --mode live.")
        ->default_val(0.0);
    app.add_option("--fill-fade", o.fill_fade,
                   "Fade rate: fraction shaved off every fill's quantity (applies to "
                   "market fills too). Requires --fill-prob > 0. 0 (default) = no fade.")
        ->default_val(0.0);
    app.add_option("--fill-decay", o.fill_decay,
                   "Exponential decay of fill probability with relative distance from "
                   "mid. Only meaningful with --fill-prob > 0.")
        ->default_val(10.0);
    app.add_option("--mm-levels", o.mm_levels,
                   "Synthetic MarketMaker book: price levels seeded per side in bar-mode "
                   "backtests. The seeded book is the sole source of spread cost for "
                   "taker fills — calibrate to the target market.")
        ->default_val(10);
    app.add_option("--mm-base-depth", o.mm_base_depth,
                   "Synthetic MarketMaker book: depth at level 1 (base units); level i "
                   "rests base_depth × i.")
        ->default_val(100);
    app.add_option("--mm-spread-pct", o.mm_spread_pct,
                   "Synthetic MarketMaker book: per-level half-spread as a fraction "
                   "(0.002 = 20 bps); level i rests at mid × (1 ± i × (spread_pct + "
                   "vol × --mm-vol-mult)).")
        ->default_val(0.002);
    app.add_option("--mm-vol-mult", o.mm_vol_mult,
                   "Synthetic MarketMaker book: multiplier widening the seeded spread "
                   "with realized volatility over the trailing window. Real spreads run "
                   "at a few percent OF bar vol; values > 1 inflate the book until "
                   "nothing fills.")
        ->default_val(0.25);
    app.add_option("--mm-max-spread-pct", o.mm_max_spread_pct,
                   "Cap on the vol-widened per-level half-spread (fraction). Prevents "
                   "one large bar return (gap) from widening the seeded book beyond "
                   "every crossing limit.")
        ->default_val(0.05);
    app.add_flag("--walked-book-impact", o.walked_book_impact,
                 "On L2-seeded symbols, price market orders against the actual VWAP of "
                 "the levels they'd walk instead of mid + --impact-k-bps. Walked depth is "
                 "deterministic for sub-book clips; the parametric model is suppressed for "
                 "those submits. Falls back to mid + impact_model when the symbol has no "
                 "L2 depth or the qty exceeds the book. Default off - byte-identical until "
                 "set. Ignored in --mode live.");
    app.add_option("--queue-model", o.queue_model,
                   "Shadow-mode queue-position estimate. 'none' (default) keeps the "
                   "legacy fill-on-cross behaviour. 'l2-snapshot' samples depth at our "
                   "limit price at submit time and only fills once the queue has been "
                   "consumed by subsequent prints; requires --depth-stream. Ignored in "
                   "--mode live.")
        ->default_val("none")
        ->check(CLI::IsMember({"none", "l2-snapshot"}));
    app.add_option("--maker-queue-model", o.maker_queue_model,
                   "Maker queue model for paper/backtest limit orders. 'none' (default) = "
                   "LocalBook only. 'uniform'/'front'/'back' use HybridPaper + cancel attribution. "
                   "--depth-stream optional: without it QueueAware refuses front-of-queue "
                   "tape fills (conservative); bar [low,high] sweep still fills limits. "
                   "Ignored in --mode live.")
        ->default_val("none")
        ->check(CLI::IsMember({"none", "uniform", "front", "back"}));
    app.add_option("--instrument", o.instrument_specs,
                   "Per-symbol trading rules. Repeatable. Format: "
                   "SYMBOL:tick=X,lot=Y,minq=Q,minn=N,maker=M,taker=T "
                   "(any field optional). Used to quantize prices/qtys and reject "
                   "orders below min_qty or min_notional before they reach the adapter.");
    app.add_option("--reconcile-tolerance-bps", o.reconcile_tolerance_bps,
                   "Acceptable local/exchange divergence (basis points) on startup "
                   "reconciliation. Live mode only. Engine refuses to start on larger drift.")
        ->default_val(10.0);
    app.add_option("--kill-switch-deadline-ms", o.kill_switch_deadline_ms,
                   "Deadline for cancel-all + flatten on shutdown. Live mode only. "
                   "A missed deadline prints a warning so operators know to intervene.")
        ->default_val(5000);

    // F-05b (docs/todos/11-F-forensic-lifecycle-audit.md): a spot account
    // cannot borrow, but nothing in the engine said so — the audit accepted
    // $186,325 of notional on $10,000 of equity (18.6x), later 68.3x.
    app.add_option("--max-gross-leverage", o.risk_max_gross_leverage,
                   "Portfolio-wide cap on gross mark-to-market exposure as a multiple of "
                   "account equity (1.0 = cash account, cannot borrow; 0 = disabled). "
                   "Defaults to 1.0 on spot venues and to disabled on futures venues, "
                   "where --max-leverage / FuturesRiskCheck owns leverage instead. "
                   "Distinct from --max-leverage, which is a per-order futures cap.")
        ->default_val(-1.0);

    app.add_option("--max-daily-loss", o.risk_max_daily_loss, "Maximum daily loss before halt (0 = no limit)")->default_val(0.0);

    app.add_option("--daily-reset-hour", o.risk_daily_reset_hour, "UTC hour (0-23) to reset daily loss counter")->default_val(0);
    app.add_option("--max-trades-per-hour", o.risk_max_trades_per_hour, "Maximum fills per rolling hour (0 = no limit)")->default_val(0);
    app.add_option("--max-orders-per-minute", o.risk_max_orders_per_minute, "Maximum orders per rolling minute (0 = no limit)")->default_val(0);
    // R3 (authoritative risk accounting). The inventory cap is applied to the
    // worst case: position + every open order on the increasing side + the
    // candidate, so resting orders cannot walk past it one ack at a time.
    app.add_option("--max-inventory-qty", o.risk_max_inventory_qty,
                   "Hard per-symbol inventory cap in base quantity, worst-case incl. open orders (0 = no limit)")->default_val(0.0);
    app.add_option("--max-funding-8h-rate", o.risk_max_funding_8h_rate,
                   "Funding circuit breaker as a fraction of notional per 8h, e.g. 0.0005 (0 = no limit)")->default_val(0.0);
    app.add_option("--max-mark-age-ms", o.risk_max_mark_age_ms,
                   "Age above which a mark counts as stale for risk (0 = no staleness classification)")->default_val(0);
    app.add_flag("--risk-require-fresh-mark", o.risk_require_fresh_mark,
                 "Refuse inventory-increasing orders when the mark is stale or missing (reductions stay allowed)");
    app.add_flag("--risk-soft-limits,!--no-risk-soft-limits", o.risk_soft_portfolio_limits,
                 "Enable soft risk limits in research/backtest (reject/log on breach instead of terminal engine halt)");
    app.add_flag("--risk-unwind", o.risk_unwind, "On risk halt, unwind all positions before stopping");
    app.add_option("--output", o.output, "Write results to file (default: stdout)");
    app.add_option("--output-format", o.output_format, "Output format: json or csv")->default_val("json");

    app.add_option("--status-format", o.status_format,
                   "Live status display: auto, tui, plain, ndjson, off (default: auto)")
        ->default_val("auto");
    app.add_flag("--no-tui", o.no_tui, "Shortcut for --status-format=plain");
    app.add_flag("--simple-tui", o.simple_tui,
                 "On engine_shadow/engine_live, use ConsoleDashboard's own "
                 "ncurses-free ANSI-box status display instead of the rich "
                 "tabbed ncurses dashboard. No effect on engine_backtest "
                 "(rich TUI isn't compiled there) or with --status-format "
                 "other than tui/auto-resolved-to-tui.");

#ifdef HAS_IMGUI_DESK
    app.add_flag("--desk", o.desk,
                 "Open the ImGui strategy desk (Monitor/Execute; personal tool)");
    app.add_flag("--desk-demo-data", o.desk_demo_data,
                 "Start the desk with deterministic DEMO DATA research panels "
                 "already enabled (headless visual QA / manual smoke)");
    app.add_flag("--no-footprint", o.no_footprint,
                 "Disable footprint public-trade collection (auto-enabled "
                 "with --desk; never affects trading behavior either way)");
    app.add_option("--footprint-tick-size", o.footprint_tick_size,
                   "Exact decimal tick-size override for the footprint panel "
                   "(e.g. \"0.01\") - only used when official instrument "
                   "metadata disagrees or is unavailable");
#endif

#ifdef HAS_WEB
    app.add_flag("--web", o.web, "Serve the web UI (read-only) for this session");
    app.add_option("--web-port", o.web_port, "Web UI port (default: 8080)")->default_val(8080);
    app.add_option("--web-bind", o.web_bind, "Web UI bind address (default: 127.0.0.1)")
        ->default_val("127.0.0.1");
    app.add_option("--web-token", o.web_token, "Optional bearer token required for web UI access");
    app.add_option("--web-assets", o.web_assets,
                   "Directory of built web UI assets to serve at / (default: API/WS only)");
#endif

#ifdef HAS_QUESTDB
    app.add_flag("--persist", o.persist,
                 "Write every order lifecycle event to QuestDB for this session");
    app.add_option("--run-tag", o.run_tag,
                   "Table prefix for this run (auto-generated if omitted)");
    app.add_option("--run-notes", o.run_notes,
                   "Free-form note stored with the run metadata");
    app.add_option("--questdb-host", o.questdb_host,
                   "QuestDB host (default 127.0.0.1)")
        ->default_val("127.0.0.1");
    app.add_option("--questdb-ilp-port", o.questdb_ilp_port,
                   "QuestDB ILP ingest port (default 9009)")
        ->default_val(9009);
    app.add_option("--questdb-http-port", o.questdb_http_port,
                   "QuestDB HTTP/DDL port (default 9000)")
        ->default_val(9000);
    app.add_option("--questdb-flush-ms", o.questdb_flush_ms,
                   "QuestDB ILP time-based flush cadence in milliseconds (default 150). "
                   "Lower values reduce risk of lost rows on quiet periods or crashes.")
        ->default_val(150);
    app.add_flag("--persist-strict", o.questdb_strict,
                 "Phase 2: Make QuestDB persistence strict. Startup or persistent ILP failures "
                 "will hard-fail the run (instead of soft-disabling). Enables local ILP fallback file.");
#endif
}

static void apply_config_overlay(CLI::App& app, cli_options& opts)
{
    if (opts.config_file.empty()) return;

    cli_options file_opts;
    load_config_file(opts.config_file, file_opts);

    auto was_set = [&](const std::string& name) -> bool {
        auto opt = app.get_option_no_throw(name);
        return opt && opt->count() > 0;
    };

    if (!was_set("--replay")) opts.replay_path = file_opts.replay_path;
    if (!was_set("--log-events")) opts.event_log_path = file_opts.event_log_path;
    if (!was_set("--write-run-manifest"))
        opts.write_run_manifest_path = file_opts.write_run_manifest_path;
    if (!was_set("--replay-run-manifest"))
        opts.replay_run_manifest_path = file_opts.replay_run_manifest_path;
    if (!was_set("--artifacts-dir"))
        opts.reproducibility_artifacts_directory =
            file_opts.reproducibility_artifacts_directory;
    if (!was_set("--verify-hashes"))
        opts.verify_hashes = file_opts.verify_hashes;
    if (!was_set("--allow-dataset-mismatch"))
        opts.allow_dataset_mismatch = file_opts.allow_dataset_mismatch;
    if (!was_set("--seed")) {
        opts.seed = file_opts.seed;
        opts.seed_explicitly_set = file_opts.seed_explicitly_set;
    }
    if (!was_set("--thread-preset")) opts.thread_preset_str = file_opts.thread_preset_str;
    if (!was_set("--preset")) opts.preset = file_opts.preset;
    if (!was_set("--no-pin")) opts.no_pin = file_opts.no_pin;
    if (!was_set("--provider")) opts.provider_name = file_opts.provider_name;
    if (!was_set("--path")) opts.provider_path = file_opts.provider_path;
    if (!was_set("--monte-carlo")) opts.monte_carlo = file_opts.monte_carlo;
    if (!was_set("--mc-trials")) {
        opts.mc_trials = file_opts.mc_trials;
        opts.mc_trials_explicitly_set =
            file_opts.mc_trials_explicitly_set;
    }
    if (!was_set("--mc-model")) {
        opts.mc_model = file_opts.mc_model;
        opts.mc_model_explicitly_set =
            file_opts.mc_model_explicitly_set;
    }
    if (!was_set("--mc-params")) {
        opts.mc_params = file_opts.mc_params;
        opts.mc_params_explicitly_set =
            file_opts.mc_params_explicitly_set;
    }
    if (!was_set("--mc-parallel")) opts.mc_parallel = file_opts.mc_parallel;
    if (!was_set("--mc-workers")) {
        opts.mc_workers = file_opts.mc_workers;
        opts.mc_workers_explicitly_set =
            file_opts.mc_workers_explicitly_set;
    }
    if (!was_set("--mc-reuse-objects"))
        opts.mc_reuse_objects = file_opts.mc_reuse_objects;
    if (!was_set("--strategy")) opts.strategy = file_opts.strategy;
    if (!was_set("--param")) opts.params = file_opts.params;
    if (!was_set("--format")) opts.format = file_opts.format;
    if (!was_set("--sma-period")) opts.sma_period = file_opts.sma_period;
    if (!was_set("--fee")) {
        opts.fee_model = file_opts.fee_model;
        opts.fee_model_explicit = file_opts.fee_model_explicit;
    }
    if (!was_set("--fee-value")) opts.fee_value = file_opts.fee_value;
    if (!was_set("--maker-rate")) {
        opts.maker_rate = file_opts.maker_rate;
        opts.maker_rate_explicit = file_opts.maker_rate_explicit;
    }
    if (!was_set("--taker-rate")) {
        opts.taker_rate = file_opts.taker_rate;
        opts.taker_rate_explicit = file_opts.taker_rate_explicit;
    }
    if (!was_set("--symbol")) opts.symbol = file_opts.symbol;
    if (!was_set("--stream")) opts.stream = file_opts.stream;
    if (!was_set("--api-key")) opts.api_key = file_opts.api_key;
    if (!was_set("--api-secret")) opts.api_secret = file_opts.api_secret;
    if (!was_set("--api-passphrase")) opts.api_passphrase = file_opts.api_passphrase;
    if (!was_set("--host")) opts.host = file_opts.host;
    if (!was_set("--port")) opts.port = file_opts.port;
    if (!was_set("--record")) opts.record_path = file_opts.record_path;
    if (!was_set("--replay-data")) opts.replay_data_path = file_opts.replay_data_path;
    if (!was_set("--live")) opts.live = file_opts.live;
    if (!was_set("--testnet")) opts.testnet = file_opts.testnet;
    if (!was_set("--demo")) opts.demo = file_opts.demo;
    if (!was_set("--mode")) opts.mode = file_opts.mode;
    if (!was_set("--balance")) opts.balance = file_opts.balance;
    if (!was_set("--risk-fraction")) opts.risk_fraction = file_opts.risk_fraction;
    if (!was_set("--sl")) opts.sl_pct = file_opts.sl_pct;
    if (!was_set("--tp")) opts.tp_pct = file_opts.tp_pct;
    if (!was_set("--exit-policy")) opts.exit_policy = file_opts.exit_policy;
    if (!was_set("--backfill")) opts.backfill = file_opts.backfill;
    if (!was_set("--backfill-interval")) opts.backfill_interval = file_opts.backfill_interval;
    if (!was_set("--aggression"))
        opts.market_aggression = file_opts.market_aggression;
    if (!was_set("--qty-scale")) opts.qty_scale = file_opts.qty_scale;
    if (!was_set("--fill-rng-seed")) {
        opts.fill_rng_seed = file_opts.fill_rng_seed;
        opts.fill_rng_seed_explicitly_set =
            file_opts.fill_rng_seed_explicitly_set;
    }
    if (!was_set("--spread-step")) {
        opts.spread_step = file_opts.spread_step;
        opts.spread_step_explicitly_set =
            file_opts.spread_step_explicitly_set;
    }
    if (!was_set("--rolling-window")) opts.rolling_window = file_opts.rolling_window;
    if (!was_set("--risk-free-rate")) opts.risk_free_rate = file_opts.risk_free_rate;
    if (!was_set("--periods-per-year")) {
        opts.periods_per_year = file_opts.periods_per_year;
        opts.periods_per_year_explicit = file_opts.periods_per_year_explicit;
    }
    if (!was_set("--max-equity-points")) opts.max_equity_points = file_opts.max_equity_points;
    if (!was_set("--exec-bar-delay")) opts.exec_bar_delay = file_opts.exec_bar_delay;
    if (!was_set("--wire-latency-us")) opts.wire_latency_us = file_opts.wire_latency_us;
    if (!was_set("--order-latency-us")) opts.order_latency_us = file_opts.order_latency_us;
    if (!was_set("--order-latency-stddev-us"))
        opts.order_latency_stddev_us = file_opts.order_latency_stddev_us;
    if (!was_set("--impact-k-bps")) opts.impact_k_bps = file_opts.impact_k_bps;
    if (!was_set("--impact-adv")) opts.impact_adv = file_opts.impact_adv;
    if (!was_set("--realistic-fills")) opts.realistic_fills = file_opts.realistic_fills;
    if (!was_set("--bar-spread-bps")) opts.bar_spread_bps = file_opts.bar_spread_bps;
    if (!was_set("--walked-book-impact"))
        opts.walked_book_impact = file_opts.walked_book_impact;
    if (!was_set("--fill-prob")) opts.fill_prob = file_opts.fill_prob;
    if (!was_set("--fill-fade")) opts.fill_fade = file_opts.fill_fade;
    if (!was_set("--fill-decay")) opts.fill_decay = file_opts.fill_decay;
    if (!was_set("--mm-levels")) opts.mm_levels = file_opts.mm_levels;
    if (!was_set("--mm-base-depth")) opts.mm_base_depth = file_opts.mm_base_depth;
    if (!was_set("--mm-spread-pct")) opts.mm_spread_pct = file_opts.mm_spread_pct;
    if (!was_set("--mm-vol-mult")) opts.mm_vol_mult = file_opts.mm_vol_mult;
    if (!was_set("--mm-max-spread-pct"))
        opts.mm_max_spread_pct = file_opts.mm_max_spread_pct;
    if (!was_set("--queue-model")) opts.queue_model = file_opts.queue_model;
    if (!was_set("--maker-queue-model")) opts.maker_queue_model = file_opts.maker_queue_model;
    if (!was_set("--instrument"))
        opts.instrument_specs = file_opts.instrument_specs;
    if (!was_set("--output")) opts.output = file_opts.output;
    if (!was_set("--output-format")) opts.output_format = file_opts.output_format;

    opts.risk_max_position_value = file_opts.risk_max_position_value;
    opts.risk_max_drawdown = file_opts.risk_max_drawdown;
    opts.risk_max_loss_per_trade = file_opts.risk_max_loss_per_trade;
    opts.risk_max_open_orders = file_opts.risk_max_open_orders;
    opts.risk_max_portfolio_exposure = file_opts.risk_max_portfolio_exposure;
    if (!was_set("--max-daily-loss")) opts.risk_max_daily_loss = file_opts.risk_max_daily_loss;
    if (!was_set("--daily-reset-hour")) opts.risk_daily_reset_hour = file_opts.risk_daily_reset_hour;
    if (!was_set("--max-trades-per-hour")) opts.risk_max_trades_per_hour = file_opts.risk_max_trades_per_hour;
    if (!was_set("--max-orders-per-minute")) opts.risk_max_orders_per_minute = file_opts.risk_max_orders_per_minute;
    if (!was_set("--max-inventory-qty")) opts.risk_max_inventory_qty = file_opts.risk_max_inventory_qty;
    if (!was_set("--max-funding-8h-rate")) opts.risk_max_funding_8h_rate = file_opts.risk_max_funding_8h_rate;
    if (!was_set("--max-gross-leverage"))
        opts.risk_max_gross_leverage = file_opts.risk_max_gross_leverage;
    if (!was_set("--max-mark-age-ms")) opts.risk_max_mark_age_ms = file_opts.risk_max_mark_age_ms;
    if (!was_set("--risk-require-fresh-mark")) opts.risk_require_fresh_mark = file_opts.risk_require_fresh_mark;
    if (!was_set("--risk-unwind")) opts.risk_unwind = file_opts.risk_unwind;
    if (!was_set("--risk-soft-limits") && !was_set("--no-risk-soft-limits")) opts.risk_soft_portfolio_limits = file_opts.risk_soft_portfolio_limits;
#ifdef HAS_QUESTDB
    if (!was_set("--persist")) opts.persist = file_opts.persist;
    if (!was_set("--run-tag")) opts.run_tag = file_opts.run_tag;
    if (!was_set("--run-notes")) opts.run_notes = file_opts.run_notes;
    if (!was_set("--questdb-host")) opts.questdb_host = file_opts.questdb_host;
    if (!was_set("--questdb-ilp-port")) opts.questdb_ilp_port = file_opts.questdb_ilp_port;
    if (!was_set("--questdb-http-port")) opts.questdb_http_port = file_opts.questdb_http_port;
    if (!was_set("--questdb-flush-ms")) opts.questdb_flush_ms = file_opts.questdb_flush_ms;
    if (!was_set("--persist-strict")) opts.questdb_strict = file_opts.questdb_strict;
#endif
}

// -----------------------------------------------------------------------------
// Preset application (P1-01 / P1-02; see docs/todos/02-P1-freeze.md#P1-01 or docs/governance/03-todo.md)
// Applies a named bundle only to fields the user did not explicitly supply on
// the CLI (using the same was_set(app) test as JSON overlay). Establishes:
//   explicit CLI > --config JSON > --preset bundle > hard defaults.
// -----------------------------------------------------------------------------
static void apply_preset_with_precedence(CLI::App& app, cli_options& opts)
{
    auto was_set = [&](const std::string& name) -> bool {
        auto opt = app.get_option_no_throw(name);
        return opt && opt->count() > 0;
    };

    const std::string& name = opts.preset;

    // futures-phase0 / phase0: hardened tiny-size mainnet futures template
    // (prod.md + scripts/phase0/new-session.sh). User still supplies
    // --provider binance-futures, --symbol, --stream/--depth-stream, --live + keys.
    if (name == "futures-phase0" || name == "phase0" || name == "futures-p0") {
        if (!was_set("--reconcile-tolerance-bps")) opts.reconcile_tolerance_bps = 3.0;
        if (!was_set("--dead-man-countdown-ms"))   opts.dead_man_countdown_ms = 30000;
        if (!was_set("--dead-man-heartbeat-ms"))   opts.dead_man_heartbeat_ms = 8000;
        if (!was_set("--max-notional"))            opts.max_notional_usdt = 15000.0;
        if (!was_set("--max-leverage"))            opts.max_leverage = 2.5;
        if (!was_set("--min-liq-distance-pct"))    opts.min_liquidation_distance_pct = 0.07;
        if (!was_set("--max-daily-loss"))          opts.risk_max_daily_loss = 80.0;
        if (!was_set("--risk-unwind"))             opts.risk_unwind = true;
        return;
    }

    // mc-robustness: sensible defaults for MC research campaigns.
    // Pair with --thread-preset inline (as recommended in MC docs/warnings).
    if (name == "mc-robustness" || name == "mc" || name == "monte-carlo") {
        if (!was_set("--mc-model")) opts.mc_model = "gbm";
        if (!was_set("--mc-reuse-objects")) opts.mc_reuse_objects = true;
        // trials and mc-params are intentionally left to the caller (common
        // values 100/300/500 + asset-specific mu/sigma/n_steps).
        return;
    }

    // backtest-local-l2: L2-aware realism bundle for local (tick) data when
    // depth is also supplied or synthesized.
    if (name == "backtest-local-l2" || name == "backtest-l2" || name == "local-l2") {
        // (passive-side fill pricing is always on; no flag needed anymore)
        if (!was_set("--queue-model"))        opts.queue_model = "l2-snapshot";
        if (!was_set("--maker-queue-model"))  opts.maker_queue_model = "uniform";
        if (!was_set("--walked-book-impact")) opts.walked_book_impact = true;
        return;
    }

    // shadow-tape: reasonable base for divergence-aware shadow runs.
    if (name == "shadow-tape" || name == "shadow" || name == "paper") {
        if (!was_set("--reconcile-tolerance-bps")) opts.reconcile_tolerance_bps = 5.0;
        return;
    }

    // Unknown / future preset names are tolerated (run continues with
    // whatever other flags were given). A warning can be added at call site
    // or in dry-run if desired.
}

// Shared by the provider-config and plain-config paths: fill-model wiring,
// synthetic-MM calibration, and deprecation warnings for the retired
// fill-pricing flags. Returns non-zero on validation failure.
static int apply_sim_realism(engine_config& cfg, const cli_options& o, bool is_live)
{
    if (o.realistic_fills)
        std::cerr << "[warn] --realistic-fills is deprecated and has no effect: "
                     "passive-side fill pricing is always on\n";
    if (o.bar_spread_bps > 0.0)
        std::cerr << "[warn] --bar-spread-bps no longer affects recorded fill "
                     "prices; calibrate --mm-spread-pct instead\n";

    if (o.fill_fade > 0.0 && !(o.fill_prob > 0.0)) {
        std::cerr << "[error] --fill-fade requires --fill-prob > 0\n";
        return 1;
    }
    if (o.fill_prob > 0.0) {
        if (is_live) {
            std::cerr << "[warn] --fill-prob ignored in live mode\n";
        } else {
            cfg.fill_model = std::make_shared<RealisticFillModel>(
                o.fill_fade, o.fill_prob, o.fill_decay);
        }
    }

    if (o.mm_levels <= 0 || o.mm_base_depth <= 0 || !(o.mm_spread_pct > 0.0) ||
        !(o.mm_max_spread_pct > 0.0)) {
        std::cerr << "[error] --mm-levels, --mm-base-depth, --mm-spread-pct and "
                     "--mm-max-spread-pct must be > 0\n";
        return 1;
    }
    cfg.mm_levels_per_side = o.mm_levels;
    cfg.mm_base_depth = o.mm_base_depth;
    cfg.mm_base_spread_pct = o.mm_spread_pct;
    cfg.mm_vol_spread_mult = o.mm_vol_mult;
    cfg.mm_max_half_spread_pct = o.mm_max_spread_pct;
    return 0;
}

static bool validate_fee_configuration(const cli_options& o, std::ostream& err)
{
    if (o.fee_model != "zero" && o.fee_model != "fixed" && o.fee_model != "tiered")
    {
        err << "  ! Unknown fee model: " << o.fee_model << "\n";
        return false;
    }
    if (o.fee_model == "fixed" &&
        (!std::isfinite(o.fee_value) || !(o.fee_value > 0.0)))
    {
        err << "  ! --fee fixed requires a finite --fee-value > 0; "
               "use --fee zero for an intentional zero-cost research run.\n";
        return false;
    }
    if (o.fee_model == "tiered" &&
        (!std::isfinite(o.maker_rate) || !std::isfinite(o.taker_rate) ||
         o.maker_rate < 0.0 || o.taker_rate < 0.0))
    {
        err << "  ! Tiered fee rates must be finite and non-negative.\n";
        return false;
    }
    if (o.periods_per_year == 0)
    {
        err << "  ! --periods-per-year must be positive.\n";
        return false;
    }

    const bool protected_mode = o.live || o.mode == "live" || o.mode == "shadow";
    if (!protected_mode) return true;

    if (o.fee_model == "zero")
    {
        err << "  ! --fee zero is research-only and is refused in shadow/live mode.\n";
        return false;
    }
    if (!o.fee_model_explicit)
    {
        err << "  ! Shadow/live requires an explicit fee model and venue fee schedule; "
               "the backtest default is not venue-authoritative.\n";
        return false;
    }
    if (o.fee_model == "tiered" &&
        (!o.maker_rate_explicit || !o.taker_rate_explicit ||
         !(o.maker_rate > 0.0) || !(o.taker_rate > 0.0)))
    {
        err << "  ! Shadow/live tiered fees require explicit positive --maker-rate "
               "and --taker-rate values from the venue schedule.\n";
        return false;
    }

    // A provider can change or aggregate stream cadence. Never annualize a
    // protected shadow/live record using the backtest default: the operator
    // must state the intended sampling basis for every protected stream.
    if (!o.periods_per_year_explicit)
    {
        err << "  ! Shadow/live requires explicit --periods-per-year; "
               "the backtest 1-minute default is not venue-authoritative.\n";
        return false;
    }
    return true;
}

static int run_dry_run(const cli_options& o)
{
    // Without L2, QueueAware joins conservatively (no front-of-queue on tape);
    // bar [low,high] sweep still fills passive limits.
    if (o.maker_queue_model != "none" && o.depth_stream.empty()) {
        std::cerr << "[warn] --maker-queue-model " << o.maker_queue_model
                  << " without --depth-stream: size_ahead is infinite without L2 "
                     "(no optimistic join-front); bar range sweep still fills limits\n";
    }

    std::string resolved_strategy = o.strategy.empty() ? "mean-reversion" : o.strategy;
    std::string resolved_mode = o.mode.empty() ? TT_DEFAULT_MODE : o.mode;
    std::string resolved_preset = o.thread_preset_str.empty()
        ? preset_to_string(select_preset(detect_physical_cores()))
        : o.thread_preset_str;

    std::cout << "\n";
    std::cout << "  ============================================\n";
    std::cout << "    Dry-run config validation\n";
    std::cout << "  ============================================\n";
    std::cout << "    Mode:       " << resolved_mode << "\n";
    std::cout << "    Strategy:   " << resolved_strategy << "\n";
    std::cout << "    SMA Period: " << o.sma_period << "\n";
    std::cout << "    Balance:    $" << o.balance << "\n";
    std::cout << "    Risk:       " << (o.risk_fraction * 100) << "% per trade\n";
    std::cout << "    SL/TP:      " << (o.sl_pct * 100) << "% / " << (o.tp_pct * 100)
              << "%  (policy=" << o.exit_policy << ")\n";
    std::string resolved_spin = o.spin_policy_str.empty() ? "adaptive" : o.spin_policy_str;
    print_threading_details(std::cout, "    ",
                            string_to_preset(resolved_preset),
                            string_to_spin_policy(resolved_spin));
    std::cout << "    Provider:   " << (o.provider_name.empty() ? "(TUI)" : o.provider_name) << "\n";
    std::cout << "    Data path:  " << (o.provider_path.empty() ? "(none)" : o.provider_path) << "\n";
    std::cout << "    Fee model:  " << (o.fee_model.empty() ? "zero" : o.fee_model) << "\n";
    std::cout << "    Seed:       "
              << (o.seed_explicitly_set ? std::to_string(o.seed)
                                        : std::string("<missing>"))
              << "\n";
    std::cout << "  ── Risk Limits ──\n";
    std::cout << "    Max drawdown:        " << (o.risk_max_drawdown * 100) << "%\n";
    std::cout << "    Max position value:  $" << o.risk_max_position_value << "\n";
    std::cout << "    Max loss/trade:      $" << o.risk_max_loss_per_trade << "\n";
    std::cout << "    Max open orders:     " << o.risk_max_open_orders << "\n";
    std::cout << "    Max exposure:        $" << o.risk_max_portfolio_exposure << "\n";
    if (o.risk_max_daily_loss > 0.0)
        std::cout << "    Max daily loss:      $" << o.risk_max_daily_loss << " (reset at " << o.risk_daily_reset_hour << ":00 UTC)\n";
    if (o.risk_max_trades_per_hour > 0)
        std::cout << "    Max trades/hour:     " << o.risk_max_trades_per_hour << "\n";
    if (o.risk_max_orders_per_minute > 0)
        std::cout << "    Max orders/minute:   " << o.risk_max_orders_per_minute << "\n";
#ifdef HAS_QUESTDB
    std::cout << "    Persistence:    " << (o.persist ? "QuestDB" : "no") << "\n";
    if (o.persist)
    {
        std::cout << "    QuestDB host:   " << o.questdb_host << "\n";
        std::cout << "    ILP port:       " << o.questdb_ilp_port << "\n";
        std::cout << "    HTTP port:      " << o.questdb_http_port << "\n";
        std::cout << "    Run tag:        "
                  << (o.run_tag.empty() ? "(auto)" : o.run_tag) << "\n";
        std::cout << "    Flush cadence:  " << o.questdb_flush_ms << " ms\n";
        if (o.questdb_strict)
            std::cout << "    Strict mode:    ENABLED (hard-fail + fallback on problems)\n";
        if (!o.run_notes.empty())
            std::cout << "    Run notes:      " << o.run_notes << "\n";
    }
#endif
    std::cout << "  ============================================\n";

    bool valid = true;
    const bool resolved_live = (o.mode == "live" || o.live);
    if (!o.strategy.empty() && !StrategyRegistry::instance().has(o.strategy))
    {
        std::cerr << "  ! Unknown strategy: " << o.strategy << "\n";
        valid = false;
    }
    if (!o.mode.empty() && o.mode != "backtest" && o.mode != "shadow" && o.mode != "live")
    {
        std::cerr << "  ! Unknown mode: " << o.mode << "\n";
        valid = false;
    }
    if (resolved_live && !truetest::target_allows_live_orders())
    {
        std::cerr << "  ! Live mode is only permitted on the engine_live binary "
                     "(this is " << truetest::target_name() << ").\n";
        valid = false;
    }
    if (!validate_binance_futures_live_phase0(o, std::cerr, std::cout))
        valid = false;
    if (!validate_fee_configuration(o, std::cerr)) valid = false;
#if TT_TARGET == TT_TARGET_BACKTEST
    if (!o.seed_explicitly_set) {
        std::cerr << "  ! Backtest requires an explicit deterministic seed\n";
        valid = false;
    }
#endif
    if (!o.thread_preset_str.empty())
    {
        try { string_to_preset(o.thread_preset_str); }
        catch (...) {
            std::cerr << "  ! Unknown thread preset: " << o.thread_preset_str << "\n";
            valid = false;
        }
    }

    std::cout << "\n  Config is " << (valid ? "VALID" : "INVALID") << ".\n";
    return valid ? 0 : 1;
}

static int run_replay_mode(const cli_options& o)
{
    if (!o.event_log_path.empty())
        throw std::invalid_argument(
            "--log-events cannot be combined with authoritative --replay");

    std::cout << "\n  Replaying events from: " << o.replay_path << "\n";
    std::cout << "  Applying recorded orders, fills and funding as an authoritative ledger.\n";

    std::shared_ptr<IStrategy> strategy;

    engine_config cfg;
    cfg.seed = o.seed;
    cfg.seed_explicitly_set = o.seed_explicitly_set;
    cfg.initial_balance = o.balance;
    cfg.threading = thread_preset::inline_mode;
    cfg.compress_log = o.compress_log;
    cfg.log_max_bytes = o.log_max_size_mb * 1024ull * 1024ull;
    cfg.log_max_files = o.log_keep;

    auto dh = std::make_shared<data_handler>();
    // engine embeds multi-MiB ObjectPools — never stack-allocate (8 MiB default RLIMIT_STACK).
    auto eng = std::make_unique<engine>(dh, nullptr, strategy, std::move(cfg));
    eng->run_replay(o.replay_path, o.replay_from_us, o.replay_to_us);
    eng->print_summary();
    export_results(eng->get_analytics(), o.output, o.output_format);
    return eng->run_succeeded() ? 0 : 1;
}

static std::string reproducibility_utc_now()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (::gmtime_r(&now, &utc) == nullptr)
        throw std::runtime_error("cannot convert receipt timestamp to UTC");
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc)
        == 0)
        throw std::runtime_error("cannot format receipt timestamp");
    return buffer;
}

static std::filesystem::path default_reproducibility_artifact_root(
    const std::filesystem::path& manifest_path)
{
    const std::string name = manifest_path.stem().string() + ".artifacts";
    return manifest_path.parent_path() / name;
}

static std::string trial_hash_name(std::size_t trial_index,
                                   std::string_view kind)
{
    return "trial_" + std::to_string(trial_index) + "." + std::string(kind);
}

static std::map<std::string, std::string, std::less<>>
deterministic_campaign_hashes(
    const truetest::simulation::McAggregate& results,
    const truetest::simulation::McRunConfig& config)
{
    std::map<std::string, std::string, std::less<>> hashes{
        {"economic_result",
         truetest::simulation::economic_result_sha256(results)},
        {"event_log",
         truetest::simulation::aggregate_event_log_sha256(results)},
        {"report",
         truetest::simulation::deterministic_report_sha256(results, config)},
    };
    for (const auto& trial : results.trials)
    {
        hashes.emplace(trial_hash_name(trial.trial_id, "event_log"),
                       trial.event_log_sha256);
        hashes.emplace(trial_hash_name(trial.trial_id, "lifecycle"),
                       trial.lifecycle_sha256);
        hashes.emplace(trial_hash_name(trial.trial_id, "result"),
                       trial.trial_result_sha256);
        if (config.generator_config.emit_synthetic_l2)
            hashes.emplace(trial_hash_name(trial.trial_id, "synthetic_l2"),
                           trial.synthetic_l2_sha256);
    }
    return hashes;
}

static std::vector<std::string> compare_expected_hashes(
    const std::map<std::string, std::string, std::less<>>& expected,
    const std::map<std::string, std::string, std::less<>>& actual,
    const std::vector<std::string>& required)
{
    std::vector<std::string> mismatches;
    for (const auto& kind : required)
    {
        const auto expected_value = expected.find(kind);
        const auto actual_value = actual.find(kind);
        if (expected_value == expected.end())
        {
            std::cout << "MISMATCH " << kind
                      << " expected=<missing> actual="
                      << (actual_value == actual.end() ? "<missing>"
                                                       : actual_value->second)
                      << '\n';
            mismatches.push_back(kind + ": expected hash missing");
            continue;
        }
        if (actual_value == actual.end()
            || actual_value->second != expected_value->second)
        {
            std::cout << "MISMATCH " << kind
                      << " expected=" << expected_value->second
                      << " actual="
                      << (actual_value == actual.end() ? "<missing>"
                                                       : actual_value->second)
                      << '\n';
            mismatches.push_back(kind + ": hash mismatch");
        }
        else
        {
            std::cout << "MATCH " << kind << " expected="
                      << expected_value->second << " actual="
                      << actual_value->second << '\n';
        }
    }
    return mismatches;
}

static double deterministic_number(std::string_view text,
                                   std::string_view field)
{
    double value = 0.0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value,
        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || !std::isfinite(value))
        throw std::invalid_argument(
            "invalid finite deterministic value for " + std::string(field));
    return value;
}

static instrument_spec deterministic_instrument_snapshot(
    const cli_options& options)
{
    if (options.symbol.empty() || options.instrument_specs.size() != 1U)
        throw std::invalid_argument(
            "deterministic local backtest requires --symbol and exactly one --instrument snapshot");
    const std::string& input = options.instrument_specs.front();
    const auto colon = input.find(':');
    if (colon == std::string::npos || input.substr(0, colon) != options.symbol)
        throw std::invalid_argument(
            "deterministic --instrument must match --symbol");

    instrument_spec result;
    result.symbol = options.symbol;
    std::map<std::string, bool, std::less<>> seen{
        {"tick", false}, {"lot", false}, {"minq", false}, {"minn", false},
        {"maker", false}, {"taker", false},
    };
    std::string_view fields(input.data() + colon + 1,
                            input.size() - colon - 1);
    std::size_t cursor = 0;
    while (cursor <= fields.size())
    {
        const std::size_t comma = fields.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos
            ? fields.size() : comma;
        const std::string_view token = fields.substr(cursor, end - cursor);
        const auto equals = token.find('=');
        if (equals == std::string_view::npos || equals == 0
            || equals + 1 >= token.size())
            throw std::invalid_argument(
                "deterministic --instrument requires key=value fields");
        const std::string key(token.substr(0, equals));
        const double value = deterministic_number(
            token.substr(equals + 1), key);
        const auto known = seen.find(key);
        if (known == seen.end() || known->second)
            throw std::invalid_argument(
                "unknown or duplicate deterministic instrument field: " + key);
        known->second = true;
        if (key == "tick") result.tick_size = value;
        else if (key == "lot") result.lot_size = value;
        else if (key == "minq") result.min_qty = value;
        else if (key == "minn") result.min_notional = value;
        else if (key == "maker") result.maker_rate = value;
        else if (key == "taker") result.taker_rate = value;
        if (comma == std::string_view::npos) break;
        cursor = comma + 1;
    }
    for (const auto& [field, present] : seen)
        if (!present)
            throw std::invalid_argument(
                "deterministic instrument snapshot lacks field: " + field);
    if (!(result.tick_size > 0.0) || !(result.lot_size > 0.0)
        || result.min_qty < 0.0 || result.min_notional < 0.0)
        throw std::invalid_argument(
            "deterministic instrument tick/lot must be positive and minima non-negative");
    return result;
}

static truetest::reproducibility::CanonicalJsonValue
configure_deterministic_local_strategy(
    IStrategy& strategy, const cli_options& options,
    std::string_view strategy_name, std::size_t strategy_index)
{
    namespace strategy_config = truetest::strategy_config;
    using Json = truetest::reproducibility::CanonicalJsonValue;
    if (strategy_index == 0U
        && (strategy_name == "sma" || strategy_name == "mean-reversion")
        && options.sma_period != 20U)
        (void)strategy_config::set_parameter_if_declared(
            strategy, "period", static_cast<double>(options.sma_period));
    (void)strategy_config::set_parameter_if_declared(
        strategy, "equity", options.balance);
    (void)strategy_config::set_parameter_if_declared(
        strategy, "risk_fraction", options.risk_fraction);
    (void)strategy_config::set_parameter_if_declared(
        strategy, "sl_pct", options.sl_pct);
    (void)strategy_config::set_parameter_if_declared(
        strategy, "tp_pct", options.tp_pct);

    const double entry_fee =
        (options.fee_model == "zero" || options.fee_model == "fixed")
            ? 0.0
            : options.taker_rate > 0.0 ? options.taker_rate
            : options.maker_rate > 0.0 ? options.maker_rate : 0.0;
    (void)strategy_config::set_parameter_if_declared(
        strategy, "entry_fee_rate", entry_fee);
    (void)strategy_config::set_parameter_if_declared(
        strategy, "exit_fee_rate", entry_fee);
    (void)strategy_config::set_parameter_if_declared(
        strategy, "fixed_fee_per_leg",
        options.fee_model == "fixed" ? options.fee_value : 0.0);
    const double slip_bps = std::max(
        options.mm_spread_pct > 0.0
            ? options.mm_spread_pct * 10'000.0 : 0.0,
        options.bar_spread_bps > 0.0
            ? options.bar_spread_bps * 0.5 : 0.0);
    (void)strategy_config::set_parameter_if_declared(
        strategy, "entry_slip_bps", slip_bps);
    (void)strategy_config::set_parameter_if_declared(
        strategy, "exit_slip_bps", slip_bps);

    for (const auto& parameter : options.params)
    {
        const auto equals = parameter.find('=');
        if (equals == std::string::npos || equals == 0
            || equals + 1 >= parameter.size())
            throw std::invalid_argument(
                "deterministic strategy parameter must be key=value");
        const std::string key = parameter.substr(0, equals);
        const double value = deterministic_number(
            std::string_view(parameter).substr(equals + 1), key);
        (void)strategy_config::set_parameter_if_declared(
            strategy, key, value, true);
    }

    Json::Object output;
    for (const auto& [name, value] :
         strategy_config::effective_parameter_values(strategy))
        output.emplace(name, value);
    return Json(std::move(output));
}

static truetest::reproducibility::CanonicalJsonValue
deterministic_strategy_snapshot(const cli_options& options)
{
    namespace repro = truetest::reproducibility;
    using Json = repro::CanonicalJsonValue;
    const auto names = split_csv(
        options.strategy.empty() ? std::string("mean-reversion")
                                 : options.strategy);
    if (names.empty())
        throw std::invalid_argument(
            "deterministic backtest requires at least one strategy");

    Json::Array descriptors;
    descriptors.reserve(names.size());
    for (std::size_t strategy_index = 0;
         strategy_index < names.size(); ++strategy_index)
    {
        const auto& name = names[strategy_index];
        const auto& registry = StrategyRegistry::instance();
        if (!registry.has(name))
            throw std::invalid_argument(
                "deterministic backtest has unknown strategy '" + name + "'");
        auto strategy = registry.create(name);
        if (!strategy)
            throw std::invalid_argument(
                "deterministic backtest strategy factory returned null for '"
                + name + "'");
        Json parameter_values = configure_deterministic_local_strategy(
            *strategy, options, name, strategy_index);
        descriptors.push_back(Json::object({
            {"config", Json::object({
                {"effective_parameters", std::move(parameter_values)},
            })},
            {"id", name},
            {"version", "strategy-source-contract-v1"},
        }));
    }
    Json primary = descriptors.front();
    Json::Array additional;
    for (std::size_t index = 1; index < descriptors.size(); ++index)
        additional.push_back(descriptors[index]);
    primary["config"]["additional_strategies"] = std::move(additional);
    return primary;
}

static void validate_deterministic_local_effective_config(
    const cli_options& options)
{
    const auto finite_nonnegative = [](double value) {
        return std::isfinite(value) && value >= 0.0;
    };
    if (!(options.balance > 0.0) || !std::isfinite(options.balance)
        || !finite_nonnegative(options.risk_fraction)
        || options.risk_fraction > 1.0
        || options.rolling_window == 0 || options.periods_per_year == 0
        || !std::isfinite(options.risk_free_rate)
        || !(options.market_aggression > 0.0)
        || !std::isfinite(options.market_aggression)
        || !(options.qty_scale > 0.0) || !std::isfinite(options.qty_scale))
        throw std::invalid_argument(
            "deterministic local analytics, balance, and execution scaling must be finite and effective");
    if (!finite_nonnegative(options.fill_prob) || options.fill_prob > 1.0
        || !finite_nonnegative(options.fill_fade) || options.fill_fade > 1.0
        || !finite_nonnegative(options.fill_decay)
        || (options.fill_fade > 0.0 && !(options.fill_prob > 0.0)))
        throw std::invalid_argument(
            "deterministic local fill configuration is invalid");
    if (options.fill_prob == 0.0
        && (options.fill_fade != 0.0 || options.fill_decay != 10.0))
        throw std::invalid_argument(
            "deterministic local disabled fill model refuses inert calibration");
    if (options.order_latency_us < 0
        || options.order_latency_stddev_us < 0)
        throw std::invalid_argument(
            "deterministic local latency must be non-negative");
    if (options.order_latency_stddev_us > 0
        && options.order_latency_us <= 0)
        throw std::invalid_argument(
            "deterministic local stochastic latency requires a positive mean");
    if (options.wire_latency_us != 0)
        throw std::invalid_argument(
            "deterministic local wire latency is unsupported by the local execution adapter");
    if (!finite_nonnegative(options.impact_k_bps)
        || !finite_nonnegative(options.impact_adv)
        || ((options.impact_k_bps > 0.0)
            != (options.impact_adv > 0.0)))
        throw std::invalid_argument(
            "deterministic local impact requires positive k_bps and ADV together");
    if (options.walked_book_impact)
        throw std::invalid_argument(
            "deterministic local walked-book impact requires a hashed L2 dataset and is unsupported");
    if (!finite_nonnegative(options.bar_spread_bps)
        || options.mm_levels <= 0 || options.mm_base_depth <= 0
        || !(options.mm_spread_pct > 0.0)
        || !std::isfinite(options.mm_spread_pct)
        || !finite_nonnegative(options.mm_vol_mult)
        || !(options.mm_max_spread_pct > 0.0)
        || !std::isfinite(options.mm_max_spread_pct))
        throw std::invalid_argument(
            "deterministic local market-maker calibration is invalid");
    if (!std::isfinite(options.sl_pct) || !std::isfinite(options.tp_pct)
        || !std::isfinite(options.trail_pct)
        || options.sl_pct < 0.0 || options.sl_pct > 1.0
        || options.tp_pct < 0.0 || options.tp_pct > 1.0
        || options.trail_pct < 0.0 || options.trail_pct > 1.0
        || !truetest::exits::parse_exit_policy_mode(options.exit_policy))
        throw std::invalid_argument(
            "deterministic local exit configuration is invalid");
    if (options.fill_rng_seed_explicitly_set)
        throw std::invalid_argument(
            "deterministic local run refuses --fill-rng-seed; fill seeds derive from the master seed");
    if (options.spread_step_explicitly_set)
        throw std::invalid_argument(
            "deterministic local run refuses --spread-step; the local adapter does not consume it");
    if (options.debug_fills || options.debug_fills_budget != 20
        || options.realistic_fills || options.log_max_size_mb != 0
        || options.log_keep != 5 || !options.compress_log
        || options.output_format != "json")
        throw std::invalid_argument(
            "deterministic local run refuses debug, deprecated, log-rotation, compression, and alternate output side paths");
    if (!options.api_key.empty() || !options.api_secret.empty()
        || !options.api_passphrase.empty()
        || options.checkpoint_interval != 10'000
        || options.backfill != 500 || !options.backfill_interval.empty()
        || !options.margin_type.empty()
        || options.liquidation_warn_pct != -1.0
        || options.margin_type_strict
        || options.max_notional_usdt != -1.0
        || options.max_leverage != -1.0
        || options.min_liquidation_distance_pct != -1.0
        || options.dead_man_countdown_ms != 30'000
        || options.dead_man_heartbeat_ms != 0 || options.disarm_deadman
        || options.reconcile_tolerance_bps != 10.0
        || options.kill_switch_deadline_ms != 5'000)
        throw std::invalid_argument(
            "deterministic local run refuses credentials and provider-only checkpoint, backfill, venue-risk, dead-man, reconciliation, and kill-switch inputs");
    if (options.mc_trials_explicitly_set
        || options.mc_model_explicitly_set || !options.mc_model.empty()
        || options.mc_params_explicitly_set || !options.mc_params.empty()
        || options.mc_parallel
        || options.mc_workers != 0 || options.mc_workers_explicitly_set
        || options.mc_reuse_objects || options.replay_from_explicitly_set
        || options.replay_to_explicitly_set)
        throw std::invalid_argument(
            "deterministic local run refuses mode-specific Monte Carlo and event-log replay inputs");
    (void)deterministic_strategy_snapshot(options);
}

static truetest::reproducibility::CanonicalJsonValue
deterministic_local_effective_config(
    const cli_options& options,
    std::size_t lifecycle_record_capacity)
{
    using Json = truetest::reproducibility::CanonicalJsonValue;
    const engine_config defaults;
    const pool_prewarm_settings& pools = defaults.pool_prewarm;
    Json::Array parameters;
    for (const auto& parameter : options.params)
        parameters.emplace_back(parameter);
    return Json::object({
        {"artifact_persistence", Json::object({
            {"event_log_compression", "zstd"},
            {"event_log_file_version", static_cast<std::uint64_t>(
                EVENT_LOG_FILE_VERSION)},
            {"lifecycle_capture", "full-preallocated-v1"},
            {"lifecycle_record_capacity", static_cast<std::uint64_t>(
                lifecycle_record_capacity)},
            {"economic_result_schema", "economic-result-v2"},
            {"report_canonicalization", "canonical-json-v1"},
            {"report_schema", "deterministic-report-v1"},
        })},
        {"balance", options.balance},
        {"bar_spread_bps", options.bar_spread_bps},
        {"engine_runtime", Json::object({
            {"disable_pinning", true},
            {"drop_policy", "block"},
            {"max_consecutive_worker_errors", static_cast<std::uint64_t>(
                defaults.max_consecutive_worker_errors)},
            {"mode", "backtest"},
            {"pin_event_loop", defaults.pin_event_loop},
            {"pin_logging", defaults.pin_logging},
            {"pin_market_maker", defaults.pin_mm},
            {"pin_risk", defaults.pin_risk},
            {"pin_stats", defaults.pin_stats},
            {"ring_buffer_capacity", static_cast<std::uint64_t>(
                defaults.ring_buffer_capacity)},
            {"show_progress", false},
            {"spin_policy", options.spin_policy_str.empty()
                ? std::string("adaptive") : options.spin_policy_str},
            {"thread_preset", "logging"},
        })},
        {"execution_bar_delay", static_cast<std::uint64_t>(options.exec_bar_delay)},
        {"exit_defaults", Json::object({
            {"mode", options.exit_policy},
            {"sl_pct", options.sl_pct},
            {"tp_pct", options.tp_pct},
            {"trail_pct", options.trail_pct},
        })},
        {"fill_decay", options.fill_decay},
        {"fill_fade", options.fill_fade},
        {"fill_probability", options.fill_prob},
        {"fill_seed_source", "master-seed-fill-domain-v1"},
        {"format", options.format.empty() ? std::string("bar") : options.format},
        {"impact_adv", options.impact_adv},
        {"impact_k_bps", options.impact_k_bps},
        {"maker_queue_model", options.maker_queue_model},
        {"market_aggression", options.market_aggression},
        {"max_equity_points", static_cast<std::uint64_t>(
            std::max<std::size_t>(options.max_equity_points, 4U))},
        {"mm_base_depth", options.mm_base_depth},
        {"mm_levels", options.mm_levels},
        {"mm_max_spread_pct", options.mm_max_spread_pct},
        {"mm_spread_pct", options.mm_spread_pct},
        {"mm_vol_mult", options.mm_vol_mult},
        {"order_latency_stddev_us", options.order_latency_stddev_us},
        {"order_latency_us", options.order_latency_us},
        {"periods_per_year", static_cast<std::uint64_t>(options.periods_per_year)},
        {"pool_prewarm", Json::object({
            {"amend_blocks", static_cast<std::uint64_t>(pools.amend_blocks)},
            {"cancel_blocks", static_cast<std::uint64_t>(pools.cancel_blocks)},
            {"control_block_slots", static_cast<std::uint64_t>(
                pools.control_block_slots)},
            {"fill_blocks", static_cast<std::uint64_t>(pools.fill_blocks)},
            {"forbid_runtime_grow", pools.forbid_runtime_grow},
            {"funding_blocks", static_cast<std::uint64_t>(pools.funding_blocks)},
            {"l2_snapshot_blocks", static_cast<std::uint64_t>(
                pools.l2_snapshot_blocks)},
            {"l2_update_blocks", static_cast<std::uint64_t>(
                pools.l2_update_blocks)},
            {"market_blocks", static_cast<std::uint64_t>(pools.market_blocks)},
            {"order_blocks", static_cast<std::uint64_t>(pools.order_blocks)},
            {"orderbook_order_blocks", static_cast<std::uint64_t>(
                pools.orderbook_order_blocks)},
            {"rejection_blocks", static_cast<std::uint64_t>(
                pools.rejection_blocks)},
            {"tick_blocks", static_cast<std::uint64_t>(pools.tick_blocks)},
        })},
        {"provider", "local"},
        {"quantity_scale", options.qty_scale},
        {"queue_model", options.queue_model},
        {"risk_free_rate", options.risk_free_rate},
        {"risk_fraction", options.risk_fraction},
        {"risk", Json::object({
            {"daily_reset_hour", options.risk_daily_reset_hour},
            {"max_daily_loss", options.risk_max_daily_loss},
            {"max_drawdown", options.risk_max_drawdown},
            {"max_funding_8h_rate", options.risk_max_funding_8h_rate},
            {"max_gross_leverage", resolve_max_gross_leverage(
                options.risk_max_gross_leverage, "local")},
            {"max_loss_per_trade", options.risk_max_loss_per_trade},
            {"max_mark_age_ms", static_cast<std::int64_t>(
                options.risk_max_mark_age_ms)},
            {"max_open_orders", options.risk_max_open_orders},
            {"max_orders_per_minute", options.risk_max_orders_per_minute},
            {"max_portfolio_exposure", options.risk_max_portfolio_exposure},
            {"max_position_pct_of_equity",
                defaults.risk.max_position_pct_of_equity},
            {"max_position_value", options.risk_max_position_value},
            {"max_spread_bps", defaults.risk.max_spread_bps},
            {"max_symbol_inventory_qty", options.risk_max_inventory_qty},
            {"max_trades_per_hour", options.risk_max_trades_per_hour},
            {"require_fresh_mark", options.risk_require_fresh_mark},
            {"soft_portfolio_limits", resolve_risk_soft_portfolio_limits(
                false, "backtest", options.risk_soft_portfolio_limits)},
            {"unwind", options.risk_unwind},
        })},
        {"rolling_window", static_cast<std::uint64_t>(options.rolling_window)},
        {"sma_period", static_cast<std::uint64_t>(options.sma_period)},
        {"strategy", options.strategy.empty()
             ? std::string("mean-reversion") : options.strategy},
        {"strategy_overrides", std::move(parameters)},
        {"symbol", options.symbol},
        {"walked_book_impact", options.walked_book_impact},
        {"wire_latency_us", options.wire_latency_us},
    });
}

static truetest::reproducibility::RunManifestV1
make_deterministic_local_manifest(
    const cli_options& options,
    const truetest::reproducibility::DatasetSnapshot& dataset,
    const instrument_spec& instrument,
    std::size_t lifecycle_record_capacity)
{
    namespace repro = truetest::reproducibility;
    using Json = repro::CanonicalJsonValue;
    const repro::DeterministicSeedDeriver seeds(options.seed);
    Json::Array strategy_seed_instances;
    auto strategy_names = split_csv(
        options.strategy.empty() ? std::string("mean-reversion")
                                 : options.strategy);
    if (strategy_names.empty())
        strategy_names.emplace_back("mean-reversion");
    strategy_seed_instances.reserve(strategy_names.size());
    for (std::size_t index = 0; index < strategy_names.size(); ++index)
    {
        strategy_seed_instances.push_back(Json::object({
            {"index", static_cast<std::uint64_t>(index)},
            {"name", strategy_names[index]},
            {"seed", seeds.derive(
                repro::SeedDomain::strategy,
                static_cast<std::uint64_t>(index))},
        }));
    }
    const auto model = [](std::string id, std::string version,
                          Json::Object config = {}) {
        return Json::object({{"config", std::move(config)},
                             {"id", std::move(id)},
                             {"version", std::move(version)}});
    };
    const std::string fee_id = options.fee_model.empty()
        ? "zero" : options.fee_model;
    const auto inputs = Json::object({
        {"build", repro::build_identity_json(repro::current_build_identity())},
        {"dataset", repro::dataset_identity_json(dataset)},
        {"determinism_envelope", Json::object({
            {"cross_compiler_bit_identity", false},
            {"cross_cpu_bit_identity", false},
            {"event_log_binary_encoding",
             "little-endian-ieee754-binary64-v3"},
            {"prng", "xoshiro256starstar-v1"},
            {"sampling", "truetest-uniform-box-muller-v1"},
            {"scope", "same-source-build-toolchain-libc-stdlib-architecture"},
        })},
        {"effective_config", deterministic_local_effective_config(
            options, lifecycle_record_capacity)},
        {"fee_schedule", Json::object({
            {"fixed_fee", options.fee_value},
            {"funding", Json::object({{"enabled", false}, {"version", "disabled-v1"}})},
            {"maker_rate", options.maker_rate},
            {"snapshot_version", "local-fee-snapshot-v1"},
            {"taker_rate", options.taker_rate},
            {"type", fee_id},
        })},
        {"instrument", Json::object({
            {"asset_type", "configured-local-instrument"},
            {"lot_size", instrument.lot_size},
            {"maker_rate", instrument.maker_rate},
            {"minimum_notional", instrument.min_notional},
            {"minimum_quantity", instrument.min_qty},
            {"minimum_validation_policy",
             "qty-epsilon-1e-12-notional-epsilon-1e-9-v1"},
            {"rounding_policy",
             "price-nearest-tick-quantity-floor-lot-v1"},
            {"symbol", instrument.symbol},
            {"taker_rate", instrument.taker_rate},
            {"tick_size", instrument.tick_size},
            {"venue", "local"},
        })},
        {"models", Json::object({
            {"fee", model(fee_id, "fee-model-v1", {
                {"fixed_fee", options.fee_value},
                {"maker_rate", options.maker_rate},
                {"taker_rate", options.taker_rate},
            })},
            {"fill", model(options.fill_prob > 0.0 ? "realistic" : "perfect",
                           "fill-model-v1", {
                {"decay", options.fill_decay}, {"fade", options.fill_fade},
                {"probability", options.fill_prob},
            })},
            {"impact", model(options.impact_k_bps > 0.0 ? "square-root" : "zero",
                             "impact-model-v1", {
                {"adv", options.impact_adv}, {"k_bps", options.impact_k_bps},
                {"walked_book", options.walked_book_impact},
            })},
            {"latency", model(options.order_latency_stddev_us > 0
                    ? "deterministic-stochastic" : options.order_latency_us > 0
                    ? "fixed" : "zero", "latency-model-v1", {
                {"mean_us", options.order_latency_us},
                {"stddev_us", options.order_latency_stddev_us},
                {"wire_us", options.wire_latency_us},
            })},
            {"queue", model(options.maker_queue_model, "queue-model-v1", {
                {"position_model", options.queue_model},
            })},
            {"simulator", model("truetest-event-engine", "simulation-engine-v1", {
                {"event_ordering", dataset.event_ordering_rule},
                {"tie_breaking", dataset.tie_breaking_rule},
            })},
            {"synthetic_gbm", model("disabled", "synthetic-gbm-v1")},
            {"synthetic_l2", model("seeded-market-maker", "synthetic-l2-v1", {
                {"base_depth", options.mm_base_depth},
                {"levels", options.mm_levels},
                {"max_spread_pct", options.mm_max_spread_pct},
                {"spread_pct", options.mm_spread_pct},
                {"volatility_multiplier", options.mm_vol_mult},
            })},
        })},
        {"monte_carlo", Json::object({
            {"lifecycle_persistence", "full"},
            {"trial_count", std::uint64_t{0}},
        })},
        {"run_mode", "backtest"},
        {"seed_hierarchy", Json::object({
            {"derivation_version", std::uint64_t{repro::kSeedDerivationVersion}},
            {"domains", Json::object({
                {"fill_model", seeds.derive(repro::SeedDomain::fill_model)},
                {"impact_model", seeds.derive(repro::SeedDomain::impact_model)},
                {"latency_model", seeds.derive(repro::SeedDomain::latency_model)},
                {"market_maker", seeds.derive(repro::SeedDomain::market_maker)},
                {"queue_model", seeds.derive(repro::SeedDomain::queue_model)},
                {"run", seeds.derive(repro::SeedDomain::run)},
                {"strategy", seeds.derive(repro::SeedDomain::strategy)},
            })},
            {"master_seed", options.seed},
            {"strategy_instances", std::move(strategy_seed_instances)},
            {"trials", Json::array({})},
        })},
        {"strategy", deterministic_strategy_snapshot(options)},
        {"target", "engine_backtest"},
        {"threading", Json::object({
            {"completion_order_affects_results", false},
            {"analytics_execution", "inline"},
            {"logging_workers", std::uint64_t{1}},
            {"preset", "logging"},
            {"workers", std::uint64_t{2}},
        })},
    });
    return repro::RunManifestV1(
        inputs, repro::dataset_locations_json(dataset), true);
}

static std::string deterministic_double_text(double value)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value;
    return output.str();
}

[[maybe_unused]] static cli_options deterministic_local_options_from_manifest(
    const truetest::reproducibility::RunManifestV1& manifest,
    const std::filesystem::path& manifest_directory)
{
    using Json = truetest::reproducibility::CanonicalJsonValue;
    const Json& inputs = manifest.deterministic_inputs();
    if (inputs.at("target").as_string() != "engine_backtest"
        || inputs.at("run_mode").as_string() != "backtest")
        throw std::invalid_argument(
            "manifest is not a local engine_backtest run");
    const Json& config = inputs.at("effective_config");
    if (config.at("provider").as_string() != "local")
        throw std::invalid_argument(
            "deterministic provider replay supports local datasets only");
    cli_options result;
    result.provider_name = "local";
    const auto dataset = truetest::reproducibility::dataset_snapshot_from_json(
        inputs.at("dataset"), manifest.dataset_locations());
    if (dataset.schema_version != 1U)
        throw std::invalid_argument(
            "unsupported deterministic local dataset schema version");
    for (std::size_t index = 0; index < dataset.artifacts.size(); ++index)
    {
        if (index != 0) result.provider_path.push_back(',');
        std::filesystem::path locator(dataset.artifacts[index].locator);
        if (locator.is_relative()) locator = manifest_directory / locator;
        result.provider_path += locator.string();
    }
    result.seed = inputs.at("seed_hierarchy").at("master_seed").as_u64();
    result.seed_explicitly_set = true;
    result.balance = config.at("balance").as_double();
    result.deterministic_lifecycle_record_capacity =
        static_cast<std::size_t>(config.at("artifact_persistence")
            .at("lifecycle_record_capacity").as_u64());
    result.bar_spread_bps = config.at("bar_spread_bps").as_double();
    result.exec_bar_delay = static_cast<std::size_t>(
        config.at("execution_bar_delay").as_u64());
    const Json& exit_defaults = config.at("exit_defaults");
    result.exit_policy = exit_defaults.at("mode").as_string();
    result.sl_pct = exit_defaults.at("sl_pct").as_double();
    result.tp_pct = exit_defaults.at("tp_pct").as_double();
    result.trail_pct = exit_defaults.at("trail_pct").as_double();
    result.fill_decay = config.at("fill_decay").as_double();
    result.fill_fade = config.at("fill_fade").as_double();
    result.fill_prob = config.at("fill_probability").as_double();
    result.format = config.at("format").as_string();
    result.impact_adv = config.at("impact_adv").as_double();
    result.impact_k_bps = config.at("impact_k_bps").as_double();
    result.spin_policy_str = config.at("engine_runtime")
        .at("spin_policy").as_string();
    result.maker_queue_model = config.at("maker_queue_model").as_string();
    result.market_aggression = config.at("market_aggression").as_double();
    result.max_equity_points = static_cast<std::size_t>(
        config.at("max_equity_points").as_u64());
    result.mm_base_depth = static_cast<int>(config.at("mm_base_depth").as_i64());
    result.mm_levels = static_cast<int>(config.at("mm_levels").as_i64());
    result.mm_max_spread_pct = config.at("mm_max_spread_pct").as_double();
    result.mm_spread_pct = config.at("mm_spread_pct").as_double();
    result.mm_vol_mult = config.at("mm_vol_mult").as_double();
    result.order_latency_stddev_us = static_cast<int>(
        config.at("order_latency_stddev_us").as_i64());
    result.order_latency_us = static_cast<int>(
        config.at("order_latency_us").as_i64());
    result.periods_per_year = static_cast<std::size_t>(
        config.at("periods_per_year").as_u64());
    result.periods_per_year_explicit = true;
    result.qty_scale = config.at("quantity_scale").as_double();
    result.queue_model = config.at("queue_model").as_string();
    result.risk_free_rate = config.at("risk_free_rate").as_double();
    result.risk_fraction = config.at("risk_fraction").as_double();
    const Json& risk = config.at("risk");
    result.risk_daily_reset_hour = static_cast<int>(
        risk.at("daily_reset_hour").as_i64());
    result.risk_max_daily_loss = risk.at("max_daily_loss").as_double();
    result.risk_max_drawdown = risk.at("max_drawdown").as_double();
    result.risk_max_funding_8h_rate =
        risk.at("max_funding_8h_rate").as_double();
    result.risk_max_gross_leverage =
        risk.at("max_gross_leverage").as_double();
    result.risk_max_loss_per_trade =
        risk.at("max_loss_per_trade").as_double();
    result.risk_max_mark_age_ms =
        risk.at("max_mark_age_ms").as_i64();
    result.risk_max_open_orders = static_cast<int>(
        risk.at("max_open_orders").as_i64());
    result.risk_max_orders_per_minute = static_cast<int>(
        risk.at("max_orders_per_minute").as_i64());
    result.risk_max_portfolio_exposure =
        risk.at("max_portfolio_exposure").as_double();
    result.risk_max_position_value =
        risk.at("max_position_value").as_double();
    result.risk_max_inventory_qty =
        risk.at("max_symbol_inventory_qty").as_double();
    result.risk_max_trades_per_hour = static_cast<int>(
        risk.at("max_trades_per_hour").as_i64());
    result.risk_require_fresh_mark =
        risk.at("require_fresh_mark").as_bool();
    result.risk_soft_portfolio_limits =
        risk.at("soft_portfolio_limits").as_bool();
    result.risk_unwind = risk.at("unwind").as_bool();
    result.rolling_window = static_cast<std::size_t>(
        config.at("rolling_window").as_u64());
    result.sma_period = static_cast<std::size_t>(
        config.at("sma_period").as_u64());
    result.strategy = config.at("strategy").as_string();
    for (const auto& parameter : config.at("strategy_overrides").as_array())
        result.params.push_back(parameter.as_string());
    result.symbol = config.at("symbol").as_string();
    result.walked_book_impact = config.at("walked_book_impact").as_bool();
    result.wire_latency_us = static_cast<int>(
        config.at("wire_latency_us").as_i64());
    const Json& fees = inputs.at("fee_schedule");
    result.fee_model = fees.at("type").as_string();
    result.fee_value = fees.at("fixed_fee").as_double();
    result.maker_rate = fees.at("maker_rate").as_double();
    result.taker_rate = fees.at("taker_rate").as_double();
    result.fee_model_explicit = true;
    result.maker_rate_explicit = true;
    result.taker_rate_explicit = true;
    const Json& instrument = inputs.at("instrument");
    if (instrument.at("rounding_policy").as_string()
            != "price-nearest-tick-quantity-floor-lot-v1"
        || instrument.at("minimum_validation_policy").as_string()
            != "qty-epsilon-1e-12-notional-epsilon-1e-9-v1")
        throw std::invalid_argument(
            "unsupported deterministic local instrument validation policy");
    result.instrument_specs.push_back(
        result.symbol + ":tick="
        + deterministic_double_text(instrument.at("tick_size").as_double())
        + ",lot="
        + deterministic_double_text(instrument.at("lot_size").as_double())
        + ",minq="
        + deterministic_double_text(
            instrument.at("minimum_quantity").as_double())
        + ",minn="
        + deterministic_double_text(
            instrument.at("minimum_notional").as_double())
        + ",maker="
        + deterministic_double_text(instrument.at("maker_rate").as_double())
        + ",taker="
        + deterministic_double_text(instrument.at("taker_rate").as_double()));
    result.mode = "backtest";
    result.thread_preset_str = "logging";
    result.no_pin = true;
    result.no_tui = true;
    result.status_format = "off";
    return result;
}

struct deterministic_local_artifact_paths
{
    std::filesystem::path directory;
    std::filesystem::path dataset_directory;
    std::filesystem::path event_log;
    std::filesystem::path event_log_partial;
    std::filesystem::path lifecycle;
    std::filesystem::path report;
    std::filesystem::path receipt;
};

static deterministic_local_artifact_paths deterministic_local_paths(
    const std::filesystem::path& directory)
{
    if (directory.empty())
        throw std::invalid_argument(
            "deterministic local run requires an artifacts directory");
    return {
        directory,
        directory / "dataset",
        directory / "events.zst",
        directory / "events.zst.partial",
        directory / "lifecycle.jsonl",
        directory / "report.json",
        directory / "run_receipt.json",
    };
}

static void require_new_artifact_destination(
    const std::filesystem::path& path,
    std::string_view description)
{
    if (path.empty() || path.filename().empty())
        throw std::invalid_argument(
            std::string(description) + " must name a file");
    std::error_code error;
    if (std::filesystem::exists(path, error))
        throw std::runtime_error(
            "refusing to overwrite " + std::string(description) + ": "
            + path.string());
    if (error)
        throw std::runtime_error(
            "cannot inspect " + std::string(description) + ": "
            + path.string());
    const std::filesystem::path partial(path.string() + ".partial");
    if (std::filesystem::exists(partial, error))
        throw std::runtime_error(
            "incomplete " + std::string(description) + " already exists: "
            + partial.string());
    if (error)
        throw std::runtime_error(
            "cannot inspect " + std::string(description)
            + " staging path: " + partial.string());
}

static void prepare_deterministic_local_artifacts(
    const deterministic_local_artifact_paths& paths)
{
    const std::array targets{
        paths.event_log,
        paths.event_log_partial,
        paths.lifecycle,
        std::filesystem::path(paths.lifecycle.string() + ".partial"),
        paths.report,
        std::filesystem::path(paths.report.string() + ".partial"),
        paths.receipt,
        std::filesystem::path(paths.receipt.string() + ".partial"),
    };
    std::error_code error;
    for (const auto& target : targets)
    {
        if (std::filesystem::exists(target, error))
            throw std::runtime_error(
                "refusing to overwrite deterministic run artifact: "
                + target.string());
        if (error)
            throw std::runtime_error(
                "cannot inspect deterministic run artifact: "
                + target.string());
    }
    std::filesystem::create_directories(paths.directory, error);
    if (error)
        throw std::runtime_error(
            "cannot create deterministic artifact directory: "
            + error.message());
}

class deterministic_local_receipt_guard final
{
public:
    deterministic_local_receipt_guard(
        deterministic_local_artifact_paths paths,
        std::chrono::steady_clock::time_point started,
        bool dataset_override)
        : paths_(std::move(paths)), started_(started),
          dataset_override_(dataset_override)
    {
    }

    deterministic_local_receipt_guard(
        const deterministic_local_receipt_guard&) = delete;
    deterministic_local_receipt_guard& operator=(
        const deterministic_local_receipt_guard&) = delete;

    ~deterministic_local_receipt_guard()
    {
        if (receipt_published_)
            return;
        try
        {
            truetest::reproducibility::RunReceiptV1 receipt;
            receipt.executed_at_utc = reproducibility_utc_now();
            receipt.duration_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_).count());
            receipt.status = "failed";
            receipt.dataset_mismatch_override_used = dataset_override_;
            receipt.run_fingerprint = run_fingerprint_;
            receipt.mismatches = dataset_mismatches_;
            receipt.mismatches.push_back(failure_reason_);
            receipt.write_atomic(paths_.receipt);
        }
        catch (const std::exception& error)
        {
            std::cerr << "  ! Failed to persist deterministic receipt: "
                      << error.what() << '\n';
        }
    }

    void set_run_fingerprint(std::string fingerprint)
    {
        run_fingerprint_ = std::move(fingerprint);
    }

    void set_failure(std::string_view reason)
    {
        if (!reason.empty())
            failure_reason_.assign(reason);
    }

    void set_dataset_mismatches(std::vector<std::string> mismatches)
    {
        dataset_mismatches_ = std::move(mismatches);
    }

    void mark_receipt_published() noexcept { receipt_published_ = true; }

private:
    deterministic_local_artifact_paths paths_;
    std::chrono::steady_clock::time_point started_;
    bool dataset_override_ = false;
    bool receipt_published_ = false;
    std::string run_fingerprint_;
    std::string failure_reason_ =
        "deterministic run terminated before artifact finalization";
    std::vector<std::string> dataset_mismatches_;
};

struct staged_deterministic_dataset final
{
    std::vector<std::string> execution_paths;
    std::vector<truetest::reproducibility::DatasetArtifact> identities;
};

static staged_deterministic_dataset stage_deterministic_dataset(
    const std::vector<std::string>& original_paths,
    const deterministic_local_artifact_paths& paths)
{
    namespace repro = truetest::reproducibility;
    staged_deterministic_dataset staged;
    staged.execution_paths.reserve(original_paths.size());
    staged.identities.reserve(original_paths.size());

    std::error_code error;
    std::filesystem::create_directories(paths.dataset_directory, error);
    if (error)
        throw std::runtime_error(
            "cannot create deterministic dataset snapshot directory: "
            + error.message());

    for (std::size_t index = 0; index < original_paths.size(); ++index)
    {
        std::ostringstream name;
        name << "input_" << std::setw(6) << std::setfill('0') << index;
        const std::filesystem::path destination =
            paths.dataset_directory / name.str();
        require_new_artifact_destination(
            destination, "deterministic dataset snapshot");
        const std::filesystem::path partial(destination.string() + ".partial");
        std::filesystem::copy_file(
            std::filesystem::path(original_paths[index]), partial,
            std::filesystem::copy_options::none, error);
        if (error)
            throw std::runtime_error(
                "cannot snapshot deterministic dataset input: "
                + original_paths[index] + ": " + error.message());
        std::filesystem::rename(partial, destination, error);
        if (error)
            throw std::runtime_error(
                "cannot publish deterministic dataset snapshot: "
                + destination.string() + ": " + error.message());

        const auto original_size = std::filesystem::file_size(
            std::filesystem::path(original_paths[index]), error);
        if (error)
            throw std::runtime_error(
                "cannot stat deterministic dataset input after snapshot: "
                + original_paths[index]);
        const auto snapshot_size = std::filesystem::file_size(
            destination, error);
        if (error)
            throw std::runtime_error(
                "cannot stat deterministic dataset snapshot: "
                + destination.string());
        const std::string original_hash = repro::sha256_file_hex(
            std::filesystem::path(original_paths[index]));
        const std::string snapshot_hash = repro::sha256_file_hex(destination);
        if (original_size != snapshot_size || original_hash != snapshot_hash)
            throw std::runtime_error(
                "dataset changed while creating deterministic input snapshot");

        std::ostringstream logical_name;
        logical_name << "input-" << std::setw(6) << std::setfill('0')
                     << index;
        staged.execution_paths.push_back(destination.string());
        staged.identities.push_back({
            logical_name.str(), original_paths[index],
            static_cast<std::uint64_t>(snapshot_size), snapshot_hash});
    }
    return staged;
}

static std::string deterministic_utc_timestamp(
    std::chrono::system_clock::time_point timestamp)
{
    const auto nanoseconds = std::chrono::duration_cast<
        std::chrono::nanoseconds>(timestamp.time_since_epoch()).count();
    return "unix-ns:" + std::to_string(nanoseconds) + ":UTC";
}

static truetest::reproducibility::DatasetSnapshot
snapshot_deterministic_local_dataset(
    const staged_deterministic_dataset& staged,
    const MarketSeries& series,
    bool is_tick)
{
    namespace repro = truetest::reproducibility;
    if (staged.execution_paths.empty()
        || staged.execution_paths.size() != staged.identities.size()
        || series.empty())
        throw std::invalid_argument(
            "deterministic local dataset and parsed series must not be empty");

    std::vector<repro::DatasetInput> inputs;
    inputs.reserve(staged.execution_paths.size());
    for (std::size_t index = 0; index < staged.execution_paths.size(); ++index)
    {
        inputs.push_back({staged.identities[index].logical_name,
                          staged.execution_paths[index]});
    }

    const auto first_timestamp = is_tick
        ? series.tick_at(0).timestamp : series.bar_at(0).ts;
    const auto last_timestamp = is_tick
        ? series.tick_at(series.tick_count() - 1U).timestamp
        : series.bar_at(series.bar_count() - 1U).ts;
    auto snapshot = repro::snapshot_dataset(
        is_tick ? "local-tick-dataset-v1" : "local-bar-dataset-v1",
        1,
        std::move(inputs),
        deterministic_utc_timestamp(first_timestamp),
        deterministic_utc_timestamp(last_timestamp),
        is_tick ? "timestamp-then-symbol-stable-v1"
                : "timestamp-then-symbol-then-date-stable-v1",
        "normalized-logical-input-order-then-source-row-v1",
        "0-events");
    if (snapshot.artifacts.size() != staged.identities.size())
        throw std::logic_error(
            "deterministic dataset snapshot cardinality changed");
    for (std::size_t index = 0; index < snapshot.artifacts.size(); ++index)
    {
        auto& actual = snapshot.artifacts[index];
        const auto& expected = staged.identities[index];
        if (actual.logical_name != expected.logical_name
            || actual.size_bytes != expected.size_bytes
            || actual.sha256 != expected.sha256)
            throw std::runtime_error(
                "deterministic dataset snapshot changed while being consumed");
        actual.locator = expected.locator;
    }
    snapshot.dataset_id = "sha256:" + snapshot.aggregate_sha256;
    return snapshot;
}

static void validate_deterministic_dataset_symbol(
    const MarketSeries& series,
    bool is_tick,
    std::string_view expected_symbol)
{
    if (expected_symbol.empty())
        throw std::invalid_argument(
            "deterministic dataset requires a nonempty instrument symbol");
    if (is_tick)
    {
        for (std::size_t index = 0; index < series.tick_count(); ++index)
            if (series.tick_at(index).symbol != expected_symbol)
                throw std::invalid_argument(
                    "deterministic tick dataset contains a symbol outside the instrument snapshot");
    }
    else
    {
        for (std::size_t index = 0; index < series.bar_count(); ++index)
            if (series.bar_at(index).symbol != expected_symbol)
                throw std::invalid_argument(
                    "deterministic bar dataset contains a symbol outside the instrument snapshot");
    }
}

static truetest::reproducibility::CanonicalJsonValue
deterministic_local_sanitized_report(const AnalyticsReport& report)
{
    namespace repro = truetest::reproducibility;
    auto payload = repro::parse_json_strict(report.to_results_json());
    auto& object = payload.as_object();
    object.erase("avg_tick_to_trade_ns");
    object.erase("min_tick_to_trade_ns");
    object.erase("max_tick_to_trade_ns");
    object.erase("tick_to_trade_samples");
    auto& open_positions = payload["open_positions"].as_array();
    std::sort(open_positions.begin(), open_positions.end(),
        [](const auto& lhs, const auto& rhs) {
            return truetest::reproducibility::serialize_canonical_json(lhs)
                 < truetest::reproducibility::serialize_canonical_json(rhs);
        });
    return payload;
}

static truetest::reproducibility::CanonicalJsonValue
deterministic_local_report_payload(
    const AnalyticsReport& report,
    std::string_view run_fingerprint)
{
    namespace repro = truetest::reproducibility;
    return repro::CanonicalJsonValue::object({
        {"observability_fields_excluded",
         repro::CanonicalJsonValue::array({
             "avg_tick_to_trade_ns", "max_tick_to_trade_ns",
             "min_tick_to_trade_ns", "tick_to_trade_samples"})},
        {"report", deterministic_local_sanitized_report(report)},
        {"report_artifact_schema_version", std::uint64_t{1}},
        {"run_fingerprint", run_fingerprint},
    });
}

static truetest::reproducibility::CanonicalJsonValue
deterministic_local_economic_payload(const AnalyticsReport& report)
{
    using Json = truetest::reproducibility::CanonicalJsonValue;
    return Json::object({
        {"economic_result_schema_version", std::uint64_t{2}},
        {"included_fields", "all-deterministic-analytics-report-fields-v1"},
        {"report", deterministic_local_sanitized_report(report)},
    });
}

static void finalize_deterministic_local_event_log(
    const deterministic_local_artifact_paths& paths)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(paths.event_log_partial, error)
        || error)
        throw std::runtime_error(
            "deterministic event log was not finalized by the engine: "
            + paths.event_log_partial.string());
    if (std::filesystem::exists(paths.event_log, error))
        throw std::runtime_error(
            "refusing to overwrite deterministic event log: "
            + paths.event_log.string());
    if (error)
        throw std::runtime_error(
            "cannot inspect deterministic event-log destination: "
            + paths.event_log.string());
    std::filesystem::rename(
        paths.event_log_partial, paths.event_log, error);
    if (error)
        throw std::runtime_error(
            "cannot atomically publish deterministic event log: "
            + error.message());
}

static int run_provider_mode(const cli_options& o);

static int run_deterministic_manifest_replay(const cli_options& o)
{
#if TT_TARGET != TT_TARGET_BACKTEST
    (void)o;
    std::cerr << "  ! Deterministic run-manifest replay requires the "
                 "engine_backtest target\n";
    return 2;
#else
    namespace repro = truetest::reproducibility;
    namespace sim = truetest::simulation;

    if (o.replay_run_manifest_path.empty())
        return 1;
    if (!o.write_run_manifest_path.empty())
    {
        std::cerr << "  ! --write-run-manifest and --replay-run-manifest are mutually exclusive\n";
        return 2;
    }
    if (o.reproducibility_artifacts_directory.empty())
    {
        std::cerr << "  ! Manifest replay requires an explicit new --artifacts-dir\n";
        return 2;
    }
    if (!o.replay_path.empty() || !o.event_log_path.empty())
    {
        std::cerr << "  ! Manifest replay cannot be combined with --replay or --log-events\n";
        return 2;
    }
    if (!o.config_file.empty()
        || o.deterministic_replay_input_override_requested)
    {
        std::cerr << "  ! Manifest replay refuses deterministic input overrides; the manifest is authoritative\n";
        return 2;
    }

    const auto started = std::chrono::steady_clock::now();
    repro::RunReceiptV1 receipt;
    receipt.executed_at_utc = reproducibility_utc_now();
    receipt.status = "failed";
    receipt.dataset_mismatch_override_used = false;
    try
    {
        repro::validate_deterministic_runtime_environment();
        const std::filesystem::path manifest_path(o.replay_run_manifest_path);
        const auto manifest = repro::RunManifestV1::load(manifest_path);
        receipt.run_fingerprint = manifest.run_fingerprint();
        const std::string manifest_run_mode = manifest.deterministic_inputs()
            .at("run_mode").as_string();
        if (o.verify_hashes && !manifest.exact_lifecycle_replayable())
            throw std::invalid_argument(
                "--verify-hashes requires an exact lifecycle-replayable manifest");
        if (manifest_run_mode != "monte_carlo"
            && o.deterministic_trial_index_explicitly_set)
            throw std::invalid_argument(
                "--trial is only valid for Monte Carlo run manifests");
        const auto verification = sim::verify_mc_manifest_environment(
            manifest, manifest_path.parent_path());
        if (!verification.build_exact)
        {
            throw std::runtime_error(
                "replay rejected: build identity does not match manifest");
        }
        if (!verification.dataset_exact && !o.allow_dataset_mismatch)
        {
            for (const auto& mismatch : verification.mismatches)
                std::cerr << "    " << mismatch << '\n';
            throw std::runtime_error(
                "replay rejected: dataset identity does not match manifest");
        }
        receipt.dataset_mismatch_override_used =
            !verification.dataset_exact && o.allow_dataset_mismatch;
        if (receipt.dataset_mismatch_override_used)
        {
            std::cerr << "  ! Dataset mismatch override active; replay cannot be exact\n";
            receipt.mismatches.insert(receipt.mismatches.end(),
                verification.mismatches.begin(), verification.mismatches.end());
        }

        if (manifest_run_mode == "backtest")
        {
            auto replay_options = deterministic_local_options_from_manifest(
                manifest, manifest_path.parent_path());
            const auto dataset = repro::dataset_snapshot_from_json(
                manifest.deterministic_inputs().at("dataset"),
                manifest.dataset_locations());
            const auto instrument = deterministic_instrument_snapshot(
                replay_options);
            const auto reconstructed = make_deterministic_local_manifest(
                replay_options, dataset, instrument,
                replay_options.deterministic_lifecycle_record_capacity);
            if (repro::serialize_canonical_json(
                    reconstructed.deterministic_inputs())
                != repro::serialize_canonical_json(
                    manifest.deterministic_inputs()))
                throw std::invalid_argument(
                    "local run manifest contains contradictory deterministic inputs");

            replay_options.deterministic_local_replay = true;
            replay_options.deterministic_run_fingerprint =
                manifest.run_fingerprint();
            replay_options.deterministic_expected_dataset_identity =
                repro::serialize_canonical_json(
                    repro::dataset_identity_json(dataset));
            replay_options.deterministic_expected_hashes =
                manifest.expected_hashes();
            replay_options.verify_hashes = o.verify_hashes;
            replay_options.allow_dataset_mismatch =
                receipt.dataset_mismatch_override_used;
            if (receipt.dataset_mismatch_override_used)
                replay_options.deterministic_dataset_mismatches =
                    verification.mismatches;
            replay_options.reproducibility_artifacts_directory =
                o.reproducibility_artifacts_directory;
            return run_provider_mode(replay_options);
        }

        auto config = sim::mc_config_from_manifest(
            manifest, o.reproducibility_artifacts_directory);
        sim::MonteCarloController controller(config);
        sim::McAggregate results;
        std::vector<std::string> required_hashes;
        if (o.deterministic_trial_index_explicitly_set)
        {
            receipt.verification_scope = "trial";
            receipt.trial_index = static_cast<std::uint64_t>(
                o.deterministic_trial_index);
            auto trial = controller.run_trial(o.deterministic_trial_index);
            results.trials.push_back(std::move(trial));
            sim::summarize_monte_carlo_trials(results);
            required_hashes = {
                trial_hash_name(o.deterministic_trial_index, "event_log"),
                trial_hash_name(o.deterministic_trial_index, "lifecycle"),
                trial_hash_name(o.deterministic_trial_index, "result"),
            };
            if (config.generator_config.emit_synthetic_l2)
                required_hashes.push_back(
                    trial_hash_name(o.deterministic_trial_index,
                                    "synthetic_l2"));
        }
        else
        {
            results = controller.run();
            required_hashes = {"event_log", "economic_result", "report"};
            for (const auto& trial : results.trials)
            {
                required_hashes.push_back(
                    trial_hash_name(trial.trial_id, "event_log"));
                required_hashes.push_back(
                    trial_hash_name(trial.trial_id, "lifecycle"));
                required_hashes.push_back(
                    trial_hash_name(trial.trial_id, "result"));
                if (config.generator_config.emit_synthetic_l2)
                    required_hashes.push_back(
                        trial_hash_name(trial.trial_id, "synthetic_l2"));
            }
        }

        if (o.deterministic_trial_index_explicitly_set)
        {
            const auto& trial = results.trials.front();
            receipt.actual_hashes.emplace(required_hashes[0],
                                           trial.event_log_sha256);
            receipt.actual_hashes.emplace(required_hashes[1],
                                           trial.lifecycle_sha256);
            receipt.actual_hashes.emplace(required_hashes[2],
                                           trial.trial_result_sha256);
            if (config.generator_config.emit_synthetic_l2)
                receipt.actual_hashes.emplace(required_hashes[3],
                                               trial.synthetic_l2_sha256);
        }
        else
        {
            receipt.actual_hashes = deterministic_campaign_hashes(
                results, config);
            const std::string report = sim::deterministic_report_json(
                results, config);
            repro::write_text_file_atomic(
                std::filesystem::path(o.reproducibility_artifacts_directory)
                    / "report.json",
                report);
        }

        if (o.verify_hashes)
        {
            const auto hash_mismatches = compare_expected_hashes(
                manifest.expected_hashes(), receipt.actual_hashes,
                required_hashes);
            receipt.mismatches.insert(receipt.mismatches.end(),
                hash_mismatches.begin(), hash_mismatches.end());
        }

        receipt.exact_reproduction = o.verify_hashes
            && manifest.exact_lifecycle_replayable()
            && verification.exact && receipt.mismatches.empty();
        receipt.status = receipt.mismatches.empty()
            ? "complete" : "hash_mismatch";
        receipt.duration_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
        receipt.write_atomic(
            std::filesystem::path(o.reproducibility_artifacts_directory)
                / "run_receipt.json");
        std::cout << "  Run fingerprint: " << receipt.run_fingerprint << '\n';
        return receipt.mismatches.empty() ? 0 : 4;
    }
    catch (const std::exception& error)
    {
        receipt.mismatches.emplace_back(error.what());
        receipt.duration_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
        std::cerr << "  ! Deterministic manifest replay failed: "
                  << error.what() << '\n';
        try
        {
            receipt.write_atomic(
                std::filesystem::path(o.reproducibility_artifacts_directory)
                    / "run_receipt.json");
        }
        catch (const std::exception& receipt_error)
        {
            std::cerr << "  ! Failed to persist replay receipt: "
                      << receipt_error.what() << '\n';
        }
        return 3;
    }
#endif
}

static double parse_mc_finite_double(std::string_view text,
                                     std::string_view field)
{
    double value = 0.0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value,
        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || !std::isfinite(value))
        throw std::invalid_argument(
            "invalid finite MC parameter '" + std::string(field) + "'");
    return value;
}

static std::int64_t parse_mc_i64(std::string_view text,
                                 std::string_view field)
{
    std::int64_t value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument(
            "invalid integer MC parameter '" + std::string(field) + "'");
    return value;
}

static void apply_mc_generator_parameters(
    std::string_view parameters,
    truetest::simulation::McGeneratorConfig& config)
{
    if (parameters.empty())
        return;
    std::size_t cursor = 0;
    while (cursor <= parameters.size())
    {
        const std::size_t comma = parameters.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos
            ? parameters.size() : comma;
        const std::string_view token = parameters.substr(cursor, end - cursor);
        const std::size_t equals = token.find('=');
        if (token.empty() || equals == std::string_view::npos || equals == 0
            || equals + 1 >= token.size())
            throw std::invalid_argument(
                "MC parameters must be nonempty key=value pairs");
        const std::string_view key = token.substr(0, equals);
        const std::string_view value = token.substr(equals + 1);
        if (key == "n_steps" || key == "n_bars")
            config.n_steps = parse_mc_i64(value, key);
        else if (key == "dt") config.dt = parse_mc_finite_double(value, key);
        else if (key == "mu") config.mu = parse_mc_finite_double(value, key);
        else if (key == "sigma") config.sigma = parse_mc_finite_double(value, key);
        else if (key == "initial_price")
            config.initial_price = parse_mc_finite_double(value, key);
        else if (key == "base_spread_bps" || key == "spread_bps")
            config.base_spread_bps = parse_mc_finite_double(value, key);
        else if (key == "depth_noise")
            config.depth_noise = parse_mc_finite_double(value, key);
        else if (key == "emit_synthetic_l2" || key == "emit_l2")
        {
            if (value == "1" || value == "true")
                config.emit_synthetic_l2 = true;
            else if (value == "0" || value == "false")
                config.emit_synthetic_l2 = false;
            else
                throw std::invalid_argument(
                    "emit_synthetic_l2 must be true|false|1|0");
        }
        else if (key == "symbol") config.symbol = std::string(value);
        else
            throw std::invalid_argument(
                "unknown MC generator parameter: " + std::string(key));
        if (comma == std::string_view::npos)
            break;
        cursor = comma + 1;
    }
    truetest::simulation::validate_mc_generator_config(config);
}

// Monte Carlo campaign runner (Phase 3)
static int run_monte_carlo_mode(const cli_options& o)
{
#if TT_TARGET != TT_TARGET_BACKTEST
    std::cerr << "  ! Monte Carlo execution requires the engine_backtest target\n";
    return 2;
#endif
    if ((!o.mode.empty() && o.mode != "backtest") || o.live
        || o.testnet || o.demo)
    {
        std::cerr << "  ! Monte Carlo execution requires mode=backtest\n";
        return 2;
    }
    try
    {
        truetest::reproducibility::validate_deterministic_runtime_environment();
    }
    catch (const std::exception& error)
    {
        std::cerr << "  ! Monte Carlo deterministic runtime rejected: "
                  << error.what() << '\n';
        return 2;
    }
    if ((!o.provider_name.empty() && o.provider_name != "synthetic"
         && o.provider_name != "montecarlo")
        || !o.provider_path.empty() || !o.depth_stream.empty()
        || !o.stream.empty() || !o.record_path.empty()
        || !o.replay_data_path.empty() || !o.host.empty() || !o.port.empty()
        || !o.api_key.empty() || !o.api_secret.empty()
        || !o.api_passphrase.empty())
    {
        std::cerr << "  ! Monte Carlo uses only its embedded synthetic dataset; "
                     "external provider, path, stream, network, and credential "
                     "inputs are unsupported\n";
        return 2;
    }
    if (!o.format.empty() && o.format != "bar")
    {
        std::cerr << "  ! Monte Carlo currently requires --format bar\n";
        return 2;
    }
    if (o.replay_from_explicitly_set || o.replay_to_explicitly_set
        || o.replay_from_us != 0 || o.replay_to_us != INT64_MAX)
    {
        std::cerr << "  ! Monte Carlo refuses --replay-from/--replay-to; "
                     "synthetic trials do not consume event-log time bounds\n";
        return 2;
    }
    if (o.queue_model != "none")
    {
        std::cerr << "  ! Monte Carlo --queue-model requires an L2 consumer "
                     "and is currently unsupported\n";
        return 2;
    }
    const std::string_view required_trial_preset =
        o.write_run_manifest_path.empty() ? std::string_view{"inline"}
                                          : std::string_view{"logging"};
    if (!o.spin_policy_str.empty()
        || (!o.thread_preset_str.empty()
            && o.thread_preset_str != required_trial_preset))
    {
        std::cerr << "  ! Monte Carlo trial threading is fixed: manifest runs "
                     "use --thread-preset logging and exploratory runs use inline; "
                     "custom thread/spin settings are unsupported\n";
        return 2;
    }
    if (o.fill_rng_seed_explicitly_set)
    {
        std::cerr << "  ! Monte Carlo refuses --fill-rng-seed; fill seeds are "
                     "derived from the explicit master seed\n";
        return 2;
    }
    if (o.spread_step_explicitly_set)
    {
        std::cerr << "  ! Monte Carlo refuses --spread-step because the current "
                     "trial adapter does not consume it\n";
        return 2;
    }
    if (o.mc_workers_explicitly_set && !o.mc_parallel)
    {
        std::cerr << "  ! --mc-workers requires --mc-parallel\n";
        return 2;
    }
    if (o.wire_latency_us != 0)
    {
        std::cerr << "  ! Monte Carlo refuses --wire-latency-us because the "
                     "current local execution adapter does not consume it\n";
        return 2;
    }
    if (o.order_latency_stddev_us > 0 && o.order_latency_us <= 0)
    {
        std::cerr << "  ! --order-latency-stddev-us requires "
                     "--order-latency-us > 0\n";
        return 2;
    }
    if (o.walked_book_impact)
    {
        std::cerr << "  ! Monte Carlo --walked-book-impact requires an "
                     "L2-enabled trial consumer\n";
        return 2;
    }
    if (o.output_format != "json")
    {
        std::cerr << "  ! Monte Carlo output currently supports only JSON\n";
        return 2;
    }
    if (!o.event_log_path.empty() || !o.checkpoint_path.empty()
        || o.checkpoint_interval != 10'000 || o.log_max_size_mb != 0
        || o.log_keep != 5 || !o.compress_log || o.debug_fills
        || o.debug_fills_budget != 20 || o.realistic_fills)
    {
        std::cerr << "  ! Monte Carlo refuses standalone event-log, checkpoint, "
                     "log-rotation, debug-fill, and deprecated fill options; "
                     "manifest mode owns its trial artifacts\n";
        return 2;
    }
    if (!o.preset.empty() && o.preset != "mc-robustness"
        && o.preset != "mc" && o.preset != "monte-carlo")
    {
        std::cerr << "  ! Monte Carlo accepts only the mc-robustness preset "
                     "and its aliases\n";
        return 2;
    }
    if (!o.margin_type.empty() || o.liquidation_warn_pct >= 0.0
        || o.margin_type_strict || o.max_notional_usdt >= 0.0
        || o.max_leverage >= 0.0 || o.min_liquidation_distance_pct >= 0.0
        || o.dead_man_countdown_ms != 30'000
        || o.dead_man_heartbeat_ms != 0 || o.disarm_deadman
        || o.backfill != 500 || !o.backfill_interval.empty()
        || o.reconcile_tolerance_bps != 10.0
        || o.kill_switch_deadline_ms != 5'000)
    {
        std::cerr << "  ! Monte Carlo refuses provider-only backfill, venue-risk, "
                     "reconciliation, kill-switch, and dead-man inputs\n";
        return 2;
    }
    if (!validate_fee_configuration(o, std::cerr)) return 1;
    std::cout << "\n=== Monte Carlo Campaign ===\n";

    truetest::simulation::McRunConfig mc_cfg;
    mc_cfg.n_trials = (o.mc_trials > 0) ? o.mc_trials : 50;
    mc_cfg.generator_name = o.mc_model.empty() ? "gbm" : o.mc_model;

    try
    {
        apply_mc_generator_parameters(o.mc_params, mc_cfg.generator_config);
    }
    catch (const std::exception& error)
    {
        std::cerr << "  ! Invalid --mc-params: " << error.what() << '\n';
        return 2;
    }
    if (!o.symbol.empty())
        mc_cfg.generator_config.symbol = o.symbol;

    if (!o.strategy.empty()) {
        mc_cfg.strategy_name = o.strategy;
    }
    mc_cfg.base_seed = o.seed;
    mc_cfg.master_seed_explicitly_set = o.seed_explicitly_set;
    mc_cfg.initial_balance = o.balance;
    mc_cfg.risk_fraction = o.risk_fraction;
    mc_cfg.sma_period = o.sma_period;
    mc_cfg.rolling_window = o.rolling_window;
    mc_cfg.risk_free_rate = o.risk_free_rate;
    mc_cfg.periods_per_year = o.periods_per_year;
    mc_cfg.max_equity_points = std::max<std::size_t>(
        o.max_equity_points, 4U);
    mc_cfg.execution_bar_delay = o.exec_bar_delay;
    mc_cfg.market_aggression = o.market_aggression;
    mc_cfg.quantity_scale = o.qty_scale;
    const auto exit_mode = truetest::exits::parse_exit_policy_mode(
        o.exit_policy);
    if (!exit_mode)
    {
        std::cerr << "  ! Unknown --exit-policy '" << o.exit_policy
                  << "' (use floor|strategy_only|engine_only|union)\n";
        return 2;
    }
    mc_cfg.exit_defaults.mode = *exit_mode;
    mc_cfg.exit_defaults.sl_pct = o.sl_pct;
    mc_cfg.exit_defaults.tp_pct = o.tp_pct;
    mc_cfg.risk.max_position_value = o.risk_max_position_value;
    mc_cfg.risk.max_drawdown = o.risk_max_drawdown;
    mc_cfg.risk.max_loss_per_trade = o.risk_max_loss_per_trade;
    mc_cfg.risk.max_open_orders = o.risk_max_open_orders;
    mc_cfg.risk.max_portfolio_exposure = o.risk_max_portfolio_exposure;
    mc_cfg.risk.max_gross_leverage = resolve_max_gross_leverage(
        o.risk_max_gross_leverage, "synthetic");
    mc_cfg.risk.max_daily_loss = o.risk_max_daily_loss;
    mc_cfg.risk.daily_reset_hour = o.risk_daily_reset_hour;
    mc_cfg.risk.max_trades_per_hour = o.risk_max_trades_per_hour;
    mc_cfg.risk.max_orders_per_minute = o.risk_max_orders_per_minute;
    mc_cfg.risk.max_symbol_inventory_qty = o.risk_max_inventory_qty;
    mc_cfg.risk.max_funding_8h_rate = o.risk_max_funding_8h_rate;
    mc_cfg.risk.max_mark_age_ms = o.risk_max_mark_age_ms;
    mc_cfg.risk.require_fresh_mark = o.risk_require_fresh_mark;
    mc_cfg.risk_unwind = o.risk_unwind;
    mc_cfg.risk_soft_portfolio_limits = resolve_risk_soft_portfolio_limits(
        false, "backtest", o.risk_soft_portfolio_limits);

    // Strategy --param key=value (same contract as single-run backtests)
    for (const auto& p : o.params) {
        const auto eq = p.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= p.size()
            || p.find('=', eq + 1) != std::string::npos) {
            std::cerr << "  ! Invalid --param format (expected key=value): " << p << "\n";
            return 1;
        }
        try {
            mc_cfg.strategy_params.emplace_back(
                p.substr(0, eq), parse_mc_finite_double(
                    std::string_view(p).substr(eq + 1), p.substr(0, eq)));
        } catch (const std::exception& e) {
            std::cerr << "  ! Invalid --param value for '" << p << "': " << e.what() << "\n";
            return 1;
        }
    }

    // Wire fee/latency/impact + maker-queue into every MC trial (FR-02 / FR-mc-queue).
    mc_cfg.order_latency_us = o.order_latency_us;
    mc_cfg.order_latency_stddev_us = o.order_latency_stddev_us;
    mc_cfg.wire_latency_us = o.wire_latency_us;
    mc_cfg.impact_k_bps = o.impact_k_bps;
    mc_cfg.impact_adv = o.impact_adv;
    mc_cfg.fee_model = o.fee_model;
    mc_cfg.fee_value = o.fee_value;
    mc_cfg.maker_rate = o.maker_rate;
    mc_cfg.taker_rate = o.taker_rate;
    mc_cfg.bar_spread_bps = o.bar_spread_bps;
    mc_cfg.mm_spread_pct = o.mm_spread_pct;   // F-04
    mc_cfg.mm_levels_per_side = o.mm_levels;
    mc_cfg.mm_base_depth = o.mm_base_depth;
    mc_cfg.mm_vol_spread_mult = o.mm_vol_mult;
    mc_cfg.mm_max_half_spread_pct = o.mm_max_spread_pct;
    mc_cfg.fill_probability = o.fill_prob;
    mc_cfg.fill_fade = o.fill_fade;
    mc_cfg.fill_decay = o.fill_decay;

    mc_cfg.instrument.symbol = mc_cfg.generator_config.symbol;
    mc_cfg.instrument.lot_size =
        truetest::simulation::kSyntheticDefaultLotSize;
    mc_cfg.instrument.maker_rate = o.maker_rate;
    mc_cfg.instrument.taker_rate = o.taker_rate;
    if (!o.instrument_specs.empty())
    {
        try
        {
            cli_options instrument_options = o;
            instrument_options.symbol = mc_cfg.generator_config.symbol;
            mc_cfg.instrument = deterministic_instrument_snapshot(
                instrument_options);
        }
        catch (const std::exception& error)
        {
            std::cerr << "  ! Invalid deterministic MC instrument snapshot: "
                      << error.what() << '\n';
            return 2;
        }
    }

    mc_cfg.maker_queue_model = o.maker_queue_model;
    mc_cfg.walked_book_impact = o.walked_book_impact;
    if (mc_cfg.impact_k_bps > 0.0 && !(mc_cfg.impact_adv > 0.0)) {
        std::cerr << "  ! --impact-k-bps requires --impact-adv > 0 (MC would ignore impact)\n";
        return 1;
    }
    if (!mc_cfg.maker_queue_model.empty() && mc_cfg.maker_queue_model != "none"
        && mc_cfg.maker_queue_model != "uniform" && mc_cfg.maker_queue_model != "front"
        && mc_cfg.maker_queue_model != "back") {
        std::cerr << "  ! --maker-queue-model must be none|uniform|front|back\n";
        return 1;
    }
    if (!mc_cfg.maker_queue_model.empty() && mc_cfg.maker_queue_model != "none"
        && o.depth_stream.empty()) {
        std::cerr << "[warn] MC --maker-queue-model without --depth-stream: "
                     "QueueAware joins conservatively (no front-of-queue without L2); "
                     "bar/tick range sweep still fills passive limits\n";
    }

    // Enable richer per-trial data when user asked for --output (for downstream tools / plot.py)
    mc_cfg.keep_full_reports = !o.output.empty();

    // Phase 5 parallel support
    mc_cfg.parallel_trials = o.mc_parallel;
    mc_cfg.max_parallel_threads = o.mc_workers;

    // Phase A object reuse
    mc_cfg.reuse_objects_between_trials = o.mc_reuse_objects;

    if (mc_cfg.parallel_trials && mc_cfg.reuse_objects_between_trials) {
        std::cerr << "  ! --mc-parallel and --mc-reuse-objects are mutually exclusive "
                     "(shared strategy/data_handler would race).\n";
        return 1;
    }

    if (o.deterministic_trial_index_explicitly_set)
    {
        std::cerr << "  ! --trial is only valid with --replay-run-manifest\n";
        return 2;
    }
    if (o.verify_hashes)
    {
        std::cerr << "  ! --verify-hashes is only valid with --replay-run-manifest\n";
        return 2;
    }
    if (o.allow_dataset_mismatch)
    {
        std::cerr << "  ! --allow-dataset-mismatch is only valid with --replay-run-manifest\n";
        return 2;
    }

    namespace repro = truetest::reproducibility;
    namespace sim = truetest::simulation;
    std::optional<repro::RunManifestV1> deterministic_manifest;
    std::filesystem::path deterministic_manifest_path;
    std::filesystem::path deterministic_artifacts;
    const auto deterministic_started = std::chrono::steady_clock::now();
    if (!o.write_run_manifest_path.empty())
    {
        if (!o.output.empty())
        {
            std::cerr << "  ! Deterministic Monte Carlo manifest mode refuses --output; use the canonical hashed artifacts/report.json\n";
            return 2;
        }
        if (!o.event_log_path.empty())
        {
            std::cerr << "  ! --log-events is replaced by per-trial logs in deterministic manifest mode\n";
            return 2;
        }
#ifdef HAS_QUESTDB
        if (o.persist)
        {
            std::cerr << "  ! Deterministic manifest mode refuses external QuestDB persistence\n";
            return 2;
        }
#endif
        deterministic_manifest_path = o.write_run_manifest_path;
        deterministic_artifacts = o.reproducibility_artifacts_directory.empty()
            ? default_reproducibility_artifact_root(
                  deterministic_manifest_path)
            : std::filesystem::path(
                  o.reproducibility_artifacts_directory);
        mc_cfg.persist_trial_lifecycle = true;
        mc_cfg.artifacts_directory = deterministic_artifacts;
        mc_cfg.lifecycle_record_capacity =
            kDeterministicLifecycleRecordCapacityV1;
        try
        {
            require_new_artifact_destination(
                deterministic_manifest_path,
                "deterministic run manifest");
            require_new_artifact_destination(
                deterministic_artifacts / "report.json",
                "deterministic campaign report");
            require_new_artifact_destination(
                deterministic_artifacts / "run_receipt.json",
                "deterministic campaign receipt");
            deterministic_manifest = sim::make_deterministic_mc_manifest(mc_cfg);
            mc_cfg.run_fingerprint =
                deterministic_manifest->run_fingerprint();
        }
        catch (const std::exception& error)
        {
            std::cerr << "  ! Cannot initialize deterministic run manifest: "
                      << error.what() << '\n';
            return 2;
        }
    }

#ifdef HAS_QUESTDB
    std::unique_ptr<truetest::questdb::QuestdbStore> questdb_store;
    if (o.persist) {
        truetest::questdb::StoreConfig sc;
        sc.host = o.questdb_host.empty() ? "127.0.0.1" : o.questdb_host;
        sc.ilp_port = o.questdb_ilp_port ? o.questdb_ilp_port : 9009;
        sc.http_port = o.questdb_http_port ? o.questdb_http_port : 9000;

        try
        {
            const std::string base_tag = o.run_tag.empty()
                ? "mc_" + std::to_string(static_cast<long long>(std::time(nullptr)))
                : o.run_tag;
            sc.run_tag = truetest::questdb::make_run_tag(base_tag);
        }
        catch (const std::exception& e)
        {
            std::cerr << "  ! Invalid --run-tag for QuestDB persistence: "
                      << e.what() << "\n";
            return 1;
        }
        sc.mode = "backtest";
        sc.binary = "engine_backtest";
        sc.strategy = mc_cfg.strategy_name;
        sc.initial_equity = 10000.0;
        sc.notes = o.run_notes;

        questdb_store = std::make_unique<truetest::questdb::QuestdbStore>(sc);
        if (!questdb_store->begin()) {
            std::cerr << "  ! QuestDB persistence failed to start (soft-fail, continuing without persist)\n";
            questdb_store.reset();
        } else {
            std::cout << "  Persist:  QuestDB run_tag=" << sc.run_tag << "\n";
        }
    }
#endif

    std::cout << "  Trials:   " << mc_cfg.n_trials << "\n";
    std::cout << "  Model:    " << mc_cfg.generator_name << "\n";
    std::cout << "  Strategy: " << mc_cfg.strategy_name << "\n";
    std::cout << "  Balance:  " << mc_cfg.initial_balance << "\n";
    std::cout << "  Base seed:" << mc_cfg.base_seed << "\n\n";
    if (mc_cfg.generator_config.emit_synthetic_l2)
        std::cout << "  Synthetic L2: deterministic trial sidecar only; "
                     "not consumed by the bar execution model\n";

    truetest::simulation::McAggregate results;
    try {
        truetest::simulation::MonteCarloController controller(mc_cfg);
        results = controller.run();
    } catch (const std::exception& e) {
        std::cerr << "  ! Monte Carlo campaign failed: " << e.what() << "\n";
        if (deterministic_manifest)
        {
            try
            {
                repro::RunReceiptV1 receipt;
                receipt.executed_at_utc = reproducibility_utc_now();
                receipt.duration_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()
                            - deterministic_started).count());
                receipt.status = "failed";
                receipt.run_fingerprint = mc_cfg.run_fingerprint;
                receipt.mismatches.emplace_back(e.what());
                receipt.write_atomic(
                    deterministic_artifacts / "run_receipt.json");
            }
            catch (const std::exception& receipt_error)
            {
                std::cerr << "  ! Failed to persist deterministic receipt: "
                          << receipt_error.what() << '\n';
            }
        }
        return 1;
    }

    if (deterministic_manifest)
    {
        try
        {
            const auto hashes = deterministic_campaign_hashes(results, mc_cfg);
            const std::string report = sim::deterministic_report_json(
                results, mc_cfg);
            repro::write_text_file_atomic(
                deterministic_artifacts / "report.json", report);
            for (const auto& [kind, hash] : hashes)
                deterministic_manifest->set_expected_hash(kind, hash);
            deterministic_manifest->write_atomic(
                deterministic_manifest_path);

            repro::RunReceiptV1 receipt;
            receipt.executed_at_utc = reproducibility_utc_now();
            receipt.duration_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now()
                        - deterministic_started).count());
            receipt.status = "complete";
            receipt.exact_reproduction = false;
            receipt.run_fingerprint = mc_cfg.run_fingerprint;
            receipt.actual_hashes = hashes;
            receipt.write_atomic(
                deterministic_artifacts / "run_receipt.json");

            std::cout << "  Run fingerprint: " << mc_cfg.run_fingerprint << '\n';
            std::cout << "  Event-log hash:  " << hashes.at("event_log") << '\n';
            std::cout << "  Economic hash:   " << hashes.at("economic_result") << '\n';
            std::cout << "  Report hash:     " << hashes.at("report") << '\n';
            std::cout << "  Manifest:        "
                      << deterministic_manifest_path.string() << '\n';
        }
        catch (const std::exception& error)
        {
            std::cerr << "  ! Deterministic artifact finalization failed: "
                      << error.what() << '\n';
            return 1;
        }
    }

    std::string mc_json = truetest::simulation::MonteCarloReporter::render_json(results, mc_cfg);

    std::cout << truetest::simulation::MonteCarloReporter::render_text_summary(results, mc_cfg);

    // Also print compact JSON for scripting / copy-paste (kept for backward compatibility)
    std::cout << "\nJSON Summary:\n" << mc_json;

    if (!o.output.empty()) {
        // Richer output for --output: keep the flat McAggregate at top-level
        // (so scripts/plot.py --mc continues to work without changes) and add
        // useful metadata + config for downstream tools / analysis.
        nlohmann::json rich = nlohmann::json::parse(mc_json);

        rich["initial_balance"] = mc_cfg.initial_balance;
        rich["order_latency_us"] = mc_cfg.order_latency_us;
        rich["impact_k_bps"] = mc_cfg.impact_k_bps;
        rich["keep_full_reports"] = mc_cfg.keep_full_reports;
        rich["parallel_trials"] = mc_cfg.parallel_trials;
        rich["reuse_objects_between_trials"] = mc_cfg.reuse_objects_between_trials;
        rich["wall_time_ms"] = results.wall_time_ms;

        rich["generator_config"] = nlohmann::json{
            {"symbol", mc_cfg.generator_config.symbol},
            {"initial_price", mc_cfg.generator_config.initial_price},
            {"n_steps", mc_cfg.generator_config.n_steps},
            {"dt", mc_cfg.generator_config.dt},
            {"mu", mc_cfg.generator_config.mu},
            {"sigma", mc_cfg.generator_config.sigma},
            {"emit_synthetic_l2", mc_cfg.generator_config.emit_synthetic_l2},
            {"base_spread_bps", mc_cfg.generator_config.base_spread_bps}
        };

        rich["output_format"] = "mc_aggregate_v1";

        std::ofstream ofs(o.output);
        if (ofs.is_open()) {
            ofs << rich.dump(2) << std::endl;   // pretty-printed for humans + tools
            std::cout << "  MC results exported to: " << o.output << "\n";
        } else {
            std::cerr << "  ! Failed to open output file for writing: " << o.output << "\n";
        }
    }

#ifdef HAS_QUESTDB
    if (questdb_store) {
        // Record campaign summary (lightweight for Phase 4)
        double final_equity = 10000.0 + (results.mean_pnl * results.n_trials); // rough
        questdb_store->end(final_equity,
                           results.n_trials,           // total orders (approx trials here)
                           results.trials_with_positive_pnl,
                           0);
        questdb_store->flush();
    }
#endif

    return 0;
}

// Resolve exchange credentials. Environment variables win over CLI arguments;
// argv-supplied secrets trigger a warning because they leak via `ps` and shell
// history. Env prefix is provider-aware:
//   bitget*  → TRUETEST_BITGET_*
//   bitunix* → TRUETEST_BITUNIX_*
//   else     → TRUETEST_BINANCE_* (historical default for binance / binance-futures)
struct exchange_credentials {
    std::string key;
    std::string secret;
    std::string passphrase;
};

static bool provider_is_bitget(const std::string& name)
{
    return name.rfind("bitget", 0) == 0;
}

static bool provider_is_bitunix(const std::string& name)
{
    return name.rfind("bitunix", 0) == 0;
}

static exchange_credentials resolve_exchange_credentials(
    const std::string& provider_name,
    const std::string& arg_key,
    const std::string& arg_secret,
    const std::string& arg_passphrase)
{
    const bool is_bitget  = provider_is_bitget(provider_name);
    const bool is_bitunix = provider_is_bitunix(provider_name);

    const char* env_key    = nullptr;
    const char* env_secret = nullptr;
    const char* env_pass   = nullptr;
    const char* key_hint = "TRUETEST_BINANCE_API_KEY";
    const char* secret_hint = "TRUETEST_BINANCE_API_SECRET";

    if (is_bitget)
    {
        env_key    = std::getenv("TRUETEST_BITGET_API_KEY");
        env_secret = std::getenv("TRUETEST_BITGET_API_SECRET");
        env_pass   = std::getenv("TRUETEST_BITGET_API_PASSPHRASE");
        key_hint = "TRUETEST_BITGET_API_KEY";
        secret_hint = "TRUETEST_BITGET_API_SECRET";
    }
    else if (is_bitunix)
    {
        env_key    = std::getenv("TRUETEST_BITUNIX_API_KEY");
        env_secret = std::getenv("TRUETEST_BITUNIX_API_SECRET");
        key_hint = "TRUETEST_BITUNIX_API_KEY";
        secret_hint = "TRUETEST_BITUNIX_API_SECRET";
    }
    else
    {
        env_key    = std::getenv("TRUETEST_BINANCE_API_KEY");
        env_secret = std::getenv("TRUETEST_BINANCE_API_SECRET");
    }

    exchange_credentials out;
    out.key        = env_key    ? env_key    : arg_key;
    out.secret     = env_secret ? env_secret : arg_secret;
    out.passphrase = env_pass   ? env_pass   : arg_passphrase;

    if (!arg_key.empty() && !env_key)
        std::cerr << "  WARNING: --api-key was passed on the command line. "
                     "Prefer env var " << key_hint
                  << " - argv is visible to other processes and shell history.\n";
    if (!arg_secret.empty() && !env_secret)
        std::cerr << "  WARNING: --api-secret was passed on the command line. "
                     "Prefer env var " << secret_hint << ".\n";
    if (is_bitget && !arg_passphrase.empty() && !env_pass)
        std::cerr << "  WARNING: --api-passphrase was passed on the command line. "
                     "Prefer env var TRUETEST_BITGET_API_PASSPHRASE.\n";

    return out;
}

static int run_provider_mode(const cli_options& o)
{
    if (!validate_fee_configuration(o, std::cerr)) return 1;
    namespace repro = truetest::reproducibility;
    const bool deterministic_local = o.deterministic_local_replay
        || !o.write_run_manifest_path.empty();
    const auto deterministic_started = std::chrono::steady_clock::now();
    std::optional<deterministic_local_artifact_paths> deterministic_paths;
    std::optional<repro::RunManifestV1> deterministic_manifest;
    std::optional<instrument_spec> deterministic_instrument;
    std::optional<staged_deterministic_dataset> deterministic_dataset;
    std::optional<deterministic_local_receipt_guard>
        deterministic_receipt_guard;

    auto input_paths = split_csv(o.provider_path);
    if (deterministic_local)
    {
        try
        {
#if TT_TARGET != TT_TARGET_BACKTEST
            throw std::invalid_argument(
                "deterministic local runs require the engine_backtest target");
#endif
            repro::validate_deterministic_runtime_environment();
            if (o.provider_name != "local"
                || (!o.mode.empty() && o.mode != "backtest") || o.live)
                throw std::invalid_argument(
                    "deterministic local runs require provider=local and mode=backtest");
            if (!o.seed_explicitly_set)
                throw std::invalid_argument(
                    "deterministic local backtest requires an explicit master seed");
            if (input_paths.empty())
                throw std::invalid_argument(
                    "deterministic local backtest requires at least one --path");
            if (!o.format.empty() && o.format != "bar" && o.format != "tick")
                throw std::invalid_argument(
                    "deterministic local backtest requires --format bar|tick");
            if (!o.thread_preset_str.empty()
                && o.thread_preset_str != "logging")
                throw std::invalid_argument(
                    "deterministic local full persistence requires --thread-preset logging");
            if (!o.event_log_path.empty() || !o.checkpoint_path.empty()
                || !o.resume_path.empty() || !o.record_path.empty()
                || !o.replay_data_path.empty() || !o.depth_stream.empty()
                || !o.stream.empty() || !o.host.empty() || !o.port.empty()
                || o.testnet || o.demo || o.desk || o.web
                || !o.output.empty() || !o.mc_params.empty())
                throw std::invalid_argument(
                    "deterministic local backtest refuses external, UI, checkpoint, output, depth-stream, and network side paths");
#ifdef HAS_QUESTDB
            if (o.persist || o.questdb_strict)
                throw std::invalid_argument(
                    "deterministic local backtest refuses external QuestDB persistence");
#endif
            if (o.queue_model != "none"
                && o.queue_model != "l2-snapshot")
                throw std::invalid_argument(
                    "unsupported deterministic local queue model: "
                    + o.queue_model);
            if (o.queue_model == "l2-snapshot")
                throw std::invalid_argument(
                    "deterministic local backtest cannot use l2-snapshot without a hashed depth dataset");
            if (o.maker_queue_model != "none"
                && o.maker_queue_model != "uniform"
                && o.maker_queue_model != "front"
                && o.maker_queue_model != "back")
                throw std::invalid_argument(
                    "unsupported deterministic local maker queue model: "
                    + o.maker_queue_model);
            if (o.verify_hashes && !o.deterministic_local_replay)
                throw std::invalid_argument(
                    "--verify-hashes requires --replay-run-manifest");
            if (o.allow_dataset_mismatch && !o.deterministic_local_replay)
                throw std::invalid_argument(
                    "--allow-dataset-mismatch requires --replay-run-manifest");
            validate_deterministic_local_effective_config(o);

            for (auto& path : input_paths)
            {
                std::error_code error;
                auto normalized = std::filesystem::absolute(
                    std::filesystem::path(path), error);
                if (error)
                    throw std::invalid_argument(
                        "cannot normalize deterministic dataset path: "
                        + path);
                path = normalized.lexically_normal().generic_string();
            }
            std::sort(input_paths.begin(), input_paths.end());
            if (std::adjacent_find(input_paths.begin(), input_paths.end())
                != input_paths.end())
                throw std::invalid_argument(
                    "deterministic local dataset contains a duplicate path");
            if (o.format == "tick" && input_paths.size() != 1U)
                throw std::invalid_argument(
                    "deterministic tick replay currently requires exactly one input file");

            deterministic_instrument = deterministic_instrument_snapshot(o);
            const std::filesystem::path artifacts =
                o.reproducibility_artifacts_directory.empty()
                    ? default_reproducibility_artifact_root(
                          std::filesystem::path(o.write_run_manifest_path))
                    : std::filesystem::path(
                          o.reproducibility_artifacts_directory);
            deterministic_paths = deterministic_local_paths(artifacts);
            prepare_deterministic_local_artifacts(*deterministic_paths);
            deterministic_receipt_guard.emplace(
                *deterministic_paths, deterministic_started,
                o.allow_dataset_mismatch);
            if (o.deterministic_local_replay)
            {
                deterministic_receipt_guard->set_run_fingerprint(
                    o.deterministic_run_fingerprint);
                deterministic_receipt_guard->set_dataset_mismatches(
                    o.deterministic_dataset_mismatches);
            }
            if (!o.deterministic_local_replay)
                require_new_artifact_destination(
                    std::filesystem::path(o.write_run_manifest_path),
                    "deterministic run manifest");
            deterministic_dataset = stage_deterministic_dataset(
                input_paths, *deterministic_paths);
        }
        catch (const std::exception& error)
        {
            if (deterministic_receipt_guard)
                deterministic_receipt_guard->set_failure(error.what());
            std::cerr << "  ! Cannot initialize deterministic local backtest: "
                      << error.what() << '\n';
            return 2;
        }
    }
    bool run_ok = true;
    const std::vector<std::string>& execution_input_paths =
        deterministic_dataset
            ? deterministic_dataset->execution_paths : input_paths;
    std::string primary_path = execution_input_paths.empty()
        ? o.provider_path : execution_input_paths.front();

    exchange_credentials creds;
    if (!deterministic_local)
        creds = resolve_exchange_credentials(
            o.provider_name, o.api_key, o.api_secret, o.api_passphrase);

    provider_config pcfg;
    if (!primary_path.empty())  pcfg["path"] = primary_path;
    if (!o.symbol.empty())      pcfg["symbol"] = o.symbol;
    if (!o.stream.empty())      pcfg["stream"] = o.stream;
    if (!o.depth_stream.empty()) pcfg["depth_stream"] = o.depth_stream;
    if (!creds.key.empty())        pcfg["api_key"] = creds.key;
    if (!creds.secret.empty())     pcfg["api_secret"] = creds.secret;
    if (!creds.passphrase.empty()) pcfg["api_passphrase"] = creds.passphrase;
    if (!o.host.empty())        pcfg["host"] = o.host;
    if (!o.port.empty())        pcfg["port"] = o.port;
    if (o.testnet)              pcfg["testnet"] = "1";
    if (o.demo)                 pcfg["demo"] = "1";

    if (!o.mc_params.empty())   pcfg["mc_params"] = o.mc_params;
    if (!o.margin_type.empty()) pcfg["margin_type"] = o.margin_type;
    if (o.liquidation_warn_pct >= 0.0)
        pcfg["liquidation_warn_pct"] = std::to_string(o.liquidation_warn_pct);
    if (o.margin_type_strict)   pcfg["margin_type_strict"] = "1";
    if (o.max_notional_usdt >= 0.0)
        pcfg["max_notional_usdt"] = std::to_string(o.max_notional_usdt);
    if (o.max_leverage >= 0.0)
        pcfg["max_leverage"] = std::to_string(o.max_leverage);
    if (o.min_liquidation_distance_pct >= 0.0)
        pcfg["min_liquidation_distance_pct"] =
            std::to_string(o.min_liquidation_distance_pct);
    // --disarm-deadman wins over --dead-man-countdown-ms - operator
    // intent for "this specific run, no DMS" is unambiguous when both
    // appear on the command line.
    const long effective_countdown = o.disarm_deadman ? 0L
                                                      : o.dead_man_countdown_ms;
    pcfg["dead_man_countdown_ms"] = std::to_string(effective_countdown);
    if (o.dead_man_heartbeat_ms > 0)
        pcfg["dead_man_heartbeat_ms"] = std::to_string(o.dead_man_heartbeat_ms);

    // === Monte Carlo mode (Phase 3) ===
    if (o.monte_carlo || o.mc_trials > 0) {
        return run_monte_carlo_mode(o);
    }

    if (!validate_binance_futures_live_phase0(o, std::cerr, std::cout))
        return 1;

    std::shared_ptr<IProvider> provider;
    try {
        provider = ProviderRegistry::instance().create(o.provider_name, pcfg);
    } catch (const std::exception& e) {
        std::cerr << "  ! " << e.what() << "\n";
        if (o.provider_name == "binance")
            std::cerr << "    Example: --provider binance --symbol btcusdt --stream trade\n";
        else if (provider_is_bitget(o.provider_name))
            std::cerr << "    Example: --provider bitget-futures --symbol BTCUSDT "
                         "--stream trade [--demo]\n";
        else if (provider_is_bitunix(o.provider_name))
            std::cerr << "    Example: --provider bitunix-futures --symbol BTCUSDT "
                         "--stream trade\n";
        else if (o.provider_name == "local")
            std::cerr << "    Example: --provider local --path market_data.csv\n";
        else if (o.provider_name == "synthetic" || o.provider_name == "montecarlo")
            std::cerr << "    Example: --provider synthetic --mc-params \"mu=0.0,sigma=0.65,n_steps=2000\"\n";
        else
        {
            std::cerr << "    Available providers:";
            for (const auto& n : ProviderRegistry::instance().available())
                std::cerr << " " << n;
            std::cerr << "\n";
        }
        return 1;
    }

    std::string resolved_raw = o.strategy.empty() ? "mean-reversion" : o.strategy;
    auto strategy_names = split_csv(resolved_raw);
    if (strategy_names.empty()) strategy_names.push_back("mean-reversion");
    const std::string& resolved = strategy_names.front();
    auto prov_strategy = StrategyRegistry::instance().create(resolved);
    // Push platform sizing / protective exits into any strategy that accepts them.
    auto try_set = [](IStrategy& s, const char* key, double v) {
        try { s.set_param(key, v); } catch (const std::exception&) {}
    };
    if (deterministic_local)
        (void)configure_deterministic_local_strategy(
            *prov_strategy, o, resolved, 0U);
    else
    {
        if ((resolved == "sma" || resolved == "mean-reversion")
            && o.sma_period != 20U)
            prov_strategy->set_param(
                "period", static_cast<double>(o.sma_period));
        try_set(*prov_strategy, "equity", o.balance);
        try_set(*prov_strategy, "risk_fraction", o.risk_fraction);
        try_set(*prov_strategy, "sl_pct", o.sl_pct);
        try_set(*prov_strategy, "tp_pct", o.tp_pct);
        apply_execution_cost_params(*prov_strategy, o);
        apply_strategy_params(*prov_strategy, o.params);
    }
    const truetest::reproducibility::DeterministicSeedDeriver strategy_seeds(
        o.seed);
    prov_strategy->reset(strategy_seeds.derive(
        truetest::reproducibility::SeedDomain::strategy, 0U));

    std::vector<std::pair<std::string, std::shared_ptr<IStrategy>>> extra_strategies;
    for (std::size_t si = 1; si < strategy_names.size(); ++si) {
        const auto& nm = strategy_names[si];
        if (!StrategyRegistry::instance().has(nm)) {
            std::cerr << "  ! Unknown additional strategy: " << nm << "\n";
            return 1;
        }
        auto extra = StrategyRegistry::instance().create(nm);
        if (deterministic_local)
            (void)configure_deterministic_local_strategy(
                *extra, o, nm, si);
        else
        {
            try_set(*extra, "equity", o.balance);
            try_set(*extra, "risk_fraction", o.risk_fraction);
            try_set(*extra, "sl_pct", o.sl_pct);
            try_set(*extra, "tp_pct", o.tp_pct);
            apply_execution_cost_params(*extra, o);
            apply_strategy_params(*extra, o.params);
        }
        extra->reset(strategy_seeds.derive(
            truetest::reproducibility::SeedDomain::strategy,
            static_cast<std::uint64_t>(si)));
        extra_strategies.emplace_back(nm, std::move(extra));
    }

    engine_config prov_cfg;
    prov_cfg.seed = o.seed;
    prov_cfg.seed_explicitly_set = o.seed_explicitly_set;
    prov_cfg.event_log_path = o.event_log_path;
    prov_cfg.compress_log = o.compress_log;
    prov_cfg.log_max_bytes = o.log_max_size_mb * 1024ull * 1024ull;
    prov_cfg.log_max_files = o.log_keep;
    prov_cfg.disable_pinning = o.no_pin;
    prov_cfg.provider = provider;
    prov_cfg.initial_balance = o.balance;
    prov_cfg.checkpoint_path = o.checkpoint_path;
    prov_cfg.resume_checkpoint_path = o.resume_path;
    prov_cfg.checkpoint_interval_events = o.checkpoint_interval;
    prov_cfg.backfill_bars = o.backfill;
    prov_cfg.backfill_interval = o.backfill_interval;
    prov_cfg.market_aggression = o.market_aggression;
    prov_cfg.qty_scale = o.qty_scale;
    prov_cfg.fill_rng_seed = o.fill_rng_seed;
    prov_cfg.spread_step_factor = o.spread_step;
    prov_cfg.debug_fills = o.debug_fills;
    prov_cfg.debug_fills_budget = o.debug_fills_budget;
    prov_cfg.rolling_window = o.rolling_window;
    prov_cfg.risk_free_rate = o.risk_free_rate;
    prov_cfg.periods_per_year = o.periods_per_year;
    prov_cfg.max_equity_points = o.max_equity_points;
    prov_cfg.execution_bar_delay = (o.mode == "live" || o.live) ? 0 : o.exec_bar_delay;
    if (deterministic_local)
    {
        prov_cfg.event_log_path =
            deterministic_paths->event_log_partial.string();
        prov_cfg.compress_log = true;
        prov_cfg.log_max_bytes = 0;
        prov_cfg.log_max_files = 1;
        prov_cfg.disable_pinning = true;
        prov_cfg.show_progress = false;
    }
    apply_exit_defaults(prov_cfg, o);
    if (o.wire_latency_us > 0 && o.mode != "live" && !o.live) {
        prov_cfg.wire_latency_model = std::make_shared<FixedLatencyModel>(
            latency_duration(o.wire_latency_us));
    }
    if (o.order_latency_us > 0 && o.mode != "live" && !o.live) {
        if (o.order_latency_stddev_us > 0) {
            prov_cfg.latency_model = std::make_shared<StochasticLatencyModel>(
                static_cast<double>(o.order_latency_us),
                static_cast<double>(o.order_latency_stddev_us),
                o.seed_explicitly_set
                    ? truetest::reproducibility::DeterministicSeedDeriver(o.seed)
                          .derive(truetest::reproducibility::SeedDomain::latency_model)
                    : static_cast<std::uint64_t>(o.fill_rng_seed));
        } else {
            prov_cfg.latency_model = std::make_shared<FixedLatencyModel>(
                latency_duration(o.order_latency_us));
        }
    }
    if (o.impact_k_bps > 0.0 && o.mode != "live" && !o.live) {
        if (!(o.impact_adv > 0.0)) {
            std::cerr << "[error] --impact-k-bps requires --impact-adv > 0\n";
            return 1;
        }
        prov_cfg.impact_model = std::make_shared<SquareRootImpactModel>(
            o.impact_k_bps, o.impact_adv);
    }
    if (apply_sim_realism(prov_cfg, o, o.mode == "live" || o.live) != 0)
        return 1;
    if (o.mode != "live" && !o.live) {
        prov_cfg.walked_book_impact = o.walked_book_impact;
        if (o.queue_model == "l2-snapshot") {
            if (o.depth_stream.empty()) {
                std::cerr << "[error] --queue-model l2-snapshot requires --depth-stream\n";
                return 1;
            }
            prov_cfg.queue_position_model = std::make_shared<L2SnapshotQueueModel>();
        }
        if (o.maker_queue_model != "none") {
            // Depth optional: without L2, no optimistic front-of-queue; bar sweep fills.
            if (o.depth_stream.empty()) {
                std::cerr << "[warn] --maker-queue-model " << o.maker_queue_model
                          << " without --depth-stream: size_ahead is infinite without L2 "
                             "(no optimistic join-front); bar range sweep still fills limits\n";
            }
            if (o.maker_queue_model == "uniform")
                prov_cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
            else if (o.maker_queue_model == "front")
                prov_cfg.maker_queue_model = std::make_shared<FrontCancelModel>();
            else if (o.maker_queue_model == "back")
                prov_cfg.maker_queue_model = std::make_shared<BackCancelModel>();
        }
    }
    prov_cfg.instrument_overrides = parse_instrument_specs(o.instrument_specs);
    prov_cfg.reconcile_tolerance_bps = o.reconcile_tolerance_bps;
    prov_cfg.kill_switch_deadline = std::chrono::milliseconds(o.kill_switch_deadline_ms);

    prov_cfg.risk.max_position_value = o.risk_max_position_value;
    prov_cfg.risk.max_drawdown = o.risk_max_drawdown;
    prov_cfg.risk.max_loss_per_trade = o.risk_max_loss_per_trade;
    prov_cfg.risk.max_open_orders = o.risk_max_open_orders;
    prov_cfg.risk.max_portfolio_exposure = o.risk_max_portfolio_exposure;
    prov_cfg.risk.max_gross_leverage =
        resolve_max_gross_leverage(o.risk_max_gross_leverage, o.provider_name);
    prov_cfg.risk.max_daily_loss = o.risk_max_daily_loss;

    prov_cfg.risk.daily_reset_hour = o.risk_daily_reset_hour;
    prov_cfg.risk.max_trades_per_hour = o.risk_max_trades_per_hour;
    prov_cfg.risk.max_orders_per_minute = o.risk_max_orders_per_minute;
    prov_cfg.risk.max_symbol_inventory_qty = o.risk_max_inventory_qty;
    prov_cfg.risk.max_funding_8h_rate = o.risk_max_funding_8h_rate;
    prov_cfg.risk.max_mark_age_ms = o.risk_max_mark_age_ms;
    prov_cfg.risk.require_fresh_mark = o.risk_require_fresh_mark;
    prov_cfg.risk_unwind = o.risk_unwind;

    // Soft portfolio limits: reject-only in backtest; hard halt in live/shadow.
    // Locked by EngineConfig.RiskSoftPortfolioLimitsResolveByMode unit tests.
    prov_cfg.risk_soft_portfolio_limits =
        resolve_risk_soft_portfolio_limits(o.live, o.mode, o.risk_soft_portfolio_limits);

#ifdef HAS_QUESTDB
    prov_cfg.persist_enabled = o.persist;
    prov_cfg.questdb_host = o.questdb_host;
    prov_cfg.questdb_ilp_port = o.questdb_ilp_port;
    prov_cfg.questdb_http_port = o.questdb_http_port;
    prov_cfg.run_tag = o.run_tag;
    prov_cfg.run_notes = o.run_notes;
    prov_cfg.questdb_flush_cadence = std::chrono::milliseconds(o.questdb_flush_ms);
    prov_cfg.questdb_strict = o.questdb_strict;
    if (o.questdb_strict)
    {
        prov_cfg.persist_enabled = true; // strict implies persist
    }
#endif

    if (o.testnet)
        prov_cfg.backfill_host = "testnet.binance.vision";
    else if (!o.host.empty())
    {
        if (o.host.find("testnet") != std::string::npos)
            prov_cfg.backfill_host = "testnet.binance.vision";
        else
            prov_cfg.backfill_host = "api.binance.com";
    }

    if (deterministic_local)
        prov_cfg.threading = thread_preset::logging_only;
    else if (!o.thread_preset_str.empty())
        prov_cfg.threading = string_to_preset(o.thread_preset_str);
    else
        prov_cfg.threading = select_preset(detect_physical_cores());

    if (!o.spin_policy_str.empty())
        prov_cfg.worker_spin_policy = string_to_spin_policy(o.spin_policy_str);

    if (o.fee_model == "fixed")
        prov_cfg.fee_model = std::make_shared<FixedFeeModel>(o.fee_value);
    else if (o.fee_model == "tiered")
        prov_cfg.fee_model = std::make_shared<TieredFeeModel>(o.maker_rate, o.taker_rate);
    else if (o.fee_model.empty() && o.provider_name == "binance")
        prov_cfg.fee_model = std::make_shared<TieredFeeModel>(0.001, 0.001);

    const bool resolved_live = (o.mode == "live" || o.live);
    if (o.mode == "shadow")
        prov_cfg.mode = engine_mode::shadow;
    else if (resolved_live)
        prov_cfg.mode = engine_mode::live;

    // Ring-buffer drops silently corrupt the shadow portfolio / halt path
    // — unacceptable when real money or real dry-run validation is on the
    // line. Backtest keeps the default (block: deterministic backpressure,
    // no event loss); blocking the hot path against a live feed is worse
    // than halting, so shadow/live escalate drops instead.
    if (prov_cfg.mode != engine_mode::backtest)
        prov_cfg.drop_policy = ring_drop_policy::halt_on_drop;

    if (prov_cfg.mode == engine_mode::live)
    {
        if (!truetest::target_allows_live_orders())
        {
            std::cerr << "  ! Live mode is only permitted on the engine_live binary "
                         "(this is " << truetest::target_name() << ").\n";
            if (o.testnet)
                std::cerr << "    Testnet still uses the live order code path; "
                             "build and run engine_live with --testnet to trade "
                             "on the spot sandbox.\n";
            return 1;
        }
        if (!o.live)
        {
            std::cerr << "  ! Live mode requires --live flag.\n";
            return 1;
        }
        if (creds.key.empty() || creds.secret.empty())
        {
            std::cerr << "  ! Live mode requires credentials via env vars "
                         "(TRUETEST_BINANCE_* / TRUETEST_BITGET_* / "
                         "TRUETEST_BITUNIX_*) or --api-key / --api-secret "
                         "[/--api-passphrase for Bitget].\n";
            return 1;
        }
        if (provider_is_bitget(o.provider_name) && creds.passphrase.empty())
        {
            std::cerr << "  ! Bitget live mode requires API passphrase via "
                         "TRUETEST_BITGET_API_PASSPHRASE or --api-passphrase.\n";
            return 1;
        }
        // Sandbox confirmation is provider-aware: Bitget honors --demo|--testnet
        // as paptrading; Binance only honors --testnet. Never skip mainnet
        // real-money math because of a foreign venue's --demo flag.
        if (futures_live_sandbox_flags(o)
            || (provider_is_bitunix(o.provider_name) && (o.demo || o.testnet)))
        {
            std::cout << "  [SANDBOX] Live mode will submit orders to the "
                         "venue demo/testnet endpoints.\n";
        }
        else
        {
            if (!truetest::bin::confirm_real_money_math())
                return 0;
        }
    }
    else if (provider_is_bitget(o.provider_name) && (o.demo || o.testnet))
    {
        std::cout << "  [DEMO] Routing Bitget provider to UTA paptrading.\n";
    }
    else if (provider_is_bitunix(o.provider_name) && (o.demo || o.testnet))
    {
        std::cout << "  [SANDBOX] Bitunix sandbox flags set (verify venue hosts).\n";
    }
    else if (o.testnet)
    {
        std::cout << "  [TESTNET] Routing Binance provider to the spot testnet.\n";
    }

#ifdef HAS_WEB
    if (o.web && prov_cfg.mode != engine_mode::backtest && o.web_token.empty())
    {
        std::cerr << "  ! --web in shadow/live mode requires --web-token.\n"
                     "    Backtest report review can remain tokenless on localhost.\n";
        return 1;
    }
#endif

    provider->configure(prov_cfg);

    live_safety_requirements safety_requirements;
    safety_requirements.target_allows_private_exchange_writes =
        truetest::target_allows_live_orders();
    safety_requirements.private_exchange_execution_requested =
        prov_cfg.mode == engine_mode::live;
    safety_requirements.reconciler = prov_cfg.reconciler;
    safety_requirements.kill_switch = prov_cfg.kill_switch;
    auto live_safety_session = std::make_shared<LiveSafetySession>(
        provider, std::move(safety_requirements),
        prov_cfg.kill_switch_deadline);
    prov_cfg.live_safety_session = live_safety_session;

    // Install the async-signal-safe notifier before provider open so a signal
    // during bounded startup is latched and the session can close partial
    // venue state instead of relying on the OS default termination path.
    if (prov_cfg.mode != engine_mode::backtest) install_shutdown_handler();
    if (!truetest::bin::open_provider_if_required(
            *provider, *live_safety_session))
    {
        const auto startup_error = live_safety_session->startup_error();
        if (!startup_error.empty())
            std::cerr << "  ! " << startup_error << "\n";
        else
            std::cerr << "  ! Provider failed to open.\n";
        return 1;
    }

    return truetest::bin::run_provider_session_guarded(
        *live_safety_session,
        [&]() -> int {

    auto market_data_feed = provider->get_market_data_feed();
    const bool provider_advertises_event_stream =
        provider->supports_event_stream();
    auto transport = market_data_feed
        ? market_data_feed->transport
        : provider->get_transport();
    if (!transport)
        transport = std::make_shared<FileTransport>(primary_path);

#ifdef HAS_BINANCE
    if (!o.record_path.empty())
        transport = std::make_shared<RecordingTransport>(transport, o.record_path);
    if (!o.replay_data_path.empty())
        transport = std::make_shared<ReplayTransport>(o.replay_data_path);
#endif

    auto dh = std::make_shared<data_handler>();

    // Performance: for local CSV backtests on large files (1.7M+ rows is common
    // for multi-year bar data) we pre-reserve the internal vectors. This is one
    // of the cheapest and most effective wins. 2M is a safe default for 4+ years
    // of 1-minute or tick-aggregated bars.
    if (o.provider_name == "local" || o.provider_name.empty())
        dh->reserve(2'000'000);

    bool is_tick = (o.format == "tick");
    bool is_streaming = transport->is_streaming();
    if (select_market_data_route(
            provider_advertises_event_stream, market_data_feed,
            is_streaming) == market_data_route::invalid)
    {
        // This is inside run_provider_session_guarded(). Throw rather than
        // return so an already-open live provider runs the mandatory
        // kill-before-finish shutdown sequence.
        throw std::runtime_error(
            "streaming provider lacks a complete declared parser capability; "
            "refusing unsafe provider-name/CSV fallback");
    }

    std::cout << "\n";
    std::cout << "  ============================================\n";
    std::cout << "    Provider:  " << o.provider_name << "\n";
    std::cout << "    Format:    " << (is_tick ? "tick" : "bar") << "\n";
    std::cout << "    Strategy:  " << (o.strategy.empty() ? "mean-reversion" : o.strategy) << "\n";
    std::cout << "    SMA:       " << o.sma_period << "\n";
    std::cout << "    Balance:   $" << o.balance << "\n";
    std::cout << "    Risk:      " << (o.risk_fraction * 100) << "% per trade\n";
    std::cout << "    SL/TP:     " << (o.sl_pct * 100) << "% / " << (o.tp_pct * 100)
              << "%  (policy=" << o.exit_policy << ")\n";
    std::cout << "    Fees:      " << (prov_cfg.fee_model ? "yes" : "none") << "\n";
    std::cout << "    Streaming: " << (is_streaming ? "yes" : "no") << "\n";
    print_threading_details(std::cout, "    ",
                            prov_cfg.threading, prov_cfg.worker_spin_policy);
    if (!o.record_path.empty())
        std::cout << "    Recording: " << o.record_path << "\n";
    if (!o.replay_data_path.empty())
        std::cout << "    Replay:    " << o.replay_data_path << "\n";
    std::cout << "  ============================================\n\n";

    // Construct the live dashboard for streaming runs only. In non-streaming
    // modes (batch replay, historical CSV backtest) the engine's own
    // progress printing is fine and we want to avoid competing for stdout
    // with the summary report emitted at the end.
    std::shared_ptr<truetest::ui::ConsoleDashboard> dashboard;
    if (is_streaming)
    {
        truetest::ui::dashboard_config dcfg;
        // --no-tui overrides --status-format.
        std::string sf = o.no_tui ? std::string("plain") : o.status_format;
        // desk_active is NOT set from o.desk here. If --desk is requested but
        // the GL window fails to start, we still want tui/auto to resolve to
        // a terminal TUI (rich or simple). set_desk_active(true) runs only
        // after DeskApp::start() succeeds (see below) so a live desk and the
        // box/ncurses TUI never share the terminal.
        if      (sf == "auto")   dcfg.mode = truetest::ui::output_mode::auto_detect;
        else if (sf == "tui")    dcfg.mode = truetest::ui::output_mode::tui;
        else if (sf == "plain")  dcfg.mode = truetest::ui::output_mode::plain;
        else if (sf == "ndjson") dcfg.mode = truetest::ui::output_mode::ndjson;
        else if (sf == "off")    dcfg.mode = truetest::ui::output_mode::off;
        else {
            std::cerr << "  ! unknown --status-format: " << sf
                      << " (expected auto|tui|plain|ndjson|off)\n";
            return 1;
        }
        dcfg.title = "TrueTest";
        dcfg.target = truetest::target_name();
        dcfg.feed = o.provider_name;
        if (!o.symbol.empty())
        {
            dcfg.feed += "/";
            dcfg.feed += o.symbol;
            if (!o.stream.empty())
            {
                dcfg.feed += "@";
                dcfg.feed += o.stream;
            }
        }
        dcfg.risk_max_open_orders = o.risk_max_open_orders;
        dcfg.risk_max_daily_loss = o.risk_max_daily_loss;
        dcfg.risk_max_position_value = o.risk_max_position_value;
        dcfg.initial_balance = o.balance;

        // One-line threading summary for the dashboard. Mirrors the fuller
        // banner printed above but flattened to fit the ~66 col inner box.
        {
            const std::size_t phys = detect_physical_cores();
            const int threads = preset_worker_count(prov_cfg.threading) + 1;
            auto cmap = build_core_map();
            auto core_of = [&](core_role r) -> int {
                for (const auto& a : cmap) if (a.role == r) return a.core_id;
                return -1;
            };
            std::string pins;
            auto add_pin = [&](core_role r) {
                int c = core_of(r);
                if (c < 0) return;
                if (!pins.empty()) pins += ",";
                pins += std::to_string(c);
            };
            add_pin(core_role::event_loop);
            switch (prov_cfg.threading) {
            case thread_preset::inline_mode: break;
            case thread_preset::logging_only:
                                          add_pin(core_role::logging); break;
            case thread_preset::light:    add_pin(core_role::logging); break;
            case thread_preset::standard: add_pin(core_role::logging);
                                          add_pin(core_role::risk); break;
            case thread_preset::full:     add_pin(core_role::logging);
                                          add_pin(core_role::risk);
                                          add_pin(core_role::stats); break;
            case thread_preset::extended: add_pin(core_role::logging);
                                          add_pin(core_role::risk);
                                          add_pin(core_role::stats);
                                          add_pin(core_role::market_maker); break;
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s  %d thr / %zu cores  [%s]",
                          preset_to_string(prov_cfg.threading).c_str(),
                          threads, phys, pins.c_str());
            dcfg.threading_summary = buf;
        }
        dashboard = std::make_shared<truetest::ui::ConsoleDashboard>(std::move(dcfg));
        prov_cfg.dashboard = dashboard;
        // Re-push the config so the provider can wire the dashboard into
        // its already-created executor. Without this the BinanceExecutor
        // never learns about the dashboard and every paper order falls
        // through to the std::cout fallback in submit_order().
        provider->configure(prov_cfg);
    }

#ifdef HAS_WEB
    // Read-only web UI factory — attaches to an engine via the same
    // snapshot_dashboard() seam the ncurses TUI uses; never places orders.
    auto start_web = [&](engine& eng) -> std::unique_ptr<truetest::web::WebServer> {
        if (!o.web) return nullptr;
        truetest::web::web_config wc;
        wc.bind_addr = o.web_bind;
        wc.port      = o.web_port;
        wc.token     = o.web_token;
        wc.asset_dir = o.web_assets;
        auto snap = [&eng](truetest::ui::dashboard_snapshot& out) {
            bool ok = eng.snapshot_dashboard(out);
            if (ok) out.debug.target = truetest::target_name();
            return ok;
        };
        auto report = [&eng]() {
            return truetest::web::report_to_json(eng.get_analytics().generate_report());
        };
        auto srv = std::make_unique<truetest::web::WebServer>(
            std::move(wc), std::move(snap), std::move(report));
        srv->start();
        return srv;
    };
#endif

    if (is_streaming)
    {
        // engine embeds multi-MiB ObjectPools — heap only (default stack is 8 MiB).
        auto eng = std::make_unique<engine>(dh, nullptr, prov_strategy, std::move(prov_cfg));
        eng->set_primary_strategy_name(resolved);
        for (auto& es : extra_strategies)
            eng->add_strategy(es.second, es.first);

#ifdef HAS_WEB
        // Streams live snapshots over /stream for the duration of the run;
        // stopped when web_srv leaves scope at the end of this branch.
        auto web_srv = start_web(*eng);
#endif

#if defined(HAS_RICH_TUI) || defined(HAS_IMGUI_DESK)
        // Shared operator hooks for rich TUI and/or ImGui desk.
        auto make_operator_actions = [&]() {
            truetest::ui::operator_actions ops;
            ops.pause_toggle = [&eng]() {
                eng->set_pause_all(!eng->is_pause_all());
            };
            ops.pause_state  = [&eng]() { return eng->is_pause_all(); };
            ops.flatten      = [&eng]() { eng->request_flatten(); };
            ops.kill = [&eng](std::chrono::milliseconds dl) {
                return eng->request_operator_kill(dl);
            };
            return ops;
        };

        auto make_snap_fn = [&]() {
            return [&eng](truetest::ui::dashboard_snapshot& out) {
                bool ok = eng->snapshot_dashboard(out);
                if (ok) out.debug.target = truetest::target_name();
                return ok;
            };
        };
#endif

#ifdef HAS_IMGUI_DESK
        // ImGui personal desk — preferred interactive surface when --desk.
        // Does not take over the terminal (windowed); ConsoleDashboard may
        // still run in plain/ndjson mode for logs.
        std::unique_ptr<truetest::ui::desk::DeskApp> imgui_desk;
        if (o.desk)
        {
            imgui_desk = std::make_unique<truetest::ui::desk::DeskApp>(
                make_snap_fn(), dashboard);
            imgui_desk->set_actions(make_operator_actions());
            if (o.desk_demo_data)
                imgui_desk->set_demo_enabled(true);
            std::string title = std::string("TrueTest Desk — ") + truetest::target_name();
            imgui_desk->set_title(std::move(title));
            if (!imgui_desk->start())
            {
                std::cerr << "  ! --desk: failed to start ImGui window; "
                             "falling back to terminal status "
                             "(rich/simple TUI if --status-format allows)\n";
                imgui_desk.reset();
            }
            else if (dashboard)
            {
                // Desk owns the interactive UX — fold tui/auto to plain so
                // ConsoleDashboard does not cursor-hop the same terminal.
                // Only after a successful start(); on failure we leave the
                // original resolve so operators still get a TUI.
                dashboard->set_desk_active(true);
            }
        }
#endif

#ifdef HAS_RICH_TUI
        // Rich (ncurses) tabbed dashboard for shadow/live binaries when the
        // resolved status mode is TUI and the ImGui desk is not active.
        // ncurses takes over the terminal, so we skip render_banner().
        // --simple-tui opts out in favor of ConsoleDashboard's own
        // ncurses-free render_tui() (see dashboard->start() below) - the
        // only way to reach that surface on these binaries, since this
        // condition would otherwise always prefer the rich TUI.
        // After a successful --desk start, set_desk_active(true) made
        // is_tui() false so this branch correctly stays off.
        std::unique_ptr<truetest::ui::TabbedDashboard> rich_tui;
        if (dashboard && truetest::target_uses_rich_tui()
            && dashboard->is_tui()
            && !o.simple_tui
#ifdef HAS_IMGUI_DESK
            && !imgui_desk
#endif
            )
        {
            rich_tui = std::make_unique<truetest::ui::TabbedDashboard>(
                dashboard, make_snap_fn());
            rich_tui->set_actions(make_operator_actions());
        }
#endif

        if (dashboard)
        {
#ifdef HAS_RICH_TUI
            if (rich_tui)
            {
                dashboard->push_event(
                    truetest::ui::event_severity::notice,
                    "engine starting");
                rich_tui->start();
            }
            else
#endif
            {
                // rich_tui is null here - either target_uses_rich_tui() is
                // false, --simple-tui was passed, desk is running (mode
                // folded to plain via set_desk_active), or status was
                // already plain/ndjson/off. dashboard->start() is safe:
                // it runs render_tui()/render_plain()/render_ndjson() per
                // the resolved mode, never fighting a live GL window.
                dashboard->render_banner();
                dashboard->push_event(
                    truetest::ui::event_severity::notice,
                    "engine starting");
                dashboard->start();
            }
        }

        // Unified event-stream path - provider-agnostic. A complete
        // MarketDataFeed carries the matching parser for this public stream;
        // L2 frames populate the engine's orderbook_registry_ directly so
        // LocalBookAdapter can match against real exchange levels. Providers
        // without such a feed stay on the existing specialized bar/tick
        // bridges.
        if (market_data_feed)
        {
            auto parser = market_data_feed->parser;
            auto bridge = std::make_shared<DataBridge<provider::event>>(
                transport, parser,
                [](const provider::event& ev, std::shared_ptr<data_handler> h) {
                    provider::event_sink(ev, std::move(h));
                });

#ifdef HAS_IMGUI_DESK
            // footprint.md §1/§2.1: auto-activate with --desk; --no-footprint
            // disables collection. Installed as an opt-in DataBridge research
            // tap (§2.1) - never on the engine's own event path, never
            // touching eng/provider behavior either way.
            std::shared_ptr<truetest::ui::desk::FootprintLiveSource> footprint_live_source;
            if (o.desk && !o.no_footprint)
            {
                std::optional<double> official_tick_size;
                if (auto spec = provider->get_instrument(o.symbol); spec && spec->tick_size > 0.0)
                    official_tick_size = spec->tick_size;

                std::optional<double> override_tick_size;
                if (!o.footprint_tick_size.empty())
                {
                    try { override_tick_size = std::stod(o.footprint_tick_size); }
                    catch (...) {
                        std::cerr << "  ! --footprint-tick-size: not a number, ignoring\n";
                    }
                }

                const auto resolution = truetest::footprint::resolve_footprint_tick_size(
                    official_tick_size, override_tick_size);
                if (resolution.status == truetest::footprint::tick_size_status::resolved)
                {
                    // Match the actual registered provider names (see
                    // provider_is_binance_futures/provider_is_bitget/
                    // provider_is_bitunix above), not bare "binance"/
                    // "bitget"/"bitunix" - the launch script and the
                    // --provider examples above both use the "-futures"
                    // names, which these exact-string checks used to miss,
                    // silently leaving venue at "unknown" for every
                    // real-world run.
                    truetest::footprint::venue_id venue = truetest::footprint::venue_id::unknown;
                    if (provider_is_binance_futures(o.provider_name)) venue = truetest::footprint::venue_id::binance_usdm;
                    else if (provider_is_bitget(o.provider_name)) venue = truetest::footprint::venue_id::bitget_usdtm;
                    else if (provider_is_bitunix(o.provider_name)) venue = truetest::footprint::venue_id::bitunix_futures;

                    truetest::ui::desk::FootprintLiveSourceConfig fp_cfg;
                    fp_cfg.venue = venue;
                    fp_cfg.symbol_label = o.symbol;
                    fp_cfg.tick_size = resolution.tick_size;
                    fp_cfg.qty_atom_scale = 1e8; // satoshi-like default; per-venue precision is Phase 4b

                    footprint_live_source =
                        std::make_shared<truetest::ui::desk::FootprintLiveSource>(fp_cfg);

#ifdef HAS_BINANCE
                    // Only Binance gets exact-decimal wiring today (native
                    // trade id + string-exact price/qty via
                    // BinanceCombinedParser::configure_exact_decimal(), which
                    // needs the SAME exact string the tick-size resolution
                    // above just validated). Other venues on this event-stream
                    // path still get a working, float-derived footprint tap
                    // for free via try_tap_push()'s fallback conversion.
                    if (!o.footprint_tick_size.empty())
                        if (auto* combined = dynamic_cast<BinanceCombinedParser*>(parser.get()))
                            combined->configure_exact_decimal(o.footprint_tick_size);
#endif

                    bridge->set_research_tap([footprint_live_source](const provider::event& ev) {
                        footprint_live_source->tap(ev);
                    });
                    if (imgui_desk)
                        imgui_desk->set_live_footprint_source(footprint_live_source);
                }
                else if (dashboard)
                {
                    dashboard->push_event(truetest::ui::event_severity::warn,
                        "footprint: tick size unavailable/conflicting for this "
                        "symbol - collection disabled this run");
                }
            }
#endif

            truetest::bin::bridge_shutdown_monitor bridge_monitor(bridge);
            eng->run_streaming(bridge);
        }
        else if (is_tick)
        {
            std::shared_ptr<IDataParser<tick_record>> parser;
#ifdef HAS_BINANCE
            if (o.provider_name == "binance")
                parser = std::make_shared<BinanceTradeParser>();
            else
#endif
                parser = std::make_shared<CsvTickParser>();

            auto bridge = std::make_shared<DataBridge<tick_record>>(
                transport, parser, tick_record_sink);
            truetest::bin::bridge_shutdown_monitor bridge_monitor(bridge);
            eng->run_streaming(bridge);
        }
        else
        {
            std::shared_ptr<IDataParser<bar_record>> parser;
#ifdef HAS_BINANCE
            if (o.provider_name == "binance")
                parser = std::make_shared<BinanceKlineParser>();
            else
#endif
                parser = std::make_shared<CsvBarParser>();

            auto bridge = std::make_shared<DataBridge<bar_record>>(
                transport, parser, bar_record_sink);
            truetest::bin::bridge_shutdown_monitor bridge_monitor(bridge);
            eng->run_streaming(bridge);
        }

        if (dashboard)
        {
#ifdef HAS_RICH_TUI
            if (rich_tui) rich_tui->stop();
            else
#endif
                dashboard->stop();
        }

        const bool shutdown_ok = eng->finalize_live_shutdown(
            live_shutdown_reason::normal_end);
        run_ok = shutdown_ok && eng->run_succeeded();

        // Failed streams and failed shutdowns do not publish a normal
        // completion summary/export; their partial state is diagnostic only.
        // Skip the multi-line analytics report when the TUI owns stdout -
        // the box on screen is already the report surface, and dumping
        // another fixed-width report below it was the "final analysis
        // windows" complaint. Non-TUI runs still print it. File export
        // via --output is unaffected in either case.
        if (run_ok)
        {
            if (!dashboard || !dashboard->is_tui())
                eng->print_summary();
            export_results(eng->get_analytics(), o.output, o.output_format);
        }
    }
    else
    {
        // DR-REPLAY-02: accumulate LoadStats so parse rejects (empty
        // parse_records) and sink validation rejects both reach
        // set_data_rows_rejected — validation_errors() alone misses parse drops.
        LoadStats load_stats;
        if (is_tick)
        {
            std::shared_ptr<IDataParser<tick_record>> parser;
#ifdef HAS_BINANCE
            if (o.provider_name == "binance")
                parser = std::make_shared<BinanceTradeParser>();
            else
#endif
                parser = std::make_shared<CsvTickParser>();

            auto bridge = std::make_shared<DataBridge<tick_record>>(
                transport, parser, tick_record_sink);

            if (!bridge->load_into(*dh, &load_stats))
            {
                std::cerr << "  ! Failed to load tick data via provider bridge.\n";
                return 1;
            }
        }
        else
        {
            std::shared_ptr<IDataParser<bar_record>> parser;
#ifdef HAS_BINANCE
            if (o.provider_name == "binance")
                parser = std::make_shared<BinanceKlineParser>();
            else
#endif
                parser = std::make_shared<CsvBarParser>();

            const auto& paths = execution_input_paths;
            if (paths.size() > 1 && o.provider_name == "local")
            {
                for (const auto& p : paths)
                {
                    auto file_transport = std::make_shared<FileTransport>(p);
                    auto sub_bridge = std::make_shared<DataBridge<bar_record>>(
                        file_transport, parser, bar_record_sink);
                    LoadStats part;
                    if (!sub_bridge->load_into(*dh, &part))
                    {
                        std::cerr << "  ! Failed to load bar data from " << p << "\n";
                        return 1;
                    }
                    load_stats.accepted += part.accepted;
                    load_stats.rejected += part.rejected;
                }
                dh->sort_bars_by_time();
                std::cout << "    Multi-symbol: loaded " << paths.size()
                          << " files, " << dh->bar_count()
                          << " total bars\n";
            }
            else
            {
                auto bridge = std::make_shared<DataBridge<bar_record>>(
                    transport, parser, bar_record_sink);

                if (!bridge->load_into(*dh, &load_stats))
                {
                    std::cerr << "  ! Failed to load bar data via provider bridge.\n";
                    return 1;
                }
                // Match multi-file / API path: always time-order bars (DR-04).
                dh->sort_bars_by_time();
            }
        }

        if (is_tick)
            dh->sort_ticks_by_time();

        const std::size_t data_rejects = load_stats.rejected;
        if (data_rejects > 0)
        {
            std::cerr << "  ! Data load rejected " << data_rejects
                      << " invalid/unparseable row(s)\n";
        }

        bind_symbol_to_series(*dh, o.symbol);

        std::shared_ptr<DeterministicLifecycleSink>
            deterministic_lifecycle_sink;
        std::optional<truetest::engine_support::deterministic_order_id_scope>
            deterministic_order_ids;
        if (deterministic_local)
        {
            if (data_rejects != 0)
                throw std::runtime_error(
                    "deterministic local dataset contains rejected rows");
            validate_deterministic_dataset_symbol(
                *dh, is_tick, o.symbol);
            auto dataset = snapshot_deterministic_local_dataset(
                *deterministic_dataset, *dh, is_tick);
            const std::string actual_dataset_identity =
                repro::serialize_canonical_json(
                    repro::dataset_identity_json(dataset));
            if (o.deterministic_local_replay
                && actual_dataset_identity
                    != o.deterministic_expected_dataset_identity
                && !o.allow_dataset_mismatch)
                throw std::runtime_error(
                    "dataset identity changed between replay verification and engine startup");

            if (!o.deterministic_local_replay)
            {
                deterministic_manifest = make_deterministic_local_manifest(
                    o, dataset, *deterministic_instrument,
                    kDeterministicLifecycleRecordCapacityV1);
            }

            const std::string run_fingerprint = o.deterministic_local_replay
                ? o.deterministic_run_fingerprint
                : deterministic_manifest->run_fingerprint();
            if (!repro::is_lower_hex_sha256(run_fingerprint))
                throw std::runtime_error(
                    "deterministic local run lacks a valid run fingerprint");
            const std::size_t lifecycle_capacity =
                o.deterministic_local_replay
                    ? o.deterministic_lifecycle_record_capacity
                    : kDeterministicLifecycleRecordCapacityV1;
            if (lifecycle_capacity == 0U)
                throw std::invalid_argument(
                    "deterministic local run requires an explicit lifecycle record capacity");
            deterministic_lifecycle_sink =
                std::make_shared<DeterministicLifecycleSink>(
                    lifecycle_capacity);
            prov_cfg.order_audit_sink = deterministic_lifecycle_sink;
            deterministic_order_ids.emplace();
            deterministic_receipt_guard->set_run_fingerprint(
                run_fingerprint);
        }


        // engine embeds multi-MiB ObjectPools — heap only (default stack is 8 MiB).
        auto eng = std::make_unique<engine>(dh, nullptr, prov_strategy, std::move(prov_cfg));
        if (data_rejects > 0)
            eng->set_data_rows_rejected(data_rejects);
        eng->set_primary_strategy_name(resolved);
        for (auto& es : extra_strategies)
            eng->add_strategy(es.second, es.first);

#ifdef HAS_IMGUI_DESK
        // Batch / synthetic runs: open desk so the operator can watch the
        // final snapshot (and mid-run if the batch is long). Window stays
        // up after the run until closed — same review intent as --web.
        std::unique_ptr<truetest::ui::desk::DeskApp> batch_desk;
        if (o.desk)
        {
            auto snap_fn = [&eng](truetest::ui::dashboard_snapshot& out) {
                bool ok = eng->snapshot_dashboard(out);
                if (ok) out.debug.target = truetest::target_name();
                return ok;
            };
            batch_desk = std::make_unique<truetest::ui::desk::DeskApp>(snap_fn);
            truetest::ui::operator_actions ops;
            ops.pause_toggle = [&eng]() {
                eng->set_pause_all(!eng->is_pause_all());
            };
            ops.pause_state = [&eng]() { return eng->is_pause_all(); };
            ops.flatten     = [&eng]() { eng->request_flatten(); };
            batch_desk->set_actions(std::move(ops));
            if (o.desk_demo_data)
                batch_desk->set_demo_enabled(true);
            batch_desk->set_title(std::string("TrueTest Desk — ") + truetest::target_name());
            if (!batch_desk->start())
            {
                std::cerr << "  ! --desk: failed to start ImGui window\n";
                batch_desk.reset();
            }
        }
#endif

        const std::string deterministic_fingerprint = deterministic_local
            ? (o.deterministic_local_replay
                ? o.deterministic_run_fingerprint
                : deterministic_manifest->run_fingerprint())
            : std::string{};

        try
        {
            if (is_tick)
                eng->run_tick_data();
            else
                eng->run();
        }
        catch (const std::exception& error)
        {
            eng.reset();
            if (deterministic_receipt_guard)
                deterministic_receipt_guard->set_failure(error.what());
            throw;
        }
        catch (...)
        {
            eng.reset();
            if (deterministic_receipt_guard)
                deterministic_receipt_guard->set_failure(
                    "unknown deterministic engine failure");
            throw;
        }

        const bool shutdown_ok = eng->finalize_live_shutdown(
            live_shutdown_reason::normal_end);
        run_ok = shutdown_ok && eng->run_succeeded();
        if (deterministic_local && run_ok)
        {
            try
            {
                const AnalyticsReport report =
                    eng->get_analytics().generate_report();
                const auto report_payload = deterministic_local_report_payload(
                    report, deterministic_fingerprint);
                const std::string report_bytes =
                    repro::serialize_canonical_json(report_payload);
                const std::string economic_bytes =
                    repro::serialize_canonical_json(
                        deterministic_local_economic_payload(report));

                // Inline event logging is finalized by run_*(), and reset is
                // an additional RAII boundary before the staging file is
                // atomically published.
                eng.reset();
                finalize_deterministic_local_event_log(*deterministic_paths);
                const std::string lifecycle_hash =
                    deterministic_lifecycle_sink->write_atomic_and_hash(
                        deterministic_paths->lifecycle);
                repro::write_text_file_atomic(
                    deterministic_paths->report, report_bytes);

                std::map<std::string, std::string, std::less<>> hashes{
                    {"economic_result", repro::sha256_hex(economic_bytes)},
                    {"event_log", repro::sha256_file_hex(
                        deterministic_paths->event_log)},
                    {"lifecycle", lifecycle_hash},
                    {"report", repro::sha256_hex(report_bytes)},
                };

                std::vector<std::string> mismatches;
                if (o.deterministic_local_replay && o.verify_hashes)
                    mismatches = compare_expected_hashes(
                        o.deterministic_expected_hashes, hashes,
                        {"event_log", "lifecycle", "economic_result",
                         "report"});
                if (o.allow_dataset_mismatch)
                {
                    mismatches.insert(
                        mismatches.end(),
                        o.deterministic_dataset_mismatches.begin(),
                        o.deterministic_dataset_mismatches.end());
                    if (o.deterministic_dataset_mismatches.empty())
                        mismatches.emplace_back(
                            "dataset mismatch override used");
                }

                if (!o.deterministic_local_replay)
                {
                    for (const auto& [kind, hash] : hashes)
                        deterministic_manifest->set_expected_hash(kind, hash);
                    deterministic_manifest->write_atomic(
                        std::filesystem::path(o.write_run_manifest_path));
                }

                repro::RunReceiptV1 receipt;
                receipt.executed_at_utc = reproducibility_utc_now();
                receipt.duration_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()
                            - deterministic_started).count());
                receipt.status = mismatches.empty()
                    ? "complete" : "hash_mismatch";
                receipt.dataset_mismatch_override_used =
                    o.allow_dataset_mismatch;
                receipt.exact_reproduction = o.deterministic_local_replay
                    && o.verify_hashes && mismatches.empty();
                receipt.run_fingerprint = deterministic_fingerprint;
                receipt.actual_hashes = hashes;
                receipt.mismatches = mismatches;
                receipt.write_atomic(deterministic_paths->receipt);
                deterministic_receipt_guard->mark_receipt_published();

                std::cout << "  Run fingerprint: "
                          << deterministic_fingerprint << '\n';
                std::cout << "  Event-log hash:  "
                          << hashes.at("event_log") << '\n';
                std::cout << "  Lifecycle hash:  "
                          << hashes.at("lifecycle") << '\n';
                std::cout << "  Economic hash:   "
                          << hashes.at("economic_result") << '\n';
                std::cout << "  Report hash:     "
                          << hashes.at("report") << '\n';
                run_ok = mismatches.empty();
            }
            catch (const std::exception& error)
            {
                eng.reset();
                deterministic_receipt_guard->set_failure(error.what());
                std::cerr << "  ! Deterministic local artifact finalization failed: "
                          << error.what() << '\n';
                run_ok = false;
            }
        }
        else if (deterministic_local)
        {
            eng.reset();
            deterministic_receipt_guard->set_failure(
                "deterministic engine run or shutdown failed");
        }
        else if (run_ok)
        {
            eng->print_summary();
            export_results(eng->get_analytics(), o.output, o.output_format);
        }

#ifdef HAS_IMGUI_DESK
        if (batch_desk && batch_desk->is_running())
        {
            std::cerr << "  desk open with final snapshot — close the window to exit\n";
            while (batch_desk->is_running())
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
#endif

#ifdef HAS_WEB
        // Backtest is blocking and done; keep the server alive so the operator
        // can review the final report in the browser until they Ctrl-C.
        if (o.web)
        {
            auto web_srv = start_web(*eng);
            if (web_srv && web_srv->running())
            {
                std::signal(SIGINT,  [](int) { g_web_stop.store(true, std::memory_order_release); });
                std::signal(SIGTERM, [](int) { g_web_stop.store(true, std::memory_order_release); });
                std::cerr << "  web UI serving the backtest report at "
                          << web_srv->config().url() << " — Ctrl-C to exit\n";
                while (!g_web_stop.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                web_srv->stop();
            }
        }
#endif
    }
    return run_ok ? 0 : 1;
        },
        std::cerr);
}

// Small helper extracted so that the initial frame of run_tui_mode stays tiny.
// The banner + first cin + preset logic used to contribute to the ~11 MiB frame.
[[gnu::noinline]]
static int prompt_quickstart_preset(const cli_options& o, cli_options& effective)
{
    std::cout << "\n";
    std::cout << "   _____                _____         _   \n";
    std::cout << "  |_   _| __ _   _  ___|_   _|__  ___| |_ \n";
    std::cout << "    | || '__| | | |/ _ \\ | |/ _ \\/ __| __|\n";
    std::cout << "    | || |  | |_| |  __/ | |  __/\\__ \\ |_ \n";
    std::cout << "    |_||_|   \\__,_|\\___| |_|\\___||___/\\__|\n";
    std::cout << "                                           \n";
    std::cout << "    Backtesting Engine v0.1\n";
    std::cout << "\n";

    // P1-03: lightweight preset-aware entry for the bare invocation. (see docs/todos/02-P1-freeze.md#P1-03)
    // This makes "just run the binary" discover the new simplification story
    // without forcing the user through dozens of flags for common cases.
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Quick start (preset)                     |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  [0]  Classic menu (custom choices)       |\n";
    std::cout << "  |  [1]  Local backtest + L2 realism         |\n";
    std::cout << "  |       (preset: backtest-local-l2)         |\n";
    std::cout << "  |  [2]  Monte Carlo robustness (research)   |\n";
    std::cout << "  |       (preset: mc-robustness)             |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int preset_choice = 0;
    std::cin >> preset_choice;

    effective = o;  // start from passed (may have --param etc.)
    if (preset_choice == 1) {
        effective.preset = "backtest-local-l2";
    } else if (preset_choice == 2) {
        effective.preset = "mc-robustness";
        // MC path will be taken later in main() if mc_trials or monte_carlo is set,
        // but for interactive we let the user continue and we can force MC mode
        // by setting a reasonable default here (or dispatch explicitly).
        if (effective.mc_trials == 0) effective.mc_trials = 100;
    }

    // If a preset was chosen interactively, apply its bundle now (the TUI
    // path builds config manually later; setting the fields + preset name
    // gives provenance and lets any shared logic see it).
    if (!effective.preset.empty() && truetest::presets::is_known_preset(effective.preset)) {
        // We are inside the TUI, no CLI "app" was_set. Apply the bundle
        // unconditionally for the chosen preset (user chose it here).
        // The known bundles only touch the realism/MC fields we care about.
        if (effective.preset == "backtest-local-l2") {
            // (passive-side fill pricing is always on; no flag needed)
            effective.queue_model = "l2-snapshot";
            effective.maker_queue_model = "uniform";
            effective.walked_book_impact = true;
        } else if (effective.preset == "mc-robustness") {
            effective.mc_model = "gbm";
            effective.mc_reuse_objects = true;
        }
    }
    return 0;
}

// Forward declaration so run_tui_mode can call it (definition appears later in this TU).
static int run_tui_configured_engine(const cli_options& o,
                                     const cli_options& effective,
                                     std::unique_ptr<IDataSource> source,
                                     std::shared_ptr<IStrategy> strategy,
                                     int fee_choice);

static int run_tui_mode(const cli_options& o)
{
    // NOTE: run_tui_mode is deliberately kept very small (menus only).
    // The heavy config population, model construction and engine execution
    // live in run_tui_configured_engine (called at the end) and the first
    // preset interaction lives in prompt_quickstart_preset. This prevents
    // the ~11 MiB stack frame that used to crash bare launches on default
    // 8 MiB ulimit -s.
    cli_options effective;
    if (int rc = prompt_quickstart_preset(o, effective); rc != 0)
        return rc;

    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Strategy                                 |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  [1]  Mean Reversion (SMA)                |\n";
    std::cout << "  |  [2]  SMA-based Strategy                  |\n";
    std::cout << "  |  [3]  MA Crossover                        |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int strategy_choice;
    std::cin >> strategy_choice;

    if (strategy_choice < 1 || strategy_choice > 3)
    {
        std::cout << "  ! Invalid choice, defaulting to Mean Reversion.\n";
        strategy_choice = 1;
    }

    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  SMA Period                               |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    std::size_t sma_period = 20;
    std::cin >> sma_period;

    std::string tui_strategy_name = (strategy_choice == 1) ? "mean-reversion"
                                  : (strategy_choice == 2) ? "sma"
                                  : "ma-crossover";
    auto strategy = StrategyRegistry::instance().create(tui_strategy_name);
    if (tui_strategy_name == "sma" || tui_strategy_name == "mean-reversion")
        strategy->set_param("period", static_cast<double>(sma_period));
    apply_execution_cost_params(*strategy, o);
    apply_strategy_params(*strategy, o.params);

    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Data Source                               |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  [1]  CSV File (OHLCV)                     |\n";
    std::cout << "  |  [2]  Tick CSV File                        |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int data_choice;
    std::cin >> data_choice;

    std::unique_ptr<IDataSource> source;

    if (data_choice == 1)
    {
        std::cout << "\n  CSV path: ";
        std::string csv_path;
        std::cin >> csv_path;
        source = std::make_unique<CsvDataSource>(csv_path);
    }
    else if (data_choice == 2)
    {
        std::cout << "\n  Tick CSV path: ";
        std::string tick_path;
        std::cin >> tick_path;
        source = std::make_unique<TickCsvDataSource>(tick_path);
    }
    else
    {
        std::cout << "  ! Invalid choice.\n";
        return 1;
    }

    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Fee Model                                |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  [1]  Zero Fees                           |\n";
    std::cout << "  |  [2]  Fixed Fee Per Trade                 |\n";
    std::cout << "  |  [3]  Tiered (Maker/Taker)                |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int fee_choice;
    std::cin >> fee_choice;

    // Hand off to separate function. This is the main source of the giant
    // stack frame (long config population + models + engine construction).
    return run_tui_configured_engine(o, effective, std::move(source), strategy, fee_choice);
}

// Extracted heavy path. [[gnu::noinline]] + separate frame keeps the
// entry frame of run_tui_mode (and prompt_quickstart_preset) small.
[[gnu::noinline]]
static int run_tui_configured_engine(const cli_options& o,
                                     const cli_options& effective,
                                     std::unique_ptr<IDataSource> source,
                                     std::shared_ptr<IStrategy> strategy,
                                     int fee_choice)
{
    (void)effective; // currently not differing in this path; kept for future

    engine_config config;
    config.seed = o.seed;
    config.seed_explicitly_set = o.seed_explicitly_set;
    config.event_log_path = o.event_log_path;
    config.compress_log = o.compress_log;
    config.log_max_bytes = o.log_max_size_mb * 1024ull * 1024ull;
    config.log_max_files = o.log_keep;
    config.disable_pinning = o.no_pin;
    config.checkpoint_path = o.checkpoint_path;
    config.resume_checkpoint_path = o.resume_path;
    config.checkpoint_interval_events = o.checkpoint_interval;
    config.market_aggression = o.market_aggression;
    config.qty_scale = o.qty_scale;
    config.fill_rng_seed = o.fill_rng_seed;
    config.spread_step_factor = o.spread_step;
    config.debug_fills = o.debug_fills;
    config.debug_fills_budget = o.debug_fills_budget;
    config.rolling_window = o.rolling_window;
    config.risk_free_rate = o.risk_free_rate;
    config.periods_per_year = o.periods_per_year;
    config.max_equity_points = o.max_equity_points;
    config.execution_bar_delay = (o.mode == "live" || o.live) ? 0 : o.exec_bar_delay;
    apply_exit_defaults(config, o);
    if (o.wire_latency_us > 0 && o.mode != "live" && !o.live) {
        config.wire_latency_model = std::make_shared<FixedLatencyModel>(
            latency_duration(o.wire_latency_us));
    }
    if (o.order_latency_us > 0 && o.mode != "live" && !o.live) {
        if (o.order_latency_stddev_us > 0) {
            config.latency_model = std::make_shared<StochasticLatencyModel>(
                static_cast<double>(o.order_latency_us),
                static_cast<double>(o.order_latency_stddev_us),
                o.seed_explicitly_set
                    ? truetest::reproducibility::DeterministicSeedDeriver(o.seed)
                          .derive(truetest::reproducibility::SeedDomain::latency_model)
                    : static_cast<std::uint64_t>(o.fill_rng_seed));
        } else {
            config.latency_model = std::make_shared<FixedLatencyModel>(
                latency_duration(o.order_latency_us));
        }
    } else if (o.order_latency_us > 0 && (o.mode == "live" || o.live)) {
        std::cerr << "[warn] --order-latency-us ignored in live mode\n";
    }
    if (o.impact_k_bps > 0.0 && o.mode != "live" && !o.live) {
        if (!(o.impact_adv > 0.0)) {
            std::cerr << "[error] --impact-k-bps requires --impact-adv > 0\n";
            return 1;
        }
        config.impact_model = std::make_shared<SquareRootImpactModel>(
            o.impact_k_bps, o.impact_adv);
    } else if (o.impact_k_bps > 0.0 && (o.mode == "live" || o.live)) {
        std::cerr << "[warn] --impact-k-bps ignored in live mode\n";
    }
    if (apply_sim_realism(config, o, o.mode == "live" || o.live) != 0)
        return 1;
    if (o.mode != "live" && !o.live) {
        config.walked_book_impact = o.walked_book_impact;
        if (o.queue_model == "l2-snapshot") {
            if (o.depth_stream.empty()) {
                std::cerr << "[error] --queue-model l2-snapshot requires --depth-stream\n";
                return 1;
            }
            config.queue_position_model = std::make_shared<L2SnapshotQueueModel>();
        }
        if (o.maker_queue_model != "none") {
            // Depth optional: without L2, no optimistic front-of-queue; bar sweep fills.
            if (o.depth_stream.empty()) {
                std::cerr << "[warn] --maker-queue-model " << o.maker_queue_model
                          << " without --depth-stream: size_ahead is infinite without L2 "
                             "(no optimistic join-front); bar range sweep still fills limits\n";
            }
            if (o.maker_queue_model == "uniform")
                config.maker_queue_model = std::make_shared<UniformCancelModel>();
            else if (o.maker_queue_model == "front")
                config.maker_queue_model = std::make_shared<FrontCancelModel>();
            else if (o.maker_queue_model == "back")
                config.maker_queue_model = std::make_shared<BackCancelModel>();
        }
    } else if (o.walked_book_impact ||
               o.queue_model != "none" || o.maker_queue_model != "none") {
        std::cerr << "[warn] --walked-book-impact / --queue-model / "
                     "--maker-queue-model ignored in live mode\n";
    }

    config.instrument_overrides = parse_instrument_specs(o.instrument_specs);
    config.reconcile_tolerance_bps = o.reconcile_tolerance_bps;
    config.kill_switch_deadline = std::chrono::milliseconds(o.kill_switch_deadline_ms);

#ifdef HAS_QUESTDB
    config.persist_enabled = o.persist || o.questdb_strict;
    config.questdb_host = o.questdb_host;
    config.questdb_ilp_port = o.questdb_ilp_port;
    config.questdb_http_port = o.questdb_http_port;
    config.run_tag = o.run_tag;
    config.run_notes = o.run_notes;
    config.questdb_flush_cadence =
        std::chrono::milliseconds(o.questdb_flush_ms);
    config.questdb_strict = o.questdb_strict;
#endif

    config.risk.max_position_value = o.risk_max_position_value;
    config.risk.max_drawdown = o.risk_max_drawdown;
    config.risk.max_loss_per_trade = o.risk_max_loss_per_trade;
    config.risk.max_open_orders = o.risk_max_open_orders;
    config.risk.max_portfolio_exposure = o.risk_max_portfolio_exposure;
    config.risk.max_gross_leverage =
        resolve_max_gross_leverage(o.risk_max_gross_leverage, o.provider_name);
    config.risk.max_daily_loss = o.risk_max_daily_loss;

    config.risk.daily_reset_hour = o.risk_daily_reset_hour;
    config.risk.max_trades_per_hour = o.risk_max_trades_per_hour;
    config.risk.max_orders_per_minute = o.risk_max_orders_per_minute;
    config.risk.max_symbol_inventory_qty = o.risk_max_inventory_qty;
    config.risk.max_funding_8h_rate = o.risk_max_funding_8h_rate;
    config.risk.max_mark_age_ms = o.risk_max_mark_age_ms;
    config.risk.require_fresh_mark = o.risk_require_fresh_mark;
    config.risk_unwind = o.risk_unwind;
    // Non-provider path is always engine_backtest binary → soft by default.
    // Still route through the resolver so mode typos / future flags stay consistent.
    config.risk_soft_portfolio_limits =
        resolve_risk_soft_portfolio_limits(/*live_flag=*/false, "backtest", o.risk_soft_portfolio_limits);

    if (!o.thread_preset_str.empty())
        config.threading = string_to_preset(o.thread_preset_str);
    else
        config.threading = select_preset(detect_physical_cores());

    if (!o.spin_policy_str.empty())
        config.worker_spin_policy = string_to_spin_policy(o.spin_policy_str);

    print_threading_details(std::cout, "  ",
                            config.threading, config.worker_spin_policy);

    if (fee_choice == 2)
    {
        std::cout << "  Fee per trade: ";
        double fee;
        std::cin >> fee;
        config.fee_model = std::make_shared<FixedFeeModel>(fee);
    }
    else if (fee_choice == 3)
    {
        std::cout << "  Maker rate (e.g. 0.001): ";
        double maker;
        std::cin >> maker;
        std::cout << "  Taker rate (e.g. 0.002): ";
        double taker;
        std::cin >> taker;
        config.fee_model = std::make_shared<TieredFeeModel>(maker, taker);
    }

    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Engine Mode                              |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  [1]  Backtest (batch data)               |\n";
    std::cout << "  |  Provider shadow/live: use CLI mode       |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int mode_choice;
    std::cin >> mode_choice;

    if (mode_choice != 1)
    {
        std::cerr << "  ! The offline TUI only runs backtests. Use --provider with "
                     "--mode shadow/live so fee and cadence validation runs.\n";
        return 1;
    }

    // Soft portfolio limits were resolved as backtest earlier; re-resolve after
    // the operator picks shadow/live so post-fill never fail-opens there.
    {
        const char* mode_str = "backtest";
        if (config.mode == engine_mode::shadow) mode_str = "shadow";
        else if (config.mode == engine_mode::live) mode_str = "live";
        config.risk_soft_portfolio_limits =
            resolve_risk_soft_portfolio_limits(
                config.mode == engine_mode::live, mode_str, o.risk_soft_portfolio_limits);
    }

    if (config.mode != engine_mode::backtest)
        config.drop_policy = ring_drop_policy::halt_on_drop;

    std::cout << "\n";
    std::cout << "  ============================================\n";
    if (config.mode == engine_mode::backtest)
        std::cout << "    Running backtest...\n";
    else if (config.mode == engine_mode::shadow)
        std::cout << "    Running shadow mode...\n";
    else
        std::cout << "    Running live mode...\n";
    std::cout << "  ============================================\n\n";

    auto dh = std::make_shared<data_handler>();

    // Performance: same pre-reserve for the legacy TUI/API loading path when
    // it goes through CSV data sources.
    dh->reserve(2'000'000);

    if (!source->load_data(dh))
    {
        std::cerr << "  Failed to load data.\n";
        return 1;
    }

    bind_symbol_to_series(*dh, o.symbol);


    // engine embeds multi-MiB ObjectPools — heap only (default stack is 8 MiB).
    auto eng = std::make_unique<engine>(dh, nullptr, strategy, std::move(config));
    eng->run();
    eng->print_summary();
    export_results(eng->get_analytics(), o.output, o.output_format);

    return eng->run_succeeded() ? 0 : 1;
}

static std::optional<std::string> rejected_manifest_replay_argument(
    int argc, char* argv[])
{
    const auto is_value_option = [](std::string_view name) {
        return name == "--replay-run-manifest"
            || name == "--artifacts-dir" || name == "--trial";
    };
    const auto is_flag_option = [](std::string_view name) {
        return name == "--verify-hashes"
            || name == "--allow-dataset-mismatch";
    };

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        const std::size_t equals = argument.find('=');
        const std::string_view name = equals == std::string_view::npos
            ? argument : argument.substr(0, equals);
        if (is_flag_option(name) && equals == std::string_view::npos)
            continue;
        if (is_value_option(name))
        {
            if (equals != std::string_view::npos)
            {
                if (equals + 1U == argument.size())
                    return std::string(argument);
                continue;
            }
            if (index + 1 >= argc)
                return std::string(argument);
            ++index;
            continue;
        }
        return std::string(argument);
    }
    return std::nullopt;
}

int main(int argc, char* argv[])
{
#ifdef HAS_DEBUG
    debug::init();
    static debug::FileSink file_sink("truetest_debug.log");
    absl::AddLogSink(&file_sink);
    LOG(INFO) << "TrueTest debug instrumentation enabled";
#endif

    cli_options opts;

    CLI::App app{"TrueTest - modular C++23 backtesting / shadow / live engine"};
    app.set_help_all_flag("--help-all", "Show all help");
    register_cli_options(app, opts);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (!opts.replay_run_manifest_path.empty())
    {
        if (const auto rejected = rejected_manifest_replay_argument(
                argc, argv))
        {
            std::cerr << "  ! Manifest replay refuses explicit option '"
                      << *rejected
                      << "'; only manifest/artifact/hash-verification options are allowed\n";
            return 2;
        }
    }

    auto option_was_set = [&](const char* name) {
        auto option = app.get_option_no_throw(name);
        return option && option->count() > 0;
    };
    opts.fee_model_explicit = option_was_set("--fee");
    opts.maker_rate_explicit = option_was_set("--maker-rate");
    opts.taker_rate_explicit = option_was_set("--taker-rate");
    opts.periods_per_year_explicit = option_was_set("--periods-per-year");
    opts.seed_explicitly_set = option_was_set("--seed");
    opts.deterministic_trial_index_explicitly_set = option_was_set("--trial");
    opts.replay_from_explicitly_set = option_was_set("--replay-from");
    opts.replay_to_explicitly_set = option_was_set("--replay-to");
    opts.mc_trials_explicitly_set = option_was_set("--mc-trials");
    opts.mc_model_explicitly_set = option_was_set("--mc-model");
    opts.mc_params_explicitly_set = option_was_set("--mc-params");
    opts.mc_workers_explicitly_set = option_was_set("--mc-workers");
    opts.fill_rng_seed_explicitly_set = option_was_set("--fill-rng-seed");
    opts.spread_step_explicitly_set = option_was_set("--spread-step");
    for (const char* deterministic_input_option : {
             "--seed", "--preset", "--provider", "--path", "--strategy",
             "--param", "--monte-carlo", "--mc-trials", "--mc-model",
             "--mc-params", "--mc-parallel", "--mc-workers",
             "--mc-reuse-objects", "--thread-preset", "--fee",
             "--fee-value", "--maker-rate", "--taker-rate", "--symbol",
             "--balance", "--risk-fraction", "--sl", "--tp",
             "--exit-policy", "--exec-bar-delay", "--wire-latency-us",
             "--order-latency-us", "--order-latency-stddev-us",
             "--impact-k-bps", "--impact-adv", "--fill-prob",
             "--fill-fade", "--fill-decay", "--mm-levels",
             "--mm-base-depth", "--mm-spread-pct", "--mm-vol-mult",
             "--mm-max-spread-pct", "--queue-model",
             "--maker-queue-model", "--instrument"})
    {
        if (option_was_set(deterministic_input_option))
        {
            opts.deterministic_replay_input_override_requested = true;
            break;
        }
    }

    if (!opts.log_file_path.empty())
        tt_log::Logger::instance().set_file(opts.log_file_path);
    LOG_INFO("main", "truetest starting (pid=%d)", static_cast<int>(getpid()));
    LOG_INFO("audit",
             "build version=%s sha=%s (%s) built=%s type=%s cxx=%s",
             TRUETEST_VERSION, TRUETEST_GIT_SHA, TRUETEST_GIT_DIRTY,
             TRUETEST_BUILD_TIMESTAMP, TRUETEST_BUILD_TYPE,
             TRUETEST_CXX_COMPILER);
    LOG_INFO("audit",
             "deps cli11=%s zstd=%s nlohmann=%s gtest=%s bench=%s abseil=%s",
             TRUETEST_DEP_CLI11, TRUETEST_DEP_ZSTD, TRUETEST_DEP_NLOHMANN,
             TRUETEST_DEP_GTEST, TRUETEST_DEP_BENCHMARK, TRUETEST_DEP_ABSEIL);

    apply_config_overlay(app, opts);

    // Apply named preset (if any) as a base layer. Only fills fields the user
    // did not explicitly pass on the CLI (mirrors the was_set logic used for
    // --config). This gives: explicit CLI > --config JSON > --preset > defaults.
    if (!opts.preset.empty()) {
        apply_preset_with_precedence(app, opts);
        if (!truetest::presets::is_known_preset(opts.preset)) {
            std::cerr << "  [warn] Unknown preset '" << opts.preset
                      << "'. Run will use explicit flags + defaults only.\n";
        }
    }

    if (!opts.write_run_manifest_path.empty()
        && (opts.dump_config_flag || opts.dry_run
            || !opts.replay_path.empty()))
    {
        std::cerr << "  ! --write-run-manifest cannot be combined with "
                     "--dump-config, --dry-run, or --replay\n";
        return 2;
    }

#if TT_TARGET != TT_TARGET_BACKTEST
    if (!opts.write_run_manifest_path.empty())
    {
        std::cerr << "  ! Deterministic run-manifest generation requires "
                     "the engine_backtest target\n";
        return 2;
    }
#endif

    // A manifest-backed standalone synthetic simulation is the stable-index
    // trial 0 of the existing MC machinery. Reusing that path gives it the
    // same seed hierarchy, lifecycle persistence, hashing, and replay
    // contract without maintaining a second synthetic artifact subsystem.
    if (!opts.write_run_manifest_path.empty()
        && (opts.provider_name == "synthetic"
            || opts.provider_name == "montecarlo")
        && !opts.monte_carlo && opts.mc_trials == 0)
    {
        opts.monte_carlo = true;
        opts.mc_trials = 1;
        std::cout << "  Deterministic synthetic run: using one-trial "
                     "Monte Carlo replay envelope\n";
    }

    if (!opts.resume_path.empty()) {
        std::cerr << "  ! --resume is unavailable: version 1 checkpoints are "
                     "diagnostic portfolio snapshots, not complete deterministic "
                     "engine state.\n";
        return 1;
    }
#ifdef HAS_QUESTDB
    if (opts.questdb_strict && (opts.monte_carlo || opts.mc_trials > 0)) {
        std::cerr << "  ! --persist-strict is unavailable for Monte Carlo: "
                     "campaign persistence does not provide the per-trial "
                     "strict durability contract.\n";
        return 1;
    }
#endif

    if (!opts.replay_run_manifest_path.empty())
        return run_deterministic_manifest_replay(opts);
    if (!opts.write_run_manifest_path.empty()
        && !(opts.monte_carlo || opts.mc_trials > 0)
        && opts.provider_name != "local")
    {
        std::cerr << "  ! --write-run-manifest requires Monte Carlo or --provider local\n";
        return 2;
    }
    if (opts.deterministic_trial_index_explicitly_set)
    {
        std::cerr << "  ! --trial requires --replay-run-manifest\n";
        return 2;
    }

    if (opts.dump_config_flag) { dump_config(opts); return 0; }
    if (opts.dry_run) return run_dry_run(opts);
#if TT_TARGET == TT_TARGET_BACKTEST
    if (!opts.seed_explicitly_set) {
        std::cerr << "  ! Configuration error: Backtest requires an explicit deterministic seed\n";
        return 2;
    }
#endif
    if (!opts.replay_path.empty()) return run_replay_mode(opts);
    if (opts.monte_carlo || opts.mc_trials > 0) return run_monte_carlo_mode(opts);
    if (!opts.provider_name.empty()) return run_provider_mode(opts);
    return run_tui_mode(opts);
}
