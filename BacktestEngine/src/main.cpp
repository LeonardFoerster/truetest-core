#include <atomic>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "core/engine.h"
#include "core/engine_config.h"
#include "core/event_log.h"
#include "data/data_source.h"
#include "data/csv_data_source.h"
#include "data/binary_cache_source.h"
#include "data/tick_csv_data_source.h"
#include "execution/fee_model.h"
#include "orderbook/orderbook.h"
#include "strategy/strategy_registry.h"
#include "strategy/mean_reversion_strategy.h"
#include "strategy/sma_strategy.h"
#include "strategy/ma_crossover_strategy.h"
#include "market_maker/market_maker.h"
#include "threading/thread_config.h"
#include "utils/log/logger.h"

#ifdef HAS_POSTGRESQL
#include "data/pg_data_source.h"
#endif

#include "providers/provider_registry.h"
#include "providers/data_bridge.h"
#include "providers/local/csv_parser.h"
#include "providers/local/file_transport.h"

#ifdef HAS_BINANCE
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_recorder.h"
#include "providers/binance/binance_replay_transport.h"
#endif

#ifdef HAS_DEBUG
#include "debug/debug_log.h"
#endif

// SIGINT-driven graceful shutdown for streaming mode. The signal handler
// flips an atomic flag on the transport, which unblocks the read loop and
// lets run_streaming() return normally so print_summary() can run.
static std::atomic<DataBridge<tick_record>*> g_tick_bridge{nullptr};
static std::atomic<DataBridge<bar_record>*> g_bar_bridge{nullptr};
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

// ---------------------------------------------------------------------------
// cli_options: every knob main() consumes lives here. Defaults mirror what
// the old argc/argv-era code initialised inline in main().
// ---------------------------------------------------------------------------
struct cli_options
{
    // Core
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

    // Provider / strategy
    std::string provider_name;
    std::string provider_path;
    std::string strategy;
    std::string format;
    std::size_t sma_period = 20;
    std::vector<std::string> params;
    std::string mode;

    // Fee
    std::string fee_model;
    double fee_value = 0.0;
    double maker_rate = 0.0;
    double taker_rate = 0.0;

    // Web UI
    bool enable_web_ui = false;
    uint16_t ws_port = 8765;
    bool ws_compress = true;

    // Streaming / connection
    std::string symbol;
    std::string stream;
    std::string api_key;
    std::string api_secret;
    std::string host;
    std::string port;
    std::string record_path;
    std::string replay_data_path;
    bool live = false;

    // Portfolio
    double balance = 10000.0;
    double risk_fraction = 0.02;
    double sl_pct = 0.005;
    double tp_pct = 0.01;

    // Persistence
    std::string db_path = "truetest.db";
    bool no_db = false;

    // Checkpoints
    std::string checkpoint_path;
    std::string resume_path;
    std::size_t checkpoint_interval = 10000;

    // Backfill
    int backfill = 500;
    std::string backfill_interval;

    // Execution constants
    double market_aggression = 1.1;
    double qty_scale = 1e8;
    unsigned fill_rng_seed = 42;
    double spread_step = 0.0001;

    // Config file handling
    std::string config_file;
    bool dump_config_flag = false;
    bool dry_run = false;

    // Analytics & reporting
    std::size_t rolling_window = 252;
    double risk_free_rate = 0.0;
    std::string output;
    std::string output_format = "json";

    // Risk limits
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
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Config file: load JSON into a cli_options struct. Only fields present in
// the JSON are overwritten; missing fields retain whatever they already had
// (usually the struct defaults).
// ---------------------------------------------------------------------------
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
    auto get_u16 = [&](const char* k, uint16_t& out) {
        if (j.contains(k) && j[k].is_number_unsigned()) out = j[k].get<uint16_t>();
    };

    get_str("replay", o.replay_path);
    get_str("log_events", o.event_log_path);
    get_uint64("seed", o.seed);
    get_str("thread_preset", o.thread_preset_str);
    get_bool("no_pin", o.no_pin);
    get_str("provider", o.provider_name);
    get_str("path", o.provider_path);
    get_str("strategy", o.strategy);
    get_str("format", o.format);
    get_size("sma_period", o.sma_period);
    get_str("fee", o.fee_model);
    get_double("fee_value", o.fee_value);
    get_double("maker_rate", o.maker_rate);
    get_double("taker_rate", o.taker_rate);
    get_bool("web_ui", o.enable_web_ui);
    get_u16("ws_port", o.ws_port);
    get_bool("ws_compress", o.ws_compress);
    get_str("symbol", o.symbol);
    get_str("stream", o.stream);
    get_str("api_key", o.api_key);
    get_str("api_secret", o.api_secret);
    get_str("host", o.host);
    get_str("port", o.port);
    get_str("record", o.record_path);
    get_str("replay_data", o.replay_data_path);
    get_bool("live", o.live);
    get_str("mode", o.mode);
    get_str("db", o.db_path);
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
    get_str("output", o.output);
    get_str("output_format", o.output_format);
}

static void dump_config(const cli_options& o)
{
    nlohmann::json j;
    j["replay"] = o.replay_path;
    j["log_events"] = o.event_log_path;
    j["seed"] = o.seed;
    j["thread_preset"] = o.thread_preset_str;
    j["no_pin"] = o.no_pin;
    j["provider"] = o.provider_name;
    j["path"] = o.provider_path;
    j["strategy"] = o.strategy.empty() ? "mean-reversion" : o.strategy;
    j["format"] = o.format;
    j["sma_period"] = o.sma_period;
    j["fee"] = o.fee_model;
    j["fee_value"] = o.fee_value;
    j["maker_rate"] = o.maker_rate;
    j["taker_rate"] = o.taker_rate;
    j["web_ui"] = o.enable_web_ui;
    j["ws_port"] = o.ws_port;
    j["ws_compress"] = o.ws_compress;
    j["symbol"] = o.symbol;
    j["stream"] = o.stream;
    j["host"] = o.host;
    j["port"] = o.port;
    j["record"] = o.record_path;
    j["replay_data"] = o.replay_data_path;
    j["live"] = o.live;
    j["mode"] = o.mode.empty() ? "backtest" : o.mode;
    j["db"] = o.db_path;
    j["balance"] = o.balance;
    j["risk_fraction"] = o.risk_fraction;
    j["sl"] = o.sl_pct;
    j["tp"] = o.tp_pct;
    j["backfill"] = o.backfill;
    j["backfill_interval"] = o.backfill_interval;
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
    std::cout << j.dump(2) << "\n";
}

// ---------------------------------------------------------------------------
// Register every CLI11 option against its cli_options field.
// ---------------------------------------------------------------------------
static void register_cli_options(CLI::App& app, cli_options& o)
{
    // Core
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
    app.add_flag("--no-pin", o.no_pin, "Disable CPU affinity pinning");
    app.add_option("--spin-policy", o.spin_policy_str, "Worker spin policy: spin, yield, adaptive (default: adaptive)");
    app.add_option("--provider", o.provider_name, "Provider name: local, binance");
    app.add_option("--path", o.provider_path, "Data file path (for local provider)");
    app.add_option("--strategy", o.strategy, "Strategy: mean-reversion, sma, ma-crossover");
    app.add_option("--format", o.format, "Data format: tick, bar");
    app.add_option("--sma-period", o.sma_period, "SMA indicator period")->default_val(20);
    app.add_option("--param", o.params, "Strategy parameter: key=value (repeatable)");
    app.add_option("--mode", o.mode, "Engine mode: backtest, shadow, live");

    // Fee model
    app.add_option("--fee", o.fee_model, "Fee model: fixed, tiered");
    app.add_option("--fee-value", o.fee_value, "Fixed fee per trade");
    app.add_option("--maker-rate", o.maker_rate, "Maker fee rate (tiered)");
    app.add_option("--taker-rate", o.taker_rate, "Taker fee rate (tiered)");

    // WebSocket UI
    app.add_flag("--web-ui", o.enable_web_ui, "Enable WebSocket UI server");
    app.add_option("--ws-port", o.ws_port, "WebSocket server port")->default_val(8765);
    app.add_flag("--ws-compress,!--no-ws-compress", o.ws_compress,
                 "Enable per-message deflate compression (default: on)");

    // Provider/streaming
    app.add_option("--symbol", o.symbol, "Trading symbol (e.g., BTCUSDT)");
    app.add_option("--stream", o.stream, "Stream type: trade, kline, or interval");
    app.add_option("--api-key", o.api_key, "API key for exchange");
    app.add_option("--api-secret", o.api_secret, "API secret for exchange");
    app.add_option("--host", o.host, "WebSocket host");
    app.add_option("--port", o.port, "Port number");
    app.add_option("--record", o.record_path, "Record market data to file");
    app.add_option("--replay-data", o.replay_data_path, "Replay recorded market data");
    app.add_flag("--live", o.live, "Safety flag for live (real money) mode");

    // Portfolio
    app.add_option("--balance", o.balance, "Initial account balance")->default_val(10000.0);
    app.add_option("--risk-fraction", o.risk_fraction, "Position size as equity fraction")->default_val(0.02);
    app.add_option("--sl", o.sl_pct, "Stop loss fraction of entry price")->default_val(0.005);
    app.add_option("--tp", o.tp_pct, "Take profit fraction of entry price")->default_val(0.01);

    // Data persistence
    app.add_option("--db", o.db_path, "SQLite database path")->default_val("truetest.db");
    app.add_flag("--no-db", o.no_db, "Disable SQLite persistence");

    // Checkpointing
    app.add_option("--checkpoint", o.checkpoint_path,
                   "Write periodic portfolio checkpoint to this binary file");
    app.add_option("--checkpoint-interval", o.checkpoint_interval,
                   "Write a checkpoint every N events (default: 10000)")
       ->default_val(10000);
    app.add_option("--resume", o.resume_path,
                   "Restore portfolio state from a checkpoint before running");

    // Backfill
    app.add_option("--backfill", o.backfill, "Historical bars to fetch before streaming")->default_val(500);
    app.add_option("--backfill-interval", o.backfill_interval, "Kline interval for backfill");

    // Execution constants
    app.add_option("--aggression", o.market_aggression, "Market order aggression factor")->default_val(1.1);
    app.add_option("--qty-scale", o.qty_scale, "Quantity scale (fractional → integer)")->default_val(1e8);
    app.add_option("--fill-rng-seed", o.fill_rng_seed, "RNG seed for fill model")->default_val(42);
    app.add_option("--spread-step", o.spread_step, "Spread step factor (mid * factor)")->default_val(0.0001);

    // Config file
    app.add_option("--config", o.config_file, "Load configuration from JSON file");
    app.add_flag("--dump-config", o.dump_config_flag, "Print resolved config as JSON and exit");

    // Dry run
    app.add_flag("--dry-run", o.dry_run, "Validate config, print summary, and exit");

    // Analytics & reporting
    app.add_option("--rolling-window", o.rolling_window, "Rolling metrics window size (bars)")->default_val(252);
    app.add_option("--risk-free-rate", o.risk_free_rate, "Annual risk-free rate for Sharpe/Sortino")->default_val(0.0);

    // Time-based risk limits
    app.add_option("--max-daily-loss", o.risk_max_daily_loss, "Maximum daily loss before halt (0 = no limit)")->default_val(0.0);
    app.add_option("--daily-reset-hour", o.risk_daily_reset_hour, "UTC hour (0-23) to reset daily loss counter")->default_val(0);
    app.add_option("--max-trades-per-hour", o.risk_max_trades_per_hour, "Maximum fills per rolling hour (0 = no limit)")->default_val(0);
    app.add_option("--max-orders-per-minute", o.risk_max_orders_per_minute, "Maximum orders per rolling minute (0 = no limit)")->default_val(0);
    app.add_flag("--risk-unwind", o.risk_unwind, "On risk halt, unwind all positions before stopping");
    app.add_option("--output", o.output, "Write results to file (default: stdout)");
    app.add_option("--output-format", o.output_format, "Output format: json or csv")->default_val("json");
}

// ---------------------------------------------------------------------------
// If --config was passed, load the file into a fresh cli_options and copy
// each field back into the primary opts only when the user did NOT set it
// explicitly on the command line. CLI wins over file.
// ---------------------------------------------------------------------------
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
    if (!was_set("--web-ui")) opts.enable_web_ui = file_opts.enable_web_ui;
    if (!was_set("--ws-port")) opts.ws_port = file_opts.ws_port;
    if (!was_set("--ws-compress")) opts.ws_compress = file_opts.ws_compress;
    if (!was_set("--symbol")) opts.symbol = file_opts.symbol;
    if (!was_set("--stream")) opts.stream = file_opts.stream;
    if (!was_set("--api-key")) opts.api_key = file_opts.api_key;
    if (!was_set("--api-secret")) opts.api_secret = file_opts.api_secret;
    if (!was_set("--host")) opts.host = file_opts.host;
    if (!was_set("--port")) opts.port = file_opts.port;
    if (!was_set("--record")) opts.record_path = file_opts.record_path;
    if (!was_set("--replay-data")) opts.replay_data_path = file_opts.replay_data_path;
    if (!was_set("--live")) opts.live = file_opts.live;
    if (!was_set("--mode")) opts.mode = file_opts.mode;
    if (!was_set("--db")) opts.db_path = file_opts.db_path;
    if (!was_set("--balance")) opts.balance = file_opts.balance;
    if (!was_set("--risk-fraction")) opts.risk_fraction = file_opts.risk_fraction;
    if (!was_set("--sl")) opts.sl_pct = file_opts.sl_pct;
    if (!was_set("--tp")) opts.tp_pct = file_opts.tp_pct;
    if (!was_set("--backfill")) opts.backfill = file_opts.backfill;
    if (!was_set("--backfill-interval")) opts.backfill_interval = file_opts.backfill_interval;
    if (!was_set("--rolling-window")) opts.rolling_window = file_opts.rolling_window;
    if (!was_set("--risk-free-rate")) opts.risk_free_rate = file_opts.risk_free_rate;
    if (!was_set("--output")) opts.output = file_opts.output;
    if (!was_set("--output-format")) opts.output_format = file_opts.output_format;

    // Risk block: these live only in the config file (no CLI counterparts)
    // for the value-style limits; time-based limits do have CLI flags.
    opts.risk_max_position_value = file_opts.risk_max_position_value;
    opts.risk_max_drawdown = file_opts.risk_max_drawdown;
    opts.risk_max_loss_per_trade = file_opts.risk_max_loss_per_trade;
    opts.risk_max_open_orders = file_opts.risk_max_open_orders;
    opts.risk_max_portfolio_exposure = file_opts.risk_max_portfolio_exposure;
    if (!was_set("--max-daily-loss")) opts.risk_max_daily_loss = file_opts.risk_max_daily_loss;
    if (!was_set("--daily-reset-hour")) opts.risk_daily_reset_hour = file_opts.risk_daily_reset_hour;
    if (!was_set("--max-trades-per-hour")) opts.risk_max_trades_per_hour = file_opts.risk_max_trades_per_hour;
    if (!was_set("--max-orders-per-minute")) opts.risk_max_orders_per_minute = file_opts.risk_max_orders_per_minute;
}

// ---------------------------------------------------------------------------
// --dry-run: print the resolved config summary and validate known fields.
// ---------------------------------------------------------------------------
static int run_dry_run(const cli_options& o)
{
    std::string resolved_strategy = o.strategy.empty() ? "mean-reversion" : o.strategy;
    std::string resolved_mode = o.mode.empty() ? "backtest" : o.mode;
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
    std::cout << "    Threading:  " << resolved_preset << " (" << resolved_spin << " spin)\n";
    std::cout << "    Provider:   " << (o.provider_name.empty() ? "(TUI)" : o.provider_name) << "\n";
    std::cout << "    Data path:  " << (o.provider_path.empty() ? "(none)" : o.provider_path) << "\n";
    std::cout << "    DB:         " << (o.db_path.empty() ? "(disabled)" : o.db_path) << "\n";
    std::cout << "    Fee model:  " << (o.fee_model.empty() ? "zero" : o.fee_model) << "\n";
    std::cout << "    Web UI:     " << (o.enable_web_ui ? "yes" : "no") << "\n";
    if (o.enable_web_ui)
        std::cout << "    WS port:    " << o.ws_port << "\n";
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
    std::cout << "  ============================================\n";

    bool valid = true;
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

// ---------------------------------------------------------------------------
// Replay mode: event log → engine, skip data source.
// ---------------------------------------------------------------------------
static int run_replay_mode(const cli_options& o)
{
    std::cout << "\n  Replaying events from: " << o.replay_path << "\n";
    std::cout << "  Using Mean Reversion strategy (default) for replay.\n";

    auto strategy = std::make_shared<mean_reversion_strategy>(20);
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

// ---------------------------------------------------------------------------
// Provider mode: registry-based data feed → engine (batch or streaming).
// Owns the SIGINT bridge wiring.
// ---------------------------------------------------------------------------
static int run_provider_mode(const cli_options& o)
{
    // M2: for multi-file --path, pass only the first path to the provider
    // (each additional file is loaded separately via FileTransport below).
    std::string primary_path = o.provider_path;
    {
        auto comma = primary_path.find(',');
        if (comma != std::string::npos)
            primary_path = primary_path.substr(0, comma);
    }

    provider_config pcfg;
    if (!primary_path.empty())  pcfg["path"] = primary_path;
    if (!o.symbol.empty())      pcfg["symbol"] = o.symbol;
    if (!o.stream.empty())      pcfg["stream"] = o.stream;
    if (!o.api_key.empty())     pcfg["api_key"] = o.api_key;
    if (!o.api_secret.empty())  pcfg["api_secret"] = o.api_secret;
    if (!o.host.empty())        pcfg["host"] = o.host;
    if (!o.port.empty())        pcfg["port"] = o.port;

    std::shared_ptr<IProvider> provider;
    try {
        provider = ProviderRegistry::instance().create(o.provider_name, pcfg);
    } catch (const std::exception& e) {
        std::cerr << "  ! " << e.what() << "\n";
        if (o.provider_name == "binance")
            std::cerr << "    Example: --provider binance --symbol btcusdt --stream trade\n";
        else if (o.provider_name == "local")
            std::cerr << "    Example: --provider local --path market_data.csv\n";
        else
        {
            std::cerr << "    Available providers:";
            for (const auto& n : ProviderRegistry::instance().available())
                std::cerr << " " << n;
            std::cerr << "\n";
        }
        return 1;
    }

    // Select strategy via registry (default: mean-reversion).
    // M1: comma-separated names create multiple strategies; the first is
    // the primary, the rest run alongside it via engine::add_strategy().
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
    apply_strategy_params(*prov_strategy, o.params);

    std::vector<std::pair<std::string, std::shared_ptr<IStrategy>>> extra_strategies;
    for (std::size_t si = 1; si < strategy_names.size(); ++si) {
        const auto& nm = strategy_names[si];
        if (!StrategyRegistry::instance().has(nm)) {
            std::cerr << "  ! Unknown additional strategy: " << nm << "\n";
            return 1;
        }
        auto extra = StrategyRegistry::instance().create(nm);
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
    prov_cfg.enable_web_ui = o.enable_web_ui;
    prov_cfg.ws_port = o.ws_port;
    prov_cfg.ws_compress = o.ws_compress;
    prov_cfg.initial_balance = o.balance;
    prov_cfg.db_path = o.db_path;
    prov_cfg.checkpoint_path = o.checkpoint_path;
    prov_cfg.resume_checkpoint_path = o.resume_path;
    prov_cfg.checkpoint_interval_events = o.checkpoint_interval;
    prov_cfg.backfill_bars = o.backfill;
    prov_cfg.backfill_interval = o.backfill_interval;
    prov_cfg.market_aggression = o.market_aggression;
    prov_cfg.qty_scale = o.qty_scale;
    prov_cfg.fill_rng_seed = o.fill_rng_seed;
    prov_cfg.spread_step_factor = o.spread_step;
    prov_cfg.rolling_window = o.rolling_window;
    prov_cfg.risk_free_rate = o.risk_free_rate;

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

    // Map WS host to REST host for backfill
    if (!o.host.empty()) {
        if (o.host.find("testnet") != std::string::npos)
            prov_cfg.backfill_host = "testnet.binance.vision";
        else
            prov_cfg.backfill_host = "api.binance.com";
    }

    if (!o.thread_preset_str.empty())
        prov_cfg.threading = string_to_preset(o.thread_preset_str);
    else
        prov_cfg.threading = select_preset(detect_physical_cores());

    if (!o.spin_policy_str.empty())
        prov_cfg.worker_spin_policy = string_to_spin_policy(o.spin_policy_str);

    // Fee model
    if (o.fee_model == "fixed")
        prov_cfg.fee_model = std::make_shared<FixedFeeModel>(o.fee_value);
    else if (o.fee_model == "tiered")
        prov_cfg.fee_model = std::make_shared<TieredFeeModel>(o.maker_rate, o.taker_rate);
    else if (o.provider_name == "binance")
        prov_cfg.fee_model = std::make_shared<TieredFeeModel>(0.001, 0.001);

    // Engine mode
    if (o.mode == "shadow")
        prov_cfg.mode = engine_mode::shadow;
    else if (o.mode == "live" || o.live)
        prov_cfg.mode = engine_mode::live;

    // Live-mode safety confirmation
    if (prov_cfg.mode == engine_mode::live)
    {
        if (!o.live)
        {
            std::cerr << "  ! Live mode requires --live flag.\n";
            return 1;
        }
        if (o.api_key.empty() || o.api_secret.empty())
        {
            std::cerr << "  ! Live mode requires --api-key and --api-secret.\n";
            return 1;
        }
        std::cout << "  WARNING: You are about to submit REAL orders. Type YES to continue: ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "YES")
        {
            std::cout << "  Aborted.\n";
            return 0;
        }
    }

    // Hand the engine config to the provider so it can wire fees, fills,
    // backfill, and mode-dependent execution before opening.
    provider->configure(prov_cfg);

    if (provider->has_data_feed() && !provider->open())
    {
        std::cerr << "  ! Provider failed to open.\n";
        return 1;
    }

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

    bool is_tick = (o.format == "tick");
    bool is_streaming = transport->is_streaming();

#ifdef HAS_BINANCE
    if (o.provider_name == "binance" || is_streaming)
    {
        if (o.stream.empty() || o.stream == "trade")
            is_tick = true;
        else if (o.stream.find("kline") != std::string::npos)
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
    std::cout << "    Threading: " << preset_to_string(prov_cfg.threading)
              << " (" << preset_worker_count(prov_cfg.threading) << " workers)\n";
    if (!o.record_path.empty())
        std::cout << "    Recording: " << o.record_path << "\n";
    if (!o.replay_data_path.empty())
        std::cout << "    Replay:    " << o.replay_data_path << "\n";
    std::cout << "  ============================================\n\n";

    if (is_streaming)
    {
        engine eng(dh, nullptr, prov_strategy, std::move(prov_cfg));
        eng.set_primary_strategy_name(resolved);
        for (auto& es : extra_strategies)
            eng.add_strategy(es.second, es.first);

        install_shutdown_handler();

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
                parser = std::make_shared<CsvBarParser>();

            auto bridge = std::make_shared<DataBridge<bar_record>>(
                transport, parser, bar_record_sink);
            g_bar_bridge.store(bridge.get(), std::memory_order_release);
            eng.run_streaming(bridge);
            g_bar_bridge.store(nullptr, std::memory_order_release);
        }

        eng.print_summary();
        export_results(eng.get_analytics(), o.output, o.output_format);
    }
    else
    {
        // Batch path: load all data first, then run engine
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
                parser = std::make_shared<CsvBarParser>();

            // M2: comma-separated --path loads multiple CSVs into the same
            // data_handler for multi-symbol backtesting. Only supported for
            // the local provider (each path needs its own transport).
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
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Legacy interactive TUI (no provider/replay supplied → prompt the user).
// ---------------------------------------------------------------------------
static int run_tui_mode(const cli_options& o)
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

    // ── Strategy Selection ───────────────────────────────────────
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

    // ── SMA Period ───────────────────────────────────────────────
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
    apply_strategy_params(*strategy, o.params);

    // ── Data Source Selection ────────────────────────────────────
    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Data Source                               |\n";
    std::cout << "  +-------------------------------------------+\n";
#ifdef HAS_POSTGRESQL
    std::cout << "  |  [1]  PostgreSQL Database                  |\n";
#else
    std::cout << "  |  [1]  PostgreSQL (unavailable)             |\n";
    std::cout << "  |       build with -DENABLE_POSTGRESQL=ON    |\n";
#endif
    std::cout << "  |  [2]  CSV File (OHLCV)                     |\n";
    std::cout << "  |  [3]  Tick CSV File                        |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int data_choice;
    std::cin >> data_choice;

    std::unique_ptr<IDataSource> source;

    if (data_choice == 2)
    {
        std::cout << "\n  CSV path: ";
        std::string csv_path;
        std::cin >> csv_path;
        source = std::make_unique<CsvDataSource>(csv_path);
    }
    else if (data_choice == 3)
    {
        std::cout << "\n  Tick CSV path: ";
        std::string tick_path;
        std::cin >> tick_path;
        source = std::make_unique<TickCsvDataSource>(tick_path);
    }
#ifdef HAS_POSTGRESQL
    else if (data_choice == 1)
    {
        auto pg = std::make_unique<PgDataSource>();
        pg->establish_connection();
        pg->test_connection();
        source = std::make_unique<BinaryCacheSource>(std::move(pg), "data_cache.bin");
    }
#endif
    else
    {
        std::cout << "  ! Invalid choice.\n";
        return 1;
    }

    // ── Fee Model Selection ─────────────────────────────────────
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

    engine_config config;
    config.seed = o.seed;
    config.event_log_path = o.event_log_path;
    config.compress_log = o.compress_log;
    config.log_max_bytes = o.log_max_size_mb * 1024ull * 1024ull;
    config.log_max_files = o.log_keep;
    config.disable_pinning = o.no_pin;
    config.db_path = o.db_path;
    config.checkpoint_path = o.checkpoint_path;
    config.resume_checkpoint_path = o.resume_path;
    config.checkpoint_interval_events = o.checkpoint_interval;
    config.market_aggression = o.market_aggression;
    config.qty_scale = o.qty_scale;
    config.fill_rng_seed = o.fill_rng_seed;
    config.spread_step_factor = o.spread_step;
    config.rolling_window = o.rolling_window;
    config.risk_free_rate = o.risk_free_rate;

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

    std::cout << "  Threading: " << preset_to_string(config.threading)
              << " (" << preset_worker_count(config.threading) << " workers, "
              << spin_policy_to_string(config.worker_spin_policy) << " spin)\n";

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

    // ── Engine Mode Selection ────────────────────────────────────
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
        config.mode = engine_mode::live;

    // ── Run ──────────────────────────────────────────────────────
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

// ---------------------------------------------------------------------------
// Orchestrator: parse → overlay config → dispatch.
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
#ifdef HAS_DEBUG
    debug::init();
    static debug::FileSink file_sink("truetest_debug.log");
    absl::AddLogSink(&file_sink);
    LOG(INFO) << "TrueTest debug instrumentation enabled";
#endif

    cli_options opts;

    CLI::App app{"TrueTest — modular C++17 backtesting engine"};
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

    apply_config_overlay(app, opts);

    if (opts.no_db) opts.db_path.clear();

    if (opts.dump_config_flag) { dump_config(opts); return 0; }
    if (opts.dry_run) return run_dry_run(opts);
    if (!opts.replay_path.empty()) return run_replay_mode(opts);
    if (!opts.provider_name.empty()) return run_provider_mode(opts);
    return run_tui_mode(opts);
}
