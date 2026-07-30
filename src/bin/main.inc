#include <atomic>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <exception>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "core/event_log.h"
#include "core/tt_target.h"
#include "data/data_source.h"
#include "data/csv_data_source.h"
#include "data/binary_cache_source.h"
#include "data/tick_csv_data_source.h"
#include "execution/fee_model.h"
#include "execution/latency_model.h"
#include "orderbook/fill_model.h"
#include "execution/queue_position_model.h"
#include "execution/queue_model.h"
#include "execution/impact_model.h"
#include "orderbook/orderbook.h"
#include "strategy/strategy_registry.h"
#include "strategy/mean_reversion_strategy.h"
#include "strategy/sma_strategy.h"
#include "strategy/ma_crossover_strategy.h"
#include "market_maker/market_maker.h"
#include "threading/thread_config.h"
#include "utils/log/logger.h"
#include <tt/truetest_version.h>

#include "presets/preset.h"

#include "providers/provider_registry.h"
#include "providers/data_bridge.h"
#include "providers/provider_sink.h"
#include "providers/local/csv_parser.h"
#include "providers/local/file_transport.h"

// Monte Carlo support (Phase 2+)
#include "simulation/monte_carlo_controller.h"
#include "simulation/monte_carlo_reporter.h"

#ifdef HAS_QUESTDB
#include "data/questdb/store.h"
#include "data/questdb/run_tag.h"
#endif
#include "ui/console_dashboard.h"
#ifdef HAS_RICH_TUI
#include "ui/dashboard_snapshot.h"
#include "ui/tabbed_dashboard.h"
#endif

#ifdef HAS_BINANCE
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_recorder.h"
#include "providers/binance/binance_replay_transport.h"
#endif

#ifdef HAS_BITGET
#include "providers/bitget/bitget_parser.h"
#endif

#ifdef HAS_DEBUG
#include "debug/debug_log.h"
#endif

#ifdef HAS_WEB
#include "ui/dashboard_snapshot.h"
#include "web/web_server.h"
#include "web/report_json.h"
#endif

static std::atomic<DataBridge<tick_record>*> g_tick_bridge{nullptr};
static std::atomic<DataBridge<bar_record>*> g_bar_bridge{nullptr};
#ifdef HAS_WEB
// Set by SIGINT/SIGTERM so a post-backtest web session can serve the final
// report until the operator exits.
static std::atomic<bool> g_web_stop{false};
#endif
static void install_shutdown_handler()
{
    std::signal(SIGINT, [](int) {
        if (auto* tb = g_tick_bridge.load(std::memory_order_acquire)) tb->stop();
        if (auto* bb = g_bar_bridge.load(std::memory_order_acquire)) bb->stop();
    });
    std::signal(SIGTERM, [](int) {
        if (auto* tb = g_tick_bridge.load(std::memory_order_acquire)) tb->stop();
        if (auto* bb = g_bar_bridge.load(std::memory_order_acquire)) bb->stop();
    });
}

class provider_close_guard
{
public:
    explicit provider_close_guard(std::shared_ptr<IProvider> provider)
        : provider_(std::move(provider))
    {}

    ~provider_close_guard()
    {
        if (active_ && provider_)
        {
            try { provider_->close(); }
            catch (...) {}
        }
    }

    provider_close_guard(const provider_close_guard&) = delete;
    provider_close_guard& operator=(const provider_close_guard&) = delete;

    void release() { active_ = false; }

private:
    std::shared_ptr<IProvider> provider_;
    bool active_ = true;
};

// Live-money safety gate. Prints a loud red warning and forces the operator
// to solve a fresh random addition problem before real orders can be sent.
// Returns true iff the answer was correct.
static bool confirm_real_money_math()
{
    const char* RED  = ::isatty(fileno(stdout)) ? "\033[1;31m" : "";
    const char* YEL  = ::isatty(fileno(stdout)) ? "\033[1;33m" : "";
    const char* OFF  = ::isatty(fileno(stdout)) ? "\033[0m"    : "";

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist(100, 9999);
    int a = dist(rng);
    int b = dist(rng);
    int answer = a + b;

    std::cout << "\n";
    std::cout << RED << "  ###############################################\n";
    std::cout << "  #                                             #\n";
    std::cout << "  #   !!!  LIVE TRADING - REAL MONEY  !!!       #\n";
    std::cout << "  #                                             #\n";
    std::cout << "  #   Orders submitted from here will execute   #\n";
    std::cout << "  #   on a real exchange against real funds.    #\n";
    std::cout << "  #   Losses are real and irreversible.         #\n";
    std::cout << "  #                                             #\n";
    std::cout << "  ###############################################" << OFF << "\n\n";

    std::cout << YEL << "  Solve to confirm: " << a << " + " << b << " = " << OFF;
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line))
    {
        std::cout << "  Aborted (no input).\n";
        return false;
    }

    int given = 0;
    try { given = std::stoi(line); }
    catch (...) {
        std::cout << "  ! Could not parse a number from \"" << line << "\". Aborted.\n";
        return false;
    }

    if (given != answer)
    {
        std::cout << "  ! Wrong answer (expected " << answer << "). Aborted.\n";
        return false;
    }

    std::cout << "  Confirmed. Proceeding with live execution.\n";
    return true;
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
    std::string event_log_path;
    std::string log_file_path;
    std::uint64_t log_max_size_mb = 0;
    int log_keep = 5;
    bool compress_log = true;
    uint64_t seed = 0;
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

    // Phase 3: Full Monte Carlo campaign mode
    bool monte_carlo = false;
    std::size_t mc_trials = 0;
    std::string mc_model;           // gbm, ou, etc.

    // Phase 5: Experimental parallel MC trials (see warnings in controller)
    bool mc_parallel = false;

    // Phase A (MC object reuse): reuse data_handler + strategy (and prepared reset for others)
    bool mc_reuse_objects = false;

    std::size_t sma_period = 20;
    std::vector<std::string> params;
    std::string mode;

    std::string fee_model;
    double fee_value = 0.0;
    double maker_rate = 0.0;
    double taker_rate = 0.0;

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
    bool        dms_attempt_position_close = false;  // Phase 3 DMS hardening

    double balance = 10000.0;
    double risk_fraction = 0.02;
    double sl_pct = 0.005;
    double tp_pct = 0.01;

    std::string checkpoint_path;
    std::string resume_path;
    std::size_t checkpoint_interval = 10000;

    int backfill = 500;
    std::string backfill_interval;

    double market_aggression = 1.1;
    double qty_scale = 1e8;
    unsigned fill_rng_seed = 42;
    double spread_step = 0.0001;

    bool debug_fills = false;
    int debug_fills_budget = 20;

    std::string config_file;
    bool dump_config_flag = false;
    bool dry_run = false;

    std::size_t rolling_window = 252;
    double risk_free_rate = 0.0;
    std::size_t periods_per_year = 252;
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
    double risk_max_daily_loss = 0.0;
    int risk_daily_reset_hour = 0;
    int risk_max_trades_per_hour = 0;
    int risk_max_orders_per_minute = 0;
    bool risk_unwind = false;

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

// Phase-0 live discipline for futures venues (Binance USDT-M + Bitget UTA).
// Mainnet/live without sandbox flags: refuse if no venue caps or daily-loss.
// Sandbox (Binance --testnet; Bitget --demo|--testnet): warn only.
static bool is_futures_live_provider(const std::string& name)
{
    if (name == "binance-futures")
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

static bool validate_futures_live_phase0(const cli_options& o,
                                         std::ostream& errors,
                                         std::ostream& warnings)
{
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

// Push CLI fee / spread realism into strategy sizing params when supported.
// Market entries and stop exits are treated as taker; half bar-spread is a
// conservative adverse-slip estimate when no L2 walk is available at size time.
static void apply_execution_cost_params(IStrategy& strategy, const cli_options& o)
{
    auto schema = strategy.get_param_schema();
    auto has = [&](const char* name) {
        for (const auto& p : schema)
            if (p.name == name) return true;
        return false;
    };
    auto set_if = [&](const char* name, double value) {
        if (value <= 0.0 || !has(name)) return;
        try { strategy.set_param(name, value); }
        catch (const std::exception&) { /* schema race / unknown — ignore */ }
    };

    // Prefer taker for market-style risk; fall back to maker if only that is set.
    const double entry_fee = (o.taker_rate > 0.0) ? o.taker_rate
                           : (o.maker_rate > 0.0) ? o.maker_rate : 0.0;
    const double exit_fee  = entry_fee;
    set_if("entry_fee_rate", entry_fee);
    set_if("exit_fee_rate",  exit_fee);

    // Half-spread as per-leg adverse slip when bar-spread realism is enabled.
    if (o.bar_spread_bps > 0.0)
    {
        const double half = o.bar_spread_bps * 0.5;
        set_if("entry_slip_bps", half);
        set_if("exit_slip_bps",  half);
    }
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
    auto get_uint64 = [&](const char* k, uint64_t& out) {
        if (j.contains(k) && j[k].is_number_unsigned()) out = j[k].get<uint64_t>();
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
    get_uint64("seed", o.seed);
    get_str("thread_preset", o.thread_preset_str);
    get_str("preset", o.preset);
    get_bool("no_pin", o.no_pin);
    get_str("provider", o.provider_name);
    get_str("path", o.provider_path);
    get_bool("monte_carlo", o.monte_carlo);
    get_size("mc_trials", o.mc_trials);
    get_str("mc_model", o.mc_model);
    get_str("mc_params", o.mc_params);
    get_bool("mc_parallel", o.mc_parallel);
    get_bool("mc_reuse_objects", o.mc_reuse_objects);
    get_str("strategy", o.strategy);
    get_str("format", o.format);
    get_size("sma_period", o.sma_period);
    get_str("fee", o.fee_model);
    get_double("fee_value", o.fee_value);
    get_double("maker_rate", o.maker_rate);
    get_double("taker_rate", o.taker_rate);
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
    get_int("backfill", o.backfill);
    get_str("backfill_interval", o.backfill_interval);

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
        rd("max_daily_loss", o.risk_max_daily_loss);
        ri("daily_reset_hour", o.risk_daily_reset_hour);
        ri("max_trades_per_hour", o.risk_max_trades_per_hour);
        ri("max_orders_per_minute", o.risk_max_orders_per_minute);
    }

    get_size("rolling_window", o.rolling_window);
    get_double("risk_free_rate", o.risk_free_rate);
    get_size("periods_per_year", o.periods_per_year);
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
    j["mc_reuse_objects"] = o.mc_reuse_objects;
    j["strategy"] = o.strategy.empty() ? "mean-reversion" : o.strategy;
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
    j["sl"] = o.sl_pct;
    j["tp"] = o.tp_pct;
    j["backfill"] = o.backfill;
    j["backfill_interval"] = o.backfill_interval;
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
    j["risk"] = {
        {"max_position_value", o.risk_max_position_value},
        {"max_drawdown", o.risk_max_drawdown},
        {"max_loss_per_trade", o.risk_max_loss_per_trade},
        {"max_open_orders", o.risk_max_open_orders},
        {"max_portfolio_exposure", o.risk_max_portfolio_exposure},
        {"max_daily_loss", o.risk_max_daily_loss},
        {"daily_reset_hour", o.risk_daily_reset_hour},
        {"max_trades_per_hour", o.risk_max_trades_per_hour},
        {"max_orders_per_minute", o.risk_max_orders_per_minute}
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
    app.add_option("--replay-from", o.replay_from_us, "Replay from timestamp (microseconds since epoch)");
    app.add_option("--replay-to", o.replay_to_us, "Replay to timestamp (microseconds since epoch)");
    app.add_option("--log-events", o.event_log_path, "Path to write binary event log");
    app.add_option("--log-file", o.log_file_path, "Path to write operational log (L1, default: stderr)");
    app.add_option("--log-max-size", o.log_max_size_mb, "Max size (MB) per event/text log before rotation (L3, 0 = no rotation)");
    app.add_option("--log-keep", o.log_keep, "Number of rotated log files to retain (L3, default: 5)");
    app.add_flag("--compress-log,!--no-compress-log", o.compress_log, "Compress binary event logs with zstd (default: on)");
    app.add_option("--seed", o.seed, "RNG seed (0 = non-deterministic)");
    app.add_option("--thread-preset", o.thread_preset_str, "Threading: inline, light, standard, full, extended");
    app.add_option("--preset", o.preset, "Named run preset/profile (e.g. futures-phase0, mc-robustness, backtest-l2, shadow-tape). Provides bundles of realistic defaults for common scenarios; explicit flags and --config override the preset. See --help or docs for the list.");
    app.add_flag("--no-pin", o.no_pin, "Disable CPU affinity pinning");
    app.add_option("--spin-policy", o.spin_policy_str, "Worker spin policy: spin, yield, adaptive (default: adaptive)");
    app.add_option("--provider", o.provider_name,
                   "Provider name: local, binance, binance-futures, bitget, "
                   "bitget-futures, bybit, bybit-futures, gate, gate-futures, "
                   "synthetic");
    app.add_option("--path", o.provider_path, "Data file path (for local provider)");
    app.add_option("--mc-params", o.mc_params, "Synthetic/Monte-Carlo params (e.g. \"mu=0.0,sigma=0.65,n_steps=5000\")");
    app.add_flag("--monte-carlo", o.monte_carlo, "Run Monte Carlo campaign (use with --mc-trials)");
    app.add_option("--mc-trials", o.mc_trials, "Number of independent Monte Carlo trials");
    app.add_option("--mc-model", o.mc_model, "Generator model for Monte Carlo (gbm only; default: gbm)");
    app.add_flag("--mc-parallel", o.mc_parallel, "EXPERIMENTAL: Run MC trials in parallel (Phase 5). Strong warnings apply - see controller docs. Only safe with inline threading preset.");
    app.add_flag("--mc-reuse-objects", o.mc_reuse_objects, "Phase A: Reuse data_handler/strategy (and reset other heavy objects) between MC trials instead of full reconstruction each time.");
    app.add_option("--strategy", o.strategy, "Strategy: mean-reversion, sma, ma-crossover, breakout, coiled-spring, adaptive-hybrid, structure-continuation");
    app.add_option("--format", o.format, "Data format: tick, bar");
    app.add_option("--sma-period", o.sma_period, "SMA indicator period")->default_val(20);
    app.add_option("--param", o.params, "Strategy parameter: key=value (repeatable)");
    app.add_option("--mode", o.mode, "Engine mode: backtest, shadow, live");

    app.add_option("--fee", o.fee_model, "Fee model: fixed, tiered");
    app.add_option("--fee-value", o.fee_value, "Fixed fee per trade");
    app.add_option("--maker-rate", o.maker_rate, "Maker fee rate (tiered)");
    app.add_option("--taker-rate", o.taker_rate, "Taker fee rate (tiered)");

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
        "post_notional / cash <= cap. 0 disables. Default: 0.");
    app.add_option("--min-liq-distance-pct", o.min_liquidation_distance_pct,
        "(futures providers) Minimum projected post-trade buffer "
        "to liquidation, as a fraction (0.05 = 5%). 0 disables. "
        "Default: 0. See docs/futures-testnet.md for caveats on the "
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

    app.add_flag("--dms-attempt-position-close", o.dms_attempt_position_close,
        "(futures providers, Phase 3) When the DMS heartbeat thread "
        "detects persistent failure to refresh the venue countdown, "
        "attempt a position flatten in addition to the venue "
        "order-cancel countdown (Binance: reduceOnly MARKET; Bitget: "
        "close-positions). Hardens DMS into an in-process last-resort "
        "flattener. Pairs with external tt_watchdog for total process "
        "death. Default: false.");

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
    app.add_option("--sl", o.sl_pct, "Stop loss fraction of entry price")->default_val(0.005);
    app.add_option("--tp", o.tp_pct, "Take profit fraction of entry price")->default_val(0.01);

    app.add_option("--checkpoint", o.checkpoint_path,
                   "Write periodic portfolio checkpoint to this binary file");
    app.add_option("--checkpoint-interval", o.checkpoint_interval,
                   "Write a checkpoint every N events (default: 10000)")
       ->default_val(10000);
    app.add_option("--resume", o.resume_path,
                   "Restore portfolio state from a checkpoint before running");

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
        ->default_val(252);
    app.add_option("--max-equity-points", o.max_equity_points,
                   "Max points retained in equity/benchmark curves before decimation")
        ->default_val(100000);
    app.add_option("--exec-bar-delay", o.exec_bar_delay,
                   "Bars of simulated execution delay for backtest/shadow. "
                   "1 (default) parks orders until next bar's open - kills same-bar look-ahead. "
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
                   "optimistic front-of-queue. 'uniform'/'front'/'back' use cancel attribution. "
                   "Requires --depth-stream. Ignored in --mode live.")
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

    app.add_option("--max-daily-loss", o.risk_max_daily_loss, "Maximum daily loss before halt (0 = no limit)")->default_val(0.0);
    app.add_option("--daily-reset-hour", o.risk_daily_reset_hour, "UTC hour (0-23) to reset daily loss counter")->default_val(0);
    app.add_option("--max-trades-per-hour", o.risk_max_trades_per_hour, "Maximum fills per rolling hour (0 = no limit)")->default_val(0);
    app.add_option("--max-orders-per-minute", o.risk_max_orders_per_minute, "Maximum orders per rolling minute (0 = no limit)")->default_val(0);
    app.add_flag("--risk-unwind", o.risk_unwind, "On risk halt, unwind all positions before stopping");
    app.add_option("--output", o.output, "Write results to file (default: stdout)");
    app.add_option("--output-format", o.output_format, "Output format: json or csv")->default_val("json");

    app.add_option("--status-format", o.status_format,
                   "Live status display: auto, tui, plain, ndjson, off (default: auto)")
        ->default_val("auto");
    app.add_flag("--no-tui", o.no_tui, "Shortcut for --status-format=plain");

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
    if (!was_set("--seed")) opts.seed = file_opts.seed;
    if (!was_set("--thread-preset")) opts.thread_preset_str = file_opts.thread_preset_str;
    if (!was_set("--preset")) opts.preset = file_opts.preset;
    if (!was_set("--no-pin")) opts.no_pin = file_opts.no_pin;
    if (!was_set("--provider")) opts.provider_name = file_opts.provider_name;
    if (!was_set("--path")) opts.provider_path = file_opts.provider_path;
    if (!was_set("--strategy")) opts.strategy = file_opts.strategy;
    if (!was_set("--format")) opts.format = file_opts.format;
    if (!was_set("--sma-period")) opts.sma_period = file_opts.sma_period;
    if (!was_set("--fee")) opts.fee_model = file_opts.fee_model;
    if (!was_set("--fee-value")) opts.fee_value = file_opts.fee_value;
    if (!was_set("--maker-rate")) opts.maker_rate = file_opts.maker_rate;
    if (!was_set("--taker-rate")) opts.taker_rate = file_opts.taker_rate;
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
    if (!was_set("--backfill")) opts.backfill = file_opts.backfill;
    if (!was_set("--backfill-interval")) opts.backfill_interval = file_opts.backfill_interval;
    if (!was_set("--rolling-window")) opts.rolling_window = file_opts.rolling_window;
    if (!was_set("--risk-free-rate")) opts.risk_free_rate = file_opts.risk_free_rate;
    if (!was_set("--periods-per-year")) opts.periods_per_year = file_opts.periods_per_year;
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
        // Fraction (0.07 = 7%), matching FuturesRiskCheck / CLI help. Whole
        // percent "7" would reject nearly all risk-increasing orders.
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

static int run_dry_run(const cli_options& o)
{
    if (o.maker_queue_model != "none" && o.depth_stream.empty()) {
        std::cerr << "[error] --maker-queue-model " << o.maker_queue_model
                  << " requires --depth-stream\n";
        return 1;
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
    std::cout << "    SL/TP:      " << (o.sl_pct * 100) << "% / " << (o.tp_pct * 100) << "%\n";
    std::string resolved_spin = o.spin_policy_str.empty() ? "adaptive" : o.spin_policy_str;
    print_threading_details(std::cout, "    ",
                            string_to_preset(resolved_preset),
                            string_to_spin_policy(resolved_spin));
    std::cout << "    Provider:   " << (o.provider_name.empty() ? "(TUI)" : o.provider_name) << "\n";
    std::cout << "    Data path:  " << (o.provider_path.empty() ? "(none)" : o.provider_path) << "\n";
    std::cout << "    Fee model:  " << (o.fee_model.empty() ? "zero" : o.fee_model) << "\n";
    std::cout << "    Seed:       " << (o.seed == 0 ? "random" : std::to_string(o.seed)) << "\n";
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
    if (!validate_futures_live_phase0(o, std::cerr, std::cout))
        valid = false;
    if (!o.fee_model.empty() && o.fee_model != "fixed" && o.fee_model != "tiered")
    {
        std::cerr << "  ! Unknown fee model: " << o.fee_model << "\n";
        valid = false;
    }
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
    std::cout << "\n  Replaying events from: " << o.replay_path << "\n";
    std::cout << "  Using Mean Reversion strategy (default) for replay.\n";

    auto strategy = std::make_shared<mean_reversion_strategy>(20);
    apply_execution_cost_params(*strategy, o);
    apply_strategy_params(*strategy, o.params);

    engine_config cfg;
    cfg.seed = o.seed;
    cfg.event_log_path = o.event_log_path;
    cfg.compress_log = o.compress_log;
    cfg.log_max_bytes = o.log_max_size_mb * 1024ull * 1024ull;
    cfg.log_max_files = o.log_keep;

    auto dh = std::make_shared<data_handler>();
    engine eng(dh, nullptr, strategy, std::move(cfg));
    eng.run_replay(o.replay_path, o.replay_from_us, o.replay_to_us);
    eng.print_summary();
    export_results(eng.get_analytics(), o.output, o.output_format);
    return 0;
}

// Monte Carlo campaign runner (Phase 3)
static int run_monte_carlo_mode(const cli_options& o)
{
    std::cout << "\n=== Monte Carlo Campaign ===\n";

    truetest::simulation::McRunConfig mc_cfg;
    mc_cfg.n_trials = (o.mc_trials > 0) ? o.mc_trials : 50;
    mc_cfg.generator_name = o.mc_model.empty() ? "gbm" : o.mc_model;

    // Parse generator params
    if (!o.mc_params.empty()) {
        // Reuse the small parser logic by populating a temp provider_config style
        // For simplicity we directly apply common keys here
        // (in a real implementation we would share the parser)
        std::istringstream iss(o.mc_params);
        std::string token;
        while (std::getline(iss, token, ',')) {
            auto eq = token.find('=');
            if (eq == std::string::npos) continue;
            std::string key = token.substr(0, eq);
            std::string val = token.substr(eq + 1);
            if (key == "n_steps" || key == "n_bars") {
                try { mc_cfg.generator_config.n_steps = std::stoll(val); } catch (...) {}
            } else if (key == "mu") {
                try { mc_cfg.generator_config.mu = std::stod(val); } catch (...) {}
            } else if (key == "sigma") {
                try { mc_cfg.generator_config.sigma = std::stod(val); } catch (...) {}
            } else if (key == "initial_price") {
                try { mc_cfg.generator_config.initial_price = std::stod(val); } catch (...) {}
            }
        }
    }

    if (!o.strategy.empty()) {
        mc_cfg.strategy_name = o.strategy;
    }
    mc_cfg.base_seed = o.seed;
    mc_cfg.initial_balance = o.balance;

    // Strategy --param key=value (same contract as single-run backtests)
    for (const auto& p : o.params) {
        auto eq = p.find('=');
        if (eq == std::string::npos) {
            std::cerr << "  ! Invalid --param format (expected key=value): " << p << "\n";
            return 1;
        }
        try {
            mc_cfg.strategy_params.emplace_back(
                p.substr(0, eq), std::stod(p.substr(eq + 1)));
        } catch (const std::exception& e) {
            std::cerr << "  ! Invalid --param value for '" << p << "': " << e.what() << "\n";
            return 1;
        }
    }

    // Apply some realism from top-level flags (best effort for Phase 3).
    // Passive-side fill pricing is always on; no flag to forward.
    mc_cfg.order_latency_us = o.order_latency_us;
    mc_cfg.impact_k_bps = o.impact_k_bps;

    // Enable richer per-trial data when user asked for --output (for downstream tools / plot.py)
    mc_cfg.keep_full_reports = !o.output.empty();

    // Phase 5 parallel support
    mc_cfg.parallel_trials = o.mc_parallel;

    // Phase A object reuse
    mc_cfg.reuse_objects_between_trials = o.mc_reuse_objects;

    if (mc_cfg.parallel_trials && mc_cfg.reuse_objects_between_trials) {
        std::cerr << "  ! --mc-parallel and --mc-reuse-objects are mutually exclusive "
                     "(shared strategy/data_handler would race).\n";
        return 1;
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
    std::cout << "  Base seed:" << mc_cfg.base_seed
              << (mc_cfg.base_seed == 0
                      ? " (using fixed default " +
                            std::to_string(truetest::simulation::kMcDefaultBaseSeed) + ")"
                      : "")
              << "\n\n";

    truetest::simulation::McAggregate results;
    try {
        truetest::simulation::MonteCarloController controller(mc_cfg);
        results = controller.run();
    } catch (const std::exception& e) {
        std::cerr << "  ! Monte Carlo campaign failed: " << e.what() << "\n";
        return 1;
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
//   bitget* → TRUETEST_BITGET_*
//   bybit*  → TRUETEST_BYBIT_*
//   gate*   → TRUETEST_GATE_*
//   else    → TRUETEST_BINANCE_* (historical default for binance / binance-futures)
struct exchange_credentials {
    std::string key;
    std::string secret;
    std::string passphrase;
};

static bool provider_is_bitget(const std::string& name)
{
    return name.rfind("bitget", 0) == 0;
}

static bool provider_is_bybit(const std::string& name)
{
    return name.rfind("bybit", 0) == 0;
}

static bool provider_is_gate(const std::string& name)
{
    return name.rfind("gate", 0) == 0;
}

static exchange_credentials resolve_exchange_credentials(
    const std::string& provider_name,
    const std::string& arg_key,
    const std::string& arg_secret,
    const std::string& arg_passphrase)
{
    const bool is_bitget = provider_is_bitget(provider_name);
    const bool is_bybit  = provider_is_bybit(provider_name);
    const bool is_gate   = provider_is_gate(provider_name);

    const char* env_key = nullptr;
    const char* env_secret = nullptr;
    const char* env_pass = nullptr;
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
    else if (is_bybit)
    {
        env_key    = std::getenv("TRUETEST_BYBIT_API_KEY");
        env_secret = std::getenv("TRUETEST_BYBIT_API_SECRET");
        key_hint = "TRUETEST_BYBIT_API_KEY";
        secret_hint = "TRUETEST_BYBIT_API_SECRET";
    }
    else if (is_gate)
    {
        env_key    = std::getenv("TRUETEST_GATE_API_KEY");
        env_secret = std::getenv("TRUETEST_GATE_API_SECRET");
        key_hint = "TRUETEST_GATE_API_KEY";
        secret_hint = "TRUETEST_GATE_API_SECRET";
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
    std::string primary_path = o.provider_path;
    {
        auto comma = primary_path.find(',');
        if (comma != std::string::npos)
            primary_path = primary_path.substr(0, comma);
    }

    const auto creds = resolve_exchange_credentials(
        o.provider_name, o.api_key, o.api_secret, o.api_passphrase);
    const std::string& resolved_key        = creds.key;
    const std::string& resolved_secret     = creds.secret;
    const std::string& resolved_passphrase = creds.passphrase;
    const bool is_bitget = provider_is_bitget(o.provider_name);
    const bool is_bybit  = provider_is_bybit(o.provider_name);
    // Sandbox routing must match pcfg:
    //   bitget: --demo|--testnet → paptrading demo
    //   bybit:  --demo → demo trading hosts; --testnet → testnet hosts
    //   else:   --testnet only (never let --demo alone skip mainnet captcha)
    const bool sandbox = is_bitget ? (o.demo || o.testnet)
                       : is_bybit  ? (o.demo || o.testnet)
                                   : o.testnet;

    if (o.demo && !is_bitget && !is_bybit)
    {
        std::cerr << "  WARNING: --demo is Bitget/Bybit-specific. "
                     "Ignored for provider '" << o.provider_name
                  << "'; use --testnet for Binance sandbox.\n";
    }

    provider_config pcfg;
    if (!primary_path.empty())  pcfg["path"] = primary_path;
    if (!o.symbol.empty())      pcfg["symbol"] = o.symbol;
    if (!o.stream.empty())      pcfg["stream"] = o.stream;
    if (!o.depth_stream.empty()) pcfg["depth_stream"] = o.depth_stream;
    if (!resolved_key.empty())        pcfg["api_key"] = resolved_key;
    if (!resolved_secret.empty())     pcfg["api_secret"] = resolved_secret;
    if (!resolved_passphrase.empty()) pcfg["api_passphrase"] = resolved_passphrase;
    if (!o.host.empty())        pcfg["host"] = o.host;
    if (!o.port.empty())        pcfg["port"] = o.port;
    // Bitget demo/paptrading: --demo or --testnet → pcfg["demo"].
    // Bybit: --demo → demo hosts; --testnet → testnet (distinct environments).
    // Binance keeps pcfg["testnet"] for its testnet hosts.
    if (is_bitget)
    {
        if (o.demo || o.testnet)
            pcfg["demo"] = "1";
    }
    else if (is_bybit)
    {
        if (o.demo)
            pcfg["demo"] = "1";
        else if (o.testnet)
            pcfg["testnet"] = "1";
    }
    else if (o.testnet)
    {
        pcfg["testnet"] = "1";
    }

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
    if (o.dms_attempt_position_close)
        pcfg["dms_attempt_position_close"] = "true";

    // === Monte Carlo mode (Phase 3) ===
    if (o.monte_carlo || o.mc_trials > 0) {
        return run_monte_carlo_mode(o);
    }

    if (!validate_futures_live_phase0(o, std::cerr, std::cout))
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
                         "--stream trade\n";
        else if (provider_is_bybit(o.provider_name))
            std::cerr << "    Example: --provider bybit-futures --symbol BTCUSDT "
                         "--stream trade --testnet\n";
        else if (provider_is_gate(o.provider_name))
            std::cerr << "    Example: --provider gate-futures --symbol BTC_USDT "
                         "--stream trade  (MD/shadow; live orders not wired)\n";
        else if (o.provider_name == "local")
            std::cerr << "    Example: --provider local --path data/market_data.csv\n";
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
    if (resolved == "sma" && o.sma_period != 20)
        prov_strategy->set_param("period", static_cast<double>(o.sma_period));
    if (resolved == "mean-reversion") {
        if (o.sma_period != 20) prov_strategy->set_param("period", static_cast<double>(o.sma_period));
        if (o.balance != 10000.0) prov_strategy->set_param("equity", o.balance);
        if (o.risk_fraction != 0.02) prov_strategy->set_param("risk_fraction", o.risk_fraction);
        if (o.sl_pct != 0.005) prov_strategy->set_param("sl_pct", o.sl_pct);
        if (o.tp_pct != 0.01) prov_strategy->set_param("tp_pct", o.tp_pct);
    }
    if (resolved == "breakout" || resolved == "coiled-spring") {
        if (o.balance != 10000.0) prov_strategy->set_param("equity", o.balance);
        if (o.risk_fraction != 0.02) prov_strategy->set_param("risk_fraction", o.risk_fraction);
    }
    apply_execution_cost_params(*prov_strategy, o);
    apply_strategy_params(*prov_strategy, o.params);

    std::vector<std::pair<std::string, std::shared_ptr<IStrategy>>> extra_strategies;
    for (std::size_t si = 1; si < strategy_names.size(); ++si) {
        const auto& nm = strategy_names[si];
        if (!StrategyRegistry::instance().has(nm)) {
            std::cerr << "  ! Unknown additional strategy: " << nm << "\n";
            return 1;
        }
        auto extra = StrategyRegistry::instance().create(nm);
        apply_execution_cost_params(*extra, o);
        apply_strategy_params(*extra, o.params);
        extra_strategies.emplace_back(nm, std::move(extra));
    }

    engine_config prov_cfg;
    prov_cfg.seed = o.seed;
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
    if (o.wire_latency_us > 0 && o.mode != "live" && !o.live) {
        prov_cfg.wire_latency_model = std::make_shared<FixedLatencyModel>(
            latency_duration(o.wire_latency_us));
    }
    if (o.order_latency_us > 0 && o.mode != "live" && !o.live) {
        if (o.order_latency_stddev_us > 0) {
            prov_cfg.latency_model = std::make_shared<StochasticLatencyModel>(
                static_cast<double>(o.order_latency_us),
                static_cast<double>(o.order_latency_stddev_us),
                o.fill_rng_seed);
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
            if (o.depth_stream.empty()) {
                std::cerr << "[error] --maker-queue-model " << o.maker_queue_model
                          << " requires --depth-stream\n";
                return 1;
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
    prov_cfg.risk.max_daily_loss = o.risk_max_daily_loss;
    prov_cfg.risk.daily_reset_hour = o.risk_daily_reset_hour;
    prov_cfg.risk.max_trades_per_hour = o.risk_max_trades_per_hour;
    prov_cfg.risk.max_orders_per_minute = o.risk_max_orders_per_minute;
    prov_cfg.risk_unwind = o.risk_unwind;

    const bool resolved_live = (o.mode == "live" || o.live);

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

    // Binance-only REST backfill hosts; Bitget backfill is provider-owned.
    if (!is_bitget)
    {
        if (o.testnet)
            prov_cfg.backfill_host = "testnet.binance.vision";
        else if (!o.host.empty())
        {
            if (o.host.find("testnet") != std::string::npos)
                prov_cfg.backfill_host = "testnet.binance.vision";
            else
                prov_cfg.backfill_host = "api.binance.com";
        }
    }

    if (!o.thread_preset_str.empty())
        prov_cfg.threading = string_to_preset(o.thread_preset_str);
    else
        prov_cfg.threading = select_preset(detect_physical_cores());

    if (!o.spin_policy_str.empty())
        prov_cfg.worker_spin_policy = string_to_spin_policy(o.spin_policy_str);

    if (o.fee_model == "fixed")
        prov_cfg.fee_model = std::make_shared<FixedFeeModel>(o.fee_value);
    else if (o.fee_model == "tiered")
        prov_cfg.fee_model = std::make_shared<TieredFeeModel>(o.maker_rate, o.taker_rate);
    else if (o.provider_name == "binance")
        prov_cfg.fee_model = std::make_shared<TieredFeeModel>(0.001, 0.001);

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
            if (sandbox)
                std::cerr << "    Sandbox still uses the live order code path; "
                             "build and run engine_live with --testnet"
                             << ((is_bitget || is_bybit) ? "/--demo" : "")
                             << " to trade on the venue sandbox.\n";
            return 1;
        }
        if (!o.live)
        {
            std::cerr << "  ! Live mode requires --live flag.\n";
            return 1;
        }
        if (resolved_key.empty() || resolved_secret.empty()
            || (is_bitget && resolved_passphrase.empty()))
        {
            if (is_bitget)
            {
                std::cerr << "  ! Live mode requires credentials via env vars "
                             "TRUETEST_BITGET_API_KEY / TRUETEST_BITGET_API_SECRET / "
                             "TRUETEST_BITGET_API_PASSPHRASE "
                             "(or as a fallback, --api-key / --api-secret / "
                             "--api-passphrase).\n";
            }
            else if (is_bybit)
            {
                std::cerr << "  ! Live mode requires credentials via env vars "
                             "TRUETEST_BYBIT_API_KEY / TRUETEST_BYBIT_API_SECRET "
                             "(or as a fallback, --api-key / --api-secret).\n";
            }
            else
            {
                std::cerr << "  ! Live mode requires credentials via env vars "
                             "TRUETEST_BINANCE_API_KEY / TRUETEST_BINANCE_API_SECRET "
                             "(or as a fallback, --api-key / --api-secret).\n";
            }
            return 1;
        }
        if (sandbox)
        {
            if (is_bitget)
                std::cout << "  [DEMO] Live mode will submit orders to Bitget "
                             "demo/paptrading.\n";
            else if (is_bybit && o.demo)
                std::cout << "  [DEMO] Live mode will submit orders to Bybit "
                             "Demo Trading (api-demo.bybit.com).\n";
            else if (is_bybit)
                std::cout << "  [TESTNET] Live mode will submit orders to Bybit "
                             "testnet (api-testnet.bybit.com).\n";
            else
                std::cout << "  [TESTNET] Live mode will submit orders to the "
                             "Binance spot testnet.\n";
        }
        else
        {
            if (!confirm_real_money_math())
                return 0;
        }
    }
    else if (sandbox)
    {
        if (is_bitget)
            std::cout << "  [DEMO] Routing Bitget provider to demo/paptrading.\n";
        else
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

    if (provider->has_data_feed() && !provider->open())
    {
        std::cerr << "  ! Provider failed to open.\n";
        return 1;
    }
    provider_close_guard opened_provider(provider);

    auto transport = provider->get_transport();
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

#if defined(HAS_BINANCE) || defined(HAS_BITGET)
    if (o.provider_name == "binance"
        || provider_is_bitget(o.provider_name)
        || is_streaming)
    {
        if (o.stream.empty() || o.stream == "trade")
            is_tick = true;
        else if (o.stream.find("kline") != std::string::npos
                 || o.stream.find("candle") != std::string::npos)
            is_tick = false;
    }
#endif

    std::cout << "\n";
    std::cout << "  ============================================\n";
    std::cout << "    Provider:  " << o.provider_name << "\n";
    std::cout << "    Format:    " << (is_tick ? "tick" : "bar") << "\n";
    std::cout << "    Strategy:  " << (o.strategy.empty() ? "mean-reversion" : o.strategy) << "\n";
    std::cout << "    SMA:       " << o.sma_period << "\n";
    std::cout << "    Balance:   $" << o.balance << "\n";
    std::cout << "    Risk:      " << (o.risk_fraction * 100) << "% per trade\n";
    std::cout << "    SL/TP:     " << (o.sl_pct * 100) << "% / " << (o.tp_pct * 100) << "%\n";
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
        const std::string& sf = o.no_tui ? std::string("plain") : o.status_format;
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
        engine eng(dh, nullptr, prov_strategy, std::move(prov_cfg));
        eng.set_primary_strategy_name(resolved);
        for (auto& es : extra_strategies)
            eng.add_strategy(es.second, es.first);

        install_shutdown_handler();

#ifdef HAS_WEB
        // Streams live snapshots over /stream for the duration of the run;
        // stopped when web_srv leaves scope at the end of this branch.
        auto web_srv = start_web(eng);
#endif

#ifdef HAS_RICH_TUI
        // Rich (ncurses) tabbed dashboard for shadow/live binaries when the
        // resolved status mode is TUI. ncurses takes over the terminal, so
        // we skip render_banner() (it would be cleared on initscr) and skip
        // the inner ConsoleDashboard render loop - the rich TUI reads from
        // the same ConsoleDashboard atomics + recent-event ring, and pulls
        // structured tables (positions, lots, ...) via engine::snapshot_dashboard.
        std::unique_ptr<truetest::ui::TabbedDashboard> rich_tui;
        if (dashboard && truetest::target_uses_rich_tui()
            && dashboard->is_tui())
        {
            auto snap_fn = [&eng](truetest::ui::dashboard_snapshot& out) {
                bool ok = eng.snapshot_dashboard(out);
                if (ok) out.debug.target = truetest::target_name();
                return ok;
            };
            rich_tui = std::make_unique<truetest::ui::TabbedDashboard>(
                dashboard, snap_fn);

            // Wire operator hotkey actions. Pause/flatten always available;
            // kill switch comes from the provider when one supplies it
            // (Binance does - backtest does not).
            truetest::ui::operator_actions ops;
            ops.pause_toggle = [&eng]() {
                eng.set_pause_all(!eng.is_pause_all());
            };
            ops.pause_state  = [&eng]() { return eng.is_pause_all(); };
            ops.flatten      = [&eng]() { eng.request_flatten(); };
            if (provider) {
                if (auto ks = provider->get_kill_switch()) {
                    ops.kill = [ks](std::chrono::milliseconds dl) {
                        return ks->cancel_all_and_flatten(dl);
                    };
                }
            }
            rich_tui->set_actions(std::move(ops));
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
                dashboard->render_banner();
                dashboard->push_event(
                    truetest::ui::event_severity::notice,
                    "engine starting");
                dashboard->start();
            }
        }

        // Unified event-stream path - provider-agnostic. When the provider
        // advertises it (currently: Binance with --depth-stream set), build
        // a single DataBridge<provider::event>; L2 frames populate the
        // engine's orderbook_registry_ directly so LocalBookAdapter can
        // match against real exchange levels. Otherwise fall back to the
        // existing specialized bar/tick bridges.
        if (provider && provider->supports_event_stream())
        {
            auto parser = provider->get_event_parser();
            auto bridge = std::make_shared<DataBridge<provider::event>>(
                transport, parser, provider::event_sink);
            eng.run_streaming(bridge);
        }
        else if (is_tick)
        {
            std::shared_ptr<IDataParser<tick_record>> parser;
#ifdef HAS_BINANCE
            if (o.provider_name == "binance")
                parser = std::make_shared<BinanceTradeParser>();
            else
#endif
#ifdef HAS_BITGET
            if (provider_is_bitget(o.provider_name))
                parser = std::make_shared<BitgetTradeParser>();
            else
#endif
                parser = std::make_shared<CsvTickParser>();

            auto bridge = std::make_shared<DataBridge<tick_record>>(
                transport, parser, tick_record_sink);
            g_tick_bridge.store(bridge.get(), std::memory_order_release);
            eng.run_streaming(bridge);
            g_tick_bridge.store(nullptr, std::memory_order_release);
        }
        else
        {
            std::shared_ptr<IDataParser<bar_record>> parser;
#ifdef HAS_BINANCE
            if (o.provider_name == "binance")
                parser = std::make_shared<BinanceKlineParser>();
            else
#endif
#ifdef HAS_BITGET
            if (provider_is_bitget(o.provider_name))
                parser = std::make_shared<BitgetKlineParser>();
            else
#endif
                parser = std::make_shared<CsvBarParser>();

            auto bridge = std::make_shared<DataBridge<bar_record>>(
                transport, parser, bar_record_sink);
            g_bar_bridge.store(bridge.get(), std::memory_order_release);
            eng.run_streaming(bridge);
            g_bar_bridge.store(nullptr, std::memory_order_release);
        }

        if (dashboard)
        {
#ifdef HAS_RICH_TUI
            if (rich_tui) rich_tui->stop();
            else
#endif
                dashboard->stop();
        }

        // Skip the multi-line analytics report when the TUI owns stdout -
        // the box on screen is already the report surface, and dumping
        // another fixed-width report below it was the "final analysis
        // windows" complaint. Non-TUI runs still print it. File export
        // via --output is unaffected in either case.
        if (!dashboard || !dashboard->is_tui())
            eng.print_summary();
        export_results(eng.get_analytics(), o.output, o.output_format);
        opened_provider.release();
    }
    else
    {
        if (is_tick)
        {
            std::shared_ptr<IDataParser<tick_record>> parser;
#ifdef HAS_BINANCE
            if (o.provider_name == "binance")
                parser = std::make_shared<BinanceTradeParser>();
            else
#endif
#ifdef HAS_BITGET
            if (provider_is_bitget(o.provider_name))
                parser = std::make_shared<BitgetTradeParser>();
            else
#endif
                parser = std::make_shared<CsvTickParser>();

            auto bridge = std::make_shared<DataBridge<tick_record>>(
                transport, parser, tick_record_sink);

            if (!bridge->load_data(dh))
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
#ifdef HAS_BITGET
            if (provider_is_bitget(o.provider_name))
                parser = std::make_shared<BitgetKlineParser>();
            else
#endif
                parser = std::make_shared<CsvBarParser>();

            auto paths = split_csv(o.provider_path);
            if (paths.size() > 1 && o.provider_name == "local")
            {
                for (const auto& p : paths)
                {
                    auto file_transport = std::make_shared<FileTransport>(p);
                    auto sub_bridge = std::make_shared<DataBridge<bar_record>>(
                        file_transport, parser, bar_record_sink);
                    if (!sub_bridge->load_data(dh))
                    {
                        std::cerr << "  ! Failed to load bar data from " << p << "\n";
                        return 1;
                    }
                }
                dh->sort_by_date();
                std::cout << "    Multi-symbol: loaded " << paths.size()
                          << " files, " << dh->db_data_symbol.size()
                          << " total bars\n";
            }
            else
            {
                auto bridge = std::make_shared<DataBridge<bar_record>>(
                    transport, parser, bar_record_sink);

                if (!bridge->load_data(dh))
                {
                    std::cerr << "  ! Failed to load bar data via provider bridge.\n";
                    return 1;
                }
            }
        }

        engine eng(dh, nullptr, prov_strategy, std::move(prov_cfg));
        eng.set_primary_strategy_name(resolved);
        for (auto& es : extra_strategies)
            eng.add_strategy(es.second, es.first);

        if (is_tick)
            eng.run_tick_data();
        else
            eng.run();

        eng.print_summary();
        export_results(eng.get_analytics(), o.output, o.output_format);
        opened_provider.release();

#ifdef HAS_WEB
        // Backtest is blocking and done; keep the server alive so the operator
        // can review the final report in the browser until they Ctrl-C.
        if (o.web)
        {
            auto web_srv = start_web(eng);
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
    return 0;
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
    if (o.wire_latency_us > 0 && o.mode != "live" && !o.live) {
        config.wire_latency_model = std::make_shared<FixedLatencyModel>(
            latency_duration(o.wire_latency_us));
    }
    if (o.order_latency_us > 0 && o.mode != "live" && !o.live) {
        if (o.order_latency_stddev_us > 0) {
            config.latency_model = std::make_shared<StochasticLatencyModel>(
                static_cast<double>(o.order_latency_us),
                static_cast<double>(o.order_latency_stddev_us),
                o.fill_rng_seed);
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
            if (o.depth_stream.empty()) {
                std::cerr << "[error] --maker-queue-model " << o.maker_queue_model
                          << " requires --depth-stream\n";
                return 1;
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
    // General validation for maker queue model (works for local + binance providers)
    if (o.maker_queue_model != "none" && o.depth_stream.empty()) {
        std::cerr << "[error] --maker-queue-model " << o.maker_queue_model
                  << " requires --depth-stream\n";
        return 1;
    }

    config.instrument_overrides = parse_instrument_specs(o.instrument_specs);
    config.reconcile_tolerance_bps = o.reconcile_tolerance_bps;
    config.kill_switch_deadline = std::chrono::milliseconds(o.kill_switch_deadline_ms);

    config.risk.max_position_value = o.risk_max_position_value;
    config.risk.max_drawdown = o.risk_max_drawdown;
    config.risk.max_loss_per_trade = o.risk_max_loss_per_trade;
    config.risk.max_open_orders = o.risk_max_open_orders;
    config.risk.max_portfolio_exposure = o.risk_max_portfolio_exposure;
    config.risk.max_daily_loss = o.risk_max_daily_loss;
    config.risk.daily_reset_hour = o.risk_daily_reset_hour;
    config.risk.max_trades_per_hour = o.risk_max_trades_per_hour;
    config.risk.max_orders_per_minute = o.risk_max_orders_per_minute;
    config.risk_unwind = o.risk_unwind;

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
    std::cout << "  |  [2]  Shadow  (live feed, paper fills)    |\n";
    std::cout << "  |  [3]  Live    (live feed, real fills)     |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int mode_choice;
    std::cin >> mode_choice;

    if (mode_choice == 2)
        config.mode = engine_mode::shadow;
    else if (mode_choice == 3)
    {
        if (!truetest::target_allows_live_orders())
        {
            std::cerr << "  ! Live mode is only permitted on the engine_live binary "
                         "(this is " << truetest::target_name() << ").\n";
            return 1;
        }
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (!confirm_real_money_math())
            return 0;
        config.mode = engine_mode::live;
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

    engine eng(dh, nullptr, strategy, std::move(config));
    eng.run();
    eng.print_summary();
    export_results(eng.get_analytics(), o.output, o.output_format);

    return 0;
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

    if (opts.dump_config_flag) { dump_config(opts); return 0; }
    if (opts.dry_run) return run_dry_run(opts);
    if (!opts.replay_path.empty()) return run_replay_mode(opts);
    if (opts.monte_carlo || opts.mc_trials > 0) return run_monte_carlo_mode(opts);
    if (!opts.provider_name.empty()) return run_provider_mode(opts);
    return run_tui_mode(opts);
}
