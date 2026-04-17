#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
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
#include "absl/flags/parse.h"
#endif

// ---------------------------------------------------------------------------
// Helper: apply --param key=value pairs to a strategy after construction
// ---------------------------------------------------------------------------
// M1: split "sma,mean-reversion" → ["sma","mean-reversion"]
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

// ---------------------------------------------------------------------------
// Helper: export analytics results to file (JSON or CSV)
// ---------------------------------------------------------------------------
static void export_results(const Analytics& analytics,
                           const std::string& output_path,
                           const std::string& format)
{
    if (output_path.empty()) return;

    if (format == "csv")
    {
        // For CSV, derive trade path from output path
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
// Helper: load a JSON config file and apply values to locals (CLI overrides)
// ---------------------------------------------------------------------------
static void load_config_file(const std::string& path,
                             std::string& replay_path,
                             std::string& event_log_path,
                             uint64_t& seed,
                             std::string& thread_preset_str,
                             bool& no_pin,
                             std::string& provider_name,
                             std::string& provider_path,
                             std::string& cli_strategy,
                             std::string& cli_format,
                             std::size_t& cli_sma_period,
                             std::string& cli_fee_model,
                             double& cli_fee_value,
                             double& cli_maker_rate,
                             double& cli_taker_rate,
                             bool& enable_web_ui,
                             uint16_t& ws_port,
                             bool& ws_compress,
                             std::string& cli_symbol,
                             std::string& cli_stream,
                             std::string& cli_api_key,
                             std::string& cli_api_secret,
                             std::string& cli_host,
                             std::string& cli_port,
                             std::string& cli_record_path,
                             std::string& cli_replay_data_path,
                             bool& cli_live,
                             std::string& cli_mode,
                             std::string& cli_db_path,
                             double& cli_balance,
                             double& cli_risk_fraction,
                             double& cli_sl_pct,
                             double& cli_tp_pct,
                             int& cli_backfill,
                             std::string& cli_backfill_interval,
                             double& risk_max_position_value,
                             double& risk_max_drawdown,
                             double& risk_max_loss_per_trade,
                             int& risk_max_open_orders,
                             double& risk_max_portfolio_exposure,
                             double& risk_max_daily_loss,
                             int& risk_daily_reset_hour,
                             int& risk_max_trades_per_hour,
                             int& risk_max_orders_per_minute,
                             std::size_t& cli_rolling_window,
                             double& cli_risk_free_rate,
                             std::string& cli_output,
                             std::string& cli_output_format)
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

    // Only set values that are present in the JSON (CLI overrides later)
    auto get_str = [&](const char* key, std::string& out) {
        if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    auto get_double = [&](const char* key, double& out) {
        if (j.contains(key) && j[key].is_number()) out = j[key].get<double>();
    };
    auto get_uint64 = [&](const char* key, uint64_t& out) {
        if (j.contains(key) && j[key].is_number_unsigned()) out = j[key].get<uint64_t>();
    };
    auto get_size = [&](const char* key, std::size_t& out) {
        if (j.contains(key) && j[key].is_number_unsigned()) out = j[key].get<std::size_t>();
    };
    auto get_int = [&](const char* key, int& out) {
        if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<int>();
    };
    auto get_bool = [&](const char* key, bool& out) {
        if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };
    auto get_u16 = [&](const char* key, uint16_t& out) {
        if (j.contains(key) && j[key].is_number_unsigned()) out = j[key].get<uint16_t>();
    };

    get_str("replay", replay_path);
    get_str("log_events", event_log_path);
    get_uint64("seed", seed);
    get_str("thread_preset", thread_preset_str);
    get_bool("no_pin", no_pin);
    get_str("provider", provider_name);
    get_str("path", provider_path);
    get_str("strategy", cli_strategy);
    get_str("format", cli_format);
    get_size("sma_period", cli_sma_period);
    get_str("fee", cli_fee_model);
    get_double("fee_value", cli_fee_value);
    get_double("maker_rate", cli_maker_rate);
    get_double("taker_rate", cli_taker_rate);
    get_bool("web_ui", enable_web_ui);
    get_u16("ws_port", ws_port);
    get_bool("ws_compress", ws_compress);
    get_str("symbol", cli_symbol);
    get_str("stream", cli_stream);
    get_str("api_key", cli_api_key);
    get_str("api_secret", cli_api_secret);
    get_str("host", cli_host);
    get_str("port", cli_port);
    get_str("record", cli_record_path);
    get_str("replay_data", cli_replay_data_path);
    get_bool("live", cli_live);
    get_str("mode", cli_mode);
    get_str("db", cli_db_path);
    get_double("balance", cli_balance);
    get_double("risk_fraction", cli_risk_fraction);
    get_double("sl", cli_sl_pct);
    get_double("tp", cli_tp_pct);
    get_int("backfill", cli_backfill);
    get_str("backfill_interval", cli_backfill_interval);

    // Risk limits
    if (j.contains("risk") && j["risk"].is_object())
    {
        auto& r = j["risk"];
        auto rd = [&](const char* key, double& out) {
            if (r.contains(key) && r[key].is_number()) out = r[key].get<double>();
        };
        auto ri = [&](const char* key, int& out) {
            if (r.contains(key) && r[key].is_number_integer()) out = r[key].get<int>();
        };
        rd("max_position_value", risk_max_position_value);
        rd("max_drawdown", risk_max_drawdown);
        rd("max_loss_per_trade", risk_max_loss_per_trade);
        ri("max_open_orders", risk_max_open_orders);
        rd("max_portfolio_exposure", risk_max_portfolio_exposure);
        rd("max_daily_loss", risk_max_daily_loss);
        ri("daily_reset_hour", risk_daily_reset_hour);
        ri("max_trades_per_hour", risk_max_trades_per_hour);
        ri("max_orders_per_minute", risk_max_orders_per_minute);
    }

    // Analytics & reporting
    get_size("rolling_window", cli_rolling_window);
    get_double("risk_free_rate", cli_risk_free_rate);
    get_str("output", cli_output);
    get_str("output_format", cli_output_format);
}

// ---------------------------------------------------------------------------
// Helper: dump resolved config as JSON to stdout
// ---------------------------------------------------------------------------
static void dump_config(const std::string& replay_path,
                        const std::string& event_log_path,
                        uint64_t seed,
                        const std::string& thread_preset_str,
                        bool no_pin,
                        const std::string& provider_name,
                        const std::string& provider_path,
                        const std::string& cli_strategy,
                        const std::string& cli_format,
                        std::size_t cli_sma_period,
                        const std::string& cli_fee_model,
                        double cli_fee_value,
                        double cli_maker_rate,
                        double cli_taker_rate,
                        bool enable_web_ui,
                        uint16_t ws_port,
                        bool ws_compress,
                        const std::string& cli_symbol,
                        const std::string& cli_stream,
                        const std::string& cli_host,
                        const std::string& cli_port,
                        const std::string& cli_record_path,
                        const std::string& cli_replay_data_path,
                        bool cli_live,
                        const std::string& cli_mode,
                        const std::string& cli_db_path,
                        double cli_balance,
                        double cli_risk_fraction,
                        double cli_sl_pct,
                        double cli_tp_pct,
                        int cli_backfill,
                        const std::string& cli_backfill_interval,
                        double risk_max_position_value,
                        double risk_max_drawdown,
                        double risk_max_loss_per_trade,
                        int risk_max_open_orders,
                        double risk_max_portfolio_exposure,
                        double risk_max_daily_loss,
                        int risk_daily_reset_hour,
                        int risk_max_trades_per_hour,
                        int risk_max_orders_per_minute)
{
    nlohmann::json j;
    j["replay"] = replay_path;
    j["log_events"] = event_log_path;
    j["seed"] = seed;
    j["thread_preset"] = thread_preset_str;
    j["no_pin"] = no_pin;
    j["provider"] = provider_name;
    j["path"] = provider_path;
    j["strategy"] = cli_strategy.empty() ? "mean-reversion" : cli_strategy;
    j["format"] = cli_format;
    j["sma_period"] = cli_sma_period;
    j["fee"] = cli_fee_model;
    j["fee_value"] = cli_fee_value;
    j["maker_rate"] = cli_maker_rate;
    j["taker_rate"] = cli_taker_rate;
    j["web_ui"] = enable_web_ui;
    j["ws_port"] = ws_port;
    j["ws_compress"] = ws_compress;
    j["symbol"] = cli_symbol;
    j["stream"] = cli_stream;
    j["host"] = cli_host;
    j["port"] = cli_port;
    j["record"] = cli_record_path;
    j["replay_data"] = cli_replay_data_path;
    j["live"] = cli_live;
    j["mode"] = cli_mode.empty() ? "backtest" : cli_mode;
    j["db"] = cli_db_path;
    j["balance"] = cli_balance;
    j["risk_fraction"] = cli_risk_fraction;
    j["sl"] = cli_sl_pct;
    j["tp"] = cli_tp_pct;
    j["backfill"] = cli_backfill;
    j["backfill_interval"] = cli_backfill_interval;
    j["risk"] = {
        {"max_position_value", risk_max_position_value},
        {"max_drawdown", risk_max_drawdown},
        {"max_loss_per_trade", risk_max_loss_per_trade},
        {"max_open_orders", risk_max_open_orders},
        {"max_portfolio_exposure", risk_max_portfolio_exposure},
        {"max_daily_loss", risk_max_daily_loss},
        {"daily_reset_hour", risk_daily_reset_hour},
        {"max_trades_per_hour", risk_max_trades_per_hour},
        {"max_orders_per_minute", risk_max_orders_per_minute}
    };
    std::cout << j.dump(2) << "\n";
}

int main(int argc, char* argv[])
{
#ifdef HAS_DEBUG
    absl::ParseCommandLine(argc, argv);
    debug::init();

    // Optional: log to file as well as stderr
    static debug::FileSink file_sink("truetest_debug.log");
    absl::AddLogSink(&file_sink);

    LOG(INFO) << "TrueTest debug instrumentation enabled";
#endif

    // --- Variable declarations (with defaults) ---
    std::string replay_path;
    int64_t replay_from_us = 0;
    int64_t replay_to_us = INT64_MAX;
    std::string event_log_path;
    std::string log_file_path;               // L1: operational log file
    std::uint64_t log_max_size_mb = 0;       // L3: 0 = no rotation
    int log_keep = 5;                        // L3: rotated files retained
    uint64_t seed = 0;
    std::string thread_preset_str;
    std::string spin_policy_str;
    bool no_pin = false;
    std::string provider_name;
    std::string provider_path;
    std::string cli_strategy;
    std::string cli_format;
    std::size_t cli_sma_period = 20;
    std::string cli_fee_model;
    double cli_fee_value = 0.0;
    double cli_maker_rate = 0.0;
    double cli_taker_rate = 0.0;
    bool enable_web_ui = false;
    uint16_t ws_port = 8765;
    bool ws_compress = true;
    std::string cli_symbol;
    std::string cli_stream;
    std::string cli_api_key;
    std::string cli_api_secret;
    std::string cli_host;
    std::string cli_port;
    std::string cli_record_path;
    std::string cli_replay_data_path;
    bool cli_live = false;
    std::string cli_mode;
    std::string cli_db_path = "truetest.db";
    bool no_db = false;
    // K3: checkpoint CLI state
    std::string cli_checkpoint_path;
    std::string cli_resume_path;
    std::size_t cli_checkpoint_interval = 10000;
    double cli_balance = 10000.0;
    double cli_risk_fraction = 0.02;
    double cli_sl_pct = 0.005;
    double cli_tp_pct = 0.01;
    int cli_backfill = 500;
    std::string cli_backfill_interval;
    bool compress_log = true;
    std::string config_file;
    bool dump_config_flag = false;
    bool dry_run = false;
    std::vector<std::string> cli_params; // --param key=value pairs

    // Analytics & reporting (Step G)
    std::size_t cli_rolling_window = 252;
    double cli_risk_free_rate = 0.0;
    std::string cli_output;
    std::string cli_output_format = "json";

    // Risk limit defaults (match risk_limits struct)
    double risk_max_position_value = 1e9;
    double risk_max_drawdown = 0.30;
    double risk_max_loss_per_trade = 10000.0;
    int risk_max_open_orders = 1000;
    double risk_max_portfolio_exposure = 5e9;

    // Time-based risk limits (H2)
    double risk_max_daily_loss = 0.0;
    int risk_daily_reset_hour = 0;
    int risk_max_trades_per_hour = 0;
    int risk_max_orders_per_minute = 0;

    // Risk unwind (H3)
    bool risk_unwind = false;

    // --- CLI11 setup ---
    CLI::App app{"TrueTest — modular C++17 backtesting engine"};
    app.set_help_all_flag("--help-all", "Show all help");

    // Core
    app.add_option("--replay", replay_path, "Path to event log for replay mode");
    app.add_option("--replay-from", replay_from_us, "Replay from timestamp (microseconds since epoch)");
    app.add_option("--replay-to", replay_to_us, "Replay to timestamp (microseconds since epoch)");
    app.add_option("--log-events", event_log_path, "Path to write binary event log");
    app.add_option("--log-file", log_file_path, "Path to write operational log (L1, default: stderr)");
    app.add_option("--log-max-size", log_max_size_mb, "Max size (MB) per event/text log before rotation (L3, 0 = no rotation)");
    app.add_option("--log-keep", log_keep, "Number of rotated log files to retain (L3, default: 5)");
    app.add_flag("--compress-log,!--no-compress-log", compress_log, "Compress binary event logs with zstd (default: on)");
    app.add_option("--seed", seed, "RNG seed (0 = non-deterministic)");
    app.add_option("--thread-preset", thread_preset_str, "Threading: inline, light, standard, full, extended");
    app.add_flag("--no-pin", no_pin, "Disable CPU affinity pinning");
    app.add_option("--spin-policy", spin_policy_str, "Worker spin policy: spin, yield, adaptive (default: adaptive)");
    app.add_option("--provider", provider_name, "Provider name: local, binance");
    app.add_option("--path", provider_path, "Data file path (for local provider)");
    app.add_option("--strategy", cli_strategy, "Strategy: mean-reversion, sma, ma-crossover");
    app.add_option("--format", cli_format, "Data format: tick, bar");
    app.add_option("--sma-period", cli_sma_period, "SMA indicator period")->default_val(20);
    app.add_option("--param", cli_params, "Strategy parameter: key=value (repeatable)");
    app.add_option("--mode", cli_mode, "Engine mode: backtest, shadow, live");

    // Fee model
    app.add_option("--fee", cli_fee_model, "Fee model: fixed, tiered");
    app.add_option("--fee-value", cli_fee_value, "Fixed fee per trade");
    app.add_option("--maker-rate", cli_maker_rate, "Maker fee rate (tiered)");
    app.add_option("--taker-rate", cli_taker_rate, "Taker fee rate (tiered)");

    // WebSocket UI
    app.add_flag("--web-ui", enable_web_ui, "Enable WebSocket UI server");
    app.add_option("--ws-port", ws_port, "WebSocket server port")->default_val(8765);
    app.add_flag("--ws-compress,!--no-ws-compress", ws_compress,
                 "Enable per-message deflate compression (default: on)");

    // Provider/streaming
    app.add_option("--symbol", cli_symbol, "Trading symbol (e.g., BTCUSDT)");
    app.add_option("--stream", cli_stream, "Stream type: trade, kline, or interval");
    app.add_option("--api-key", cli_api_key, "API key for exchange");
    app.add_option("--api-secret", cli_api_secret, "API secret for exchange");
    app.add_option("--host", cli_host, "WebSocket host");
    app.add_option("--port", cli_port, "Port number");
    app.add_option("--record", cli_record_path, "Record market data to file");
    app.add_option("--replay-data", cli_replay_data_path, "Replay recorded market data");
    app.add_flag("--live", cli_live, "Safety flag for live (real money) mode");

    // Portfolio
    app.add_option("--balance", cli_balance, "Initial account balance")->default_val(10000.0);
    app.add_option("--risk-fraction", cli_risk_fraction, "Position size as equity fraction")->default_val(0.02);
    app.add_option("--sl", cli_sl_pct, "Stop loss fraction of entry price")->default_val(0.005);
    app.add_option("--tp", cli_tp_pct, "Take profit fraction of entry price")->default_val(0.01);

    // Data persistence
    app.add_option("--db", cli_db_path, "SQLite database path")->default_val("truetest.db");
    app.add_flag("--no-db", no_db, "Disable SQLite persistence");

    // K3: checkpointing
    app.add_option("--checkpoint", cli_checkpoint_path,
                   "Write periodic portfolio checkpoint to this binary file");
    app.add_option("--checkpoint-interval", cli_checkpoint_interval,
                   "Write a checkpoint every N events (default: 10000)")
       ->default_val(10000);
    app.add_option("--resume", cli_resume_path,
                   "Restore portfolio state from a checkpoint before running");

    // Backfill
    app.add_option("--backfill", cli_backfill, "Historical bars to fetch before streaming")->default_val(500);
    app.add_option("--backfill-interval", cli_backfill_interval, "Kline interval for backfill");

    // Execution constants
    double cli_market_aggression = 1.1;
    double cli_qty_scale = 1e8;
    unsigned cli_fill_rng_seed = 42;
    double cli_spread_step = 0.0001;
    app.add_option("--aggression", cli_market_aggression, "Market order aggression factor")->default_val(1.1);
    app.add_option("--qty-scale", cli_qty_scale, "Quantity scale (fractional → integer)")->default_val(1e8);
    app.add_option("--fill-rng-seed", cli_fill_rng_seed, "RNG seed for fill model")->default_val(42);
    app.add_option("--spread-step", cli_spread_step, "Spread step factor (mid * factor)")->default_val(0.0001);

    // Config file (B2)
    app.add_option("--config", config_file, "Load configuration from JSON file");
    app.add_flag("--dump-config", dump_config_flag, "Print resolved config as JSON and exit");

    // Dry run (B3)
    app.add_flag("--dry-run", dry_run, "Validate config, print summary, and exit");

    // Analytics & reporting (Step G)
    app.add_option("--rolling-window", cli_rolling_window, "Rolling metrics window size (bars)")->default_val(252);
    app.add_option("--risk-free-rate", cli_risk_free_rate, "Annual risk-free rate for Sharpe/Sortino")->default_val(0.0);

    // Time-based risk limits (H2)
    app.add_option("--max-daily-loss", risk_max_daily_loss, "Maximum daily loss before halt (0 = no limit)")->default_val(0.0);
    app.add_option("--daily-reset-hour", risk_daily_reset_hour, "UTC hour (0-23) to reset daily loss counter")->default_val(0);
    app.add_option("--max-trades-per-hour", risk_max_trades_per_hour, "Maximum fills per rolling hour (0 = no limit)")->default_val(0);
    app.add_option("--max-orders-per-minute", risk_max_orders_per_minute, "Maximum orders per rolling minute (0 = no limit)")->default_val(0);
    app.add_flag("--risk-unwind", risk_unwind, "On risk halt, unwind all positions before stopping");
    app.add_option("--output", cli_output, "Write results to file (default: stdout)");
    app.add_option("--output-format", cli_output_format, "Output format: json or csv")->default_val("json");

    // Parse — reject unknown flags
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // L1: Initialize structured logger sink (stderr default, file if specified).
    if (!log_file_path.empty())
        tt_log::Logger::instance().set_file(log_file_path);
    LOG_INFO("main", "truetest starting (pid=%d)", static_cast<int>(getpid()));

    // --- Load config file first (CLI values override) ---
    // CLI11 has already parsed CLI flags into the variables above.
    // If --config is specified, we load the file into *temporary* copies,
    // then only apply values that were NOT set via CLI.
    if (!config_file.empty())
    {
        // Save CLI-set values, load file defaults, then restore CLI overrides
        // Strategy: load file into the same variables (as defaults), then re-parse CLI.
        // Simpler approach: load file values, then let CLI11 overwrite.
        // Since CLI11 already parsed, we do: save current, load file, restore CLI-set ones.

        // We need to know which options the user explicitly set on CLI.
        // CLI11 tracks this via ->count() > 0.
        auto was_set = [&](const std::string& name) -> bool {
            auto opt = app.get_option_no_throw(name);
            return opt && opt->count() > 0;
        };

        // Save CLI-set values
        auto save_replay = replay_path;
        auto save_event_log = event_log_path;
        auto save_seed = seed;
        auto save_preset = thread_preset_str;
        auto save_no_pin = no_pin;
        auto save_provider = provider_name;
        auto save_path = provider_path;
        auto save_strategy = cli_strategy;
        auto save_format = cli_format;
        auto save_sma = cli_sma_period;
        auto save_fee = cli_fee_model;
        auto save_fee_value = cli_fee_value;
        auto save_maker = cli_maker_rate;
        auto save_taker = cli_taker_rate;
        auto save_webui = enable_web_ui;
        auto save_wsport = ws_port;
        auto save_wscompress = ws_compress;
        auto save_symbol = cli_symbol;
        auto save_stream = cli_stream;
        auto save_apikey = cli_api_key;
        auto save_apisecret = cli_api_secret;
        auto save_host = cli_host;
        auto save_port = cli_port;
        auto save_record = cli_record_path;
        auto save_replay_data = cli_replay_data_path;
        auto save_live = cli_live;
        auto save_mode = cli_mode;
        auto save_db = cli_db_path;
        auto save_balance = cli_balance;
        auto save_risk_fraction = cli_risk_fraction;
        auto save_sl = cli_sl_pct;
        auto save_tp = cli_tp_pct;
        auto save_backfill = cli_backfill;
        auto save_backfill_interval = cli_backfill_interval;
        auto save_rolling_window = cli_rolling_window;
        auto save_risk_free_rate = cli_risk_free_rate;
        auto save_output = cli_output;
        auto save_output_format = cli_output_format;

        // Load file (overwrites all variables)
        load_config_file(config_file,
            replay_path, event_log_path, seed, thread_preset_str, no_pin,
            provider_name, provider_path, cli_strategy, cli_format, cli_sma_period,
            cli_fee_model, cli_fee_value, cli_maker_rate, cli_taker_rate,
            enable_web_ui, ws_port, ws_compress, cli_symbol, cli_stream,
            cli_api_key, cli_api_secret, cli_host, cli_port,
            cli_record_path, cli_replay_data_path, cli_live, cli_mode,
            cli_db_path, cli_balance, cli_risk_fraction, cli_sl_pct, cli_tp_pct,
            cli_backfill, cli_backfill_interval,
            risk_max_position_value, risk_max_drawdown, risk_max_loss_per_trade,
            risk_max_open_orders, risk_max_portfolio_exposure,
            risk_max_daily_loss, risk_daily_reset_hour,
            risk_max_trades_per_hour, risk_max_orders_per_minute,
            cli_rolling_window, cli_risk_free_rate, cli_output, cli_output_format);

        // Restore CLI-set values (CLI wins over file)
        if (was_set("--replay")) replay_path = save_replay;
        if (was_set("--log-events")) event_log_path = save_event_log;
        if (was_set("--seed")) seed = save_seed;
        if (was_set("--thread-preset")) thread_preset_str = save_preset;
        if (was_set("--no-pin")) no_pin = save_no_pin;
        if (was_set("--provider")) provider_name = save_provider;
        if (was_set("--path")) provider_path = save_path;
        if (was_set("--strategy")) cli_strategy = save_strategy;
        if (was_set("--format")) cli_format = save_format;
        if (was_set("--sma-period")) cli_sma_period = save_sma;
        if (was_set("--fee")) cli_fee_model = save_fee;
        if (was_set("--fee-value")) cli_fee_value = save_fee_value;
        if (was_set("--maker-rate")) cli_maker_rate = save_maker;
        if (was_set("--taker-rate")) cli_taker_rate = save_taker;
        if (was_set("--web-ui")) enable_web_ui = save_webui;
        if (was_set("--ws-port")) ws_port = save_wsport;
        if (was_set("--ws-compress")) ws_compress = save_wscompress;
        if (was_set("--symbol")) cli_symbol = save_symbol;
        if (was_set("--stream")) cli_stream = save_stream;
        if (was_set("--api-key")) cli_api_key = save_apikey;
        if (was_set("--api-secret")) cli_api_secret = save_apisecret;
        if (was_set("--host")) cli_host = save_host;
        if (was_set("--port")) cli_port = save_port;
        if (was_set("--record")) cli_record_path = save_record;
        if (was_set("--replay-data")) cli_replay_data_path = save_replay_data;
        if (was_set("--live")) cli_live = save_live;
        if (was_set("--mode")) cli_mode = save_mode;
        if (was_set("--db")) cli_db_path = save_db;
        if (was_set("--balance")) cli_balance = save_balance;
        if (was_set("--risk-fraction")) cli_risk_fraction = save_risk_fraction;
        if (was_set("--sl")) cli_sl_pct = save_sl;
        if (was_set("--tp")) cli_tp_pct = save_tp;
        if (was_set("--backfill")) cli_backfill = save_backfill;
        if (was_set("--backfill-interval")) cli_backfill_interval = save_backfill_interval;
        if (was_set("--rolling-window")) cli_rolling_window = save_rolling_window;
        if (was_set("--risk-free-rate")) cli_risk_free_rate = save_risk_free_rate;
        if (was_set("--output")) cli_output = save_output;
        if (was_set("--output-format")) cli_output_format = save_output_format;
    }

    // --no-db clears db path
    if (no_db) cli_db_path.clear();

    // --- --dump-config: print resolved config as JSON and exit ---
    if (dump_config_flag)
    {
        dump_config(replay_path, event_log_path, seed, thread_preset_str, no_pin,
                    provider_name, provider_path, cli_strategy, cli_format,
                    cli_sma_period, cli_fee_model, cli_fee_value, cli_maker_rate,
                    cli_taker_rate, enable_web_ui, ws_port, ws_compress, cli_symbol, cli_stream,
                    cli_host, cli_port, cli_record_path, cli_replay_data_path,
                    cli_live, cli_mode, cli_db_path, cli_balance, cli_risk_fraction,
                    cli_sl_pct, cli_tp_pct, cli_backfill, cli_backfill_interval,
                    risk_max_position_value, risk_max_drawdown, risk_max_loss_per_trade,
                    risk_max_open_orders, risk_max_portfolio_exposure,
                    risk_max_daily_loss, risk_daily_reset_hour,
                    risk_max_trades_per_hour, risk_max_orders_per_minute);
        return 0;
    }

    // --- --dry-run: validate config, print summary, exit ---
    if (dry_run)
    {
        std::string resolved_strategy = cli_strategy.empty() ? "mean-reversion" : cli_strategy;
        std::string resolved_mode = cli_mode.empty() ? "backtest" : cli_mode;
        std::string resolved_preset = thread_preset_str.empty()
            ? preset_to_string(select_preset(detect_physical_cores()))
            : thread_preset_str;

        std::cout << "\n";
        std::cout << "  ============================================\n";
        std::cout << "    Dry-run config validation\n";
        std::cout << "  ============================================\n";
        std::cout << "    Mode:       " << resolved_mode << "\n";
        std::cout << "    Strategy:   " << resolved_strategy << "\n";
        std::cout << "    SMA Period: " << cli_sma_period << "\n";
        std::cout << "    Balance:    $" << cli_balance << "\n";
        std::cout << "    Risk:       " << (cli_risk_fraction * 100) << "% per trade\n";
        std::cout << "    SL/TP:      " << (cli_sl_pct * 100) << "% / " << (cli_tp_pct * 100) << "%\n";
        std::string resolved_spin = spin_policy_str.empty() ? "adaptive" : spin_policy_str;
        std::cout << "    Threading:  " << resolved_preset << " (" << resolved_spin << " spin)\n";
        std::cout << "    Provider:   " << (provider_name.empty() ? "(TUI)" : provider_name) << "\n";
        std::cout << "    Data path:  " << (provider_path.empty() ? "(none)" : provider_path) << "\n";
        std::cout << "    DB:         " << (cli_db_path.empty() ? "(disabled)" : cli_db_path) << "\n";
        std::cout << "    Fee model:  " << (cli_fee_model.empty() ? "zero" : cli_fee_model) << "\n";
        std::cout << "    Web UI:     " << (enable_web_ui ? "yes" : "no") << "\n";
        if (enable_web_ui)
            std::cout << "    WS port:    " << ws_port << "\n";
        std::cout << "    Seed:       " << (seed == 0 ? "random" : std::to_string(seed)) << "\n";
        std::cout << "  ── Risk Limits ──\n";
        std::cout << "    Max drawdown:        " << (risk_max_drawdown * 100) << "%\n";
        std::cout << "    Max position value:  $" << risk_max_position_value << "\n";
        std::cout << "    Max loss/trade:      $" << risk_max_loss_per_trade << "\n";
        std::cout << "    Max open orders:     " << risk_max_open_orders << "\n";
        std::cout << "    Max exposure:        $" << risk_max_portfolio_exposure << "\n";
        if (risk_max_daily_loss > 0.0)
            std::cout << "    Max daily loss:      $" << risk_max_daily_loss << " (reset at " << risk_daily_reset_hour << ":00 UTC)\n";
        if (risk_max_trades_per_hour > 0)
            std::cout << "    Max trades/hour:     " << risk_max_trades_per_hour << "\n";
        if (risk_max_orders_per_minute > 0)
            std::cout << "    Max orders/minute:   " << risk_max_orders_per_minute << "\n";
        std::cout << "  ============================================\n";

        // Validate
        bool valid = true;
        if (!cli_strategy.empty() && !StrategyRegistry::instance().has(cli_strategy))
        {
            std::cerr << "  ! Unknown strategy: " << cli_strategy << "\n";
            valid = false;
        }
        if (!cli_mode.empty() &&
            cli_mode != "backtest" &&
            cli_mode != "shadow" &&
            cli_mode != "live")
        {
            std::cerr << "  ! Unknown mode: " << cli_mode << "\n";
            valid = false;
        }
        if (!cli_fee_model.empty() &&
            cli_fee_model != "fixed" &&
            cli_fee_model != "tiered")
        {
            std::cerr << "  ! Unknown fee model: " << cli_fee_model << "\n";
            valid = false;
        }
        if (!thread_preset_str.empty())
        {
            try { string_to_preset(thread_preset_str); }
            catch (...) {
                std::cerr << "  ! Unknown thread preset: " << thread_preset_str << "\n";
                valid = false;
            }
        }

        std::cout << "\n  Config is " << (valid ? "VALID" : "INVALID") << ".\n";
        return valid ? 0 : 1;
    }

    // --- Replay mode: skip TUI, replay events from log ---
    if (!replay_path.empty())
    {
        std::cout << "\n  Replaying events from: " << replay_path << "\n";

        // Strategy still needed for replay
        std::cout << "  Using Mean Reversion strategy (default) for replay.\n";
        auto strategy = std::make_shared<mean_reversion_strategy>(20);
        apply_strategy_params(*strategy, cli_params);

        engine_config cfg;
        cfg.seed = seed;
        cfg.event_log_path = event_log_path;
        cfg.compress_log = compress_log;
        cfg.log_max_bytes = log_max_size_mb * 1024ull * 1024ull;
        cfg.log_max_files = log_keep;

        auto dh = std::make_shared<data_handler>();
        engine eng(dh, nullptr, strategy, std::move(cfg));
        eng.run_replay(replay_path, replay_from_us, replay_to_us);
        eng.print_summary();
        export_results(eng.get_analytics(), cli_output, cli_output_format);
        return 0;
    }

    // --- Provider mode: skip TUI, use registry-based provider ---
    if (!provider_name.empty())
    {
        // M2: for multi-file --path, pass only the first path to the provider
        // (each additional file is loaded separately via FileTransport below).
        std::string primary_path = provider_path;
        {
            auto comma = primary_path.find(',');
            if (comma != std::string::npos)
                primary_path = primary_path.substr(0, comma);
        }

        provider_config pcfg;
        if (!primary_path.empty()) pcfg["path"] = primary_path;
        if (!cli_symbol.empty())    pcfg["symbol"] = cli_symbol;
        if (!cli_stream.empty())    pcfg["stream"] = cli_stream;
        if (!cli_api_key.empty())   pcfg["api_key"] = cli_api_key;
        if (!cli_api_secret.empty()) pcfg["api_secret"] = cli_api_secret;
        if (!cli_host.empty())      pcfg["host"] = cli_host;
        if (!cli_port.empty())      pcfg["port"] = cli_port;

        std::shared_ptr<IProvider> provider;
        try {
            provider = ProviderRegistry::instance().create(provider_name, pcfg);
        } catch (const std::exception& e) {
            std::cerr << "  ! " << e.what() << "\n";
            if (provider_name == "binance")
                std::cerr << "    Example: --provider binance --symbol btcusdt --stream trade\n";
            else if (provider_name == "local")
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
        std::string resolved_raw = cli_strategy.empty() ? "mean-reversion" : cli_strategy;
        auto strategy_names = split_csv(resolved_raw);
        if (strategy_names.empty()) strategy_names.push_back("mean-reversion");
        const std::string& resolved = strategy_names.front();
        auto prov_strategy = StrategyRegistry::instance().create(resolved);
        // Apply legacy CLI params as set_param calls (primary only)
        if (resolved == "sma" && cli_sma_period != 20)
            prov_strategy->set_param("period", static_cast<double>(cli_sma_period));
        if (resolved == "mean-reversion") {
            if (cli_sma_period != 20) prov_strategy->set_param("period", static_cast<double>(cli_sma_period));
            if (cli_balance != 10000.0) prov_strategy->set_param("equity", cli_balance);
            if (cli_risk_fraction != 0.02) prov_strategy->set_param("risk_fraction", cli_risk_fraction);
            if (cli_sl_pct != 0.005) prov_strategy->set_param("sl_pct", cli_sl_pct);
            if (cli_tp_pct != 0.01) prov_strategy->set_param("tp_pct", cli_tp_pct);
        }
        apply_strategy_params(*prov_strategy, cli_params);

        // Build additional strategies for M1 multi-strategy mode
        std::vector<std::pair<std::string, std::shared_ptr<IStrategy>>> extra_strategies;
        for (std::size_t si = 1; si < strategy_names.size(); ++si) {
            const auto& nm = strategy_names[si];
            if (!StrategyRegistry::instance().has(nm)) {
                std::cerr << "  ! Unknown additional strategy: " << nm << "\n";
                return 1;
            }
            auto extra = StrategyRegistry::instance().create(nm);
            apply_strategy_params(*extra, cli_params);
            extra_strategies.emplace_back(nm, std::move(extra));
        }

        // Build engine config
        engine_config prov_cfg;
        prov_cfg.seed = seed;
        prov_cfg.event_log_path = event_log_path;
        prov_cfg.compress_log = compress_log;
        prov_cfg.log_max_bytes = log_max_size_mb * 1024ull * 1024ull;
        prov_cfg.log_max_files = log_keep;
        prov_cfg.disable_pinning = no_pin;
        prov_cfg.provider = provider;
        prov_cfg.enable_web_ui = enable_web_ui;
        prov_cfg.ws_port = ws_port;
        prov_cfg.ws_compress = ws_compress;
        prov_cfg.initial_balance = cli_balance;
        prov_cfg.db_path = cli_db_path;
        prov_cfg.checkpoint_path = cli_checkpoint_path;
        prov_cfg.resume_checkpoint_path = cli_resume_path;
        prov_cfg.checkpoint_interval_events = cli_checkpoint_interval;
        prov_cfg.backfill_bars = cli_backfill;
        prov_cfg.backfill_interval = cli_backfill_interval;
        prov_cfg.market_aggression = cli_market_aggression;
        prov_cfg.qty_scale = cli_qty_scale;
        prov_cfg.fill_rng_seed = cli_fill_rng_seed;
        prov_cfg.spread_step_factor = cli_spread_step;
        prov_cfg.rolling_window = cli_rolling_window;
        prov_cfg.risk_free_rate = cli_risk_free_rate;

        // Apply risk limits
        prov_cfg.risk.max_position_value = risk_max_position_value;
        prov_cfg.risk.max_drawdown = risk_max_drawdown;
        prov_cfg.risk.max_loss_per_trade = risk_max_loss_per_trade;
        prov_cfg.risk.max_open_orders = risk_max_open_orders;
        prov_cfg.risk.max_portfolio_exposure = risk_max_portfolio_exposure;
        prov_cfg.risk.max_daily_loss = risk_max_daily_loss;
        prov_cfg.risk.daily_reset_hour = risk_daily_reset_hour;
        prov_cfg.risk.max_trades_per_hour = risk_max_trades_per_hour;
        prov_cfg.risk.max_orders_per_minute = risk_max_orders_per_minute;
        prov_cfg.risk_unwind = risk_unwind;

        // Map WS host to REST host for backfill
        if (!cli_host.empty()) {
            if (cli_host.find("testnet") != std::string::npos)
                prov_cfg.backfill_host = "testnet.binance.vision";
            else
                prov_cfg.backfill_host = "api.binance.com";
        }

        if (!thread_preset_str.empty())
            prov_cfg.threading = string_to_preset(thread_preset_str);
        else
            prov_cfg.threading = select_preset(detect_physical_cores());

        if (!spin_policy_str.empty())
            prov_cfg.worker_spin_policy = string_to_spin_policy(spin_policy_str);

        // Fee model
        if (cli_fee_model == "fixed")
            prov_cfg.fee_model = std::make_shared<FixedFeeModel>(cli_fee_value);
        else if (cli_fee_model == "tiered")
            prov_cfg.fee_model = std::make_shared<TieredFeeModel>(cli_maker_rate, cli_taker_rate);
        else if (provider_name == "binance")
            prov_cfg.fee_model = std::make_shared<TieredFeeModel>(0.001, 0.001);

        // Engine mode from CLI
        if (cli_mode == "shadow")
            prov_cfg.mode = engine_mode::shadow;
        else if (cli_mode == "live" || cli_live)
            prov_cfg.mode = engine_mode::live;

        // Safety: live mode requires explicit --live flag + API keys
        if (prov_cfg.mode == engine_mode::live)
        {
            if (!cli_live)
            {
                std::cerr << "  ! Live mode requires --live flag.\n";
                return 1;
            }
            if (cli_api_key.empty() || cli_api_secret.empty())
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

        // Open the provider (connects WebSocket, initializes transport, etc.)
        if (provider->has_data_feed() && !provider->open())
        {
            std::cerr << "  ! Provider failed to open.\n";
            return 1;
        }

        // Get transport from provider (instead of always using FileTransport)
        auto transport = provider->get_transport();
        if (!transport)
        {
            // Fallback: local file transport for providers without one
            transport = std::make_shared<FileTransport>(primary_path);
        }

#ifdef HAS_BINANCE
        // Wrap transport in RecordingTransport if --record is specified
        if (!cli_record_path.empty())
        {
            transport = std::make_shared<RecordingTransport>(transport, cli_record_path);
        }

        // Override transport with ReplayTransport if --replay-data is specified
        if (!cli_replay_data_path.empty())
        {
            transport = std::make_shared<ReplayTransport>(cli_replay_data_path);
        }
#endif

        auto dh = std::make_shared<data_handler>();

        // Determine data format: tick vs bar
        // Binance trade stream → tick, kline → bar; local uses --format flag
        bool is_tick = (cli_format == "tick");
        bool is_streaming = transport->is_streaming();

#ifdef HAS_BINANCE
        if (provider_name == "binance" || is_streaming)
        {
            if (cli_stream.empty() || cli_stream == "trade")
                is_tick = true;
            else if (cli_stream.find("kline") != std::string::npos)
                is_tick = false;
        }
#endif

        std::cout << "\n";
        std::cout << "  ============================================\n";
        std::cout << "    Provider:  " << provider_name << "\n";
        std::cout << "    Format:    " << (is_tick ? "tick" : "bar") << "\n";
        std::cout << "    Strategy:  " << (cli_strategy.empty() ? "mean-reversion" : cli_strategy) << "\n";
        std::cout << "    SMA:       " << cli_sma_period << "\n";
        std::cout << "    Balance:   $" << cli_balance << "\n";
        std::cout << "    Risk:      " << (cli_risk_fraction * 100) << "% per trade\n";
        std::cout << "    SL/TP:     " << (cli_sl_pct * 100) << "% / " << (cli_tp_pct * 100) << "%\n";
        std::cout << "    Fees:      " << (prov_cfg.fee_model ? "yes" : "none") << "\n";
        std::cout << "    Streaming: " << (is_streaming ? "yes" : "no") << "\n";
        std::cout << "    Threading: " << preset_to_string(prov_cfg.threading)
                  << " (" << preset_worker_count(prov_cfg.threading) << " workers)\n";
        if (!cli_record_path.empty())
            std::cout << "    Recording: " << cli_record_path << "\n";
        if (!cli_replay_data_path.empty())
            std::cout << "    Replay:    " << cli_replay_data_path << "\n";
        std::cout << "  ============================================\n\n";

        if (is_streaming)
        {
            // Streaming path: engine processes records as they arrive
            engine eng(dh, nullptr, prov_strategy, std::move(prov_cfg));
            eng.set_primary_strategy_name(resolved);
            for (auto& es : extra_strategies)
                eng.add_strategy(es.second, es.first);

            if (is_tick)
            {
                std::shared_ptr<IDataParser<tick_record>> parser;
#ifdef HAS_BINANCE
                if (provider_name == "binance")
                    parser = std::make_shared<BinanceTradeParser>();
                else
#endif
                    parser = std::make_shared<CsvTickParser>();

                auto bridge = std::make_shared<DataBridge<tick_record>>(
                    transport, parser, tick_record_sink);
                eng.run_streaming(bridge);
            }
            else
            {
                std::shared_ptr<IDataParser<bar_record>> parser;
#ifdef HAS_BINANCE
                if (provider_name == "binance")
                    parser = std::make_shared<BinanceKlineParser>();
                else
#endif
                    parser = std::make_shared<CsvBarParser>();

                auto bridge = std::make_shared<DataBridge<bar_record>>(
                    transport, parser, bar_record_sink);
                eng.run_streaming(bridge);
            }

            eng.print_summary();
            export_results(eng.get_analytics(), cli_output, cli_output_format);
        }
        else
        {
            // Batch path: load all data first, then run engine
            if (is_tick)
            {
                std::shared_ptr<IDataParser<tick_record>> parser;
#ifdef HAS_BINANCE
                if (provider_name == "binance")
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
                if (provider_name == "binance")
                    parser = std::make_shared<BinanceKlineParser>();
                else
#endif
                    parser = std::make_shared<CsvBarParser>();

                // M2: comma-separated --path loads multiple CSVs into the
                // same data_handler for multi-symbol backtesting. Only works
                // for the local provider (each path needs its own transport).
                auto paths = split_csv(provider_path);
                if (paths.size() > 1 && provider_name == "local")
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
            export_results(eng.get_analytics(), cli_output, cli_output_format);
        }
        return 0;
    }

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
    apply_strategy_params(*strategy, cli_params);

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
    config.seed = seed;
    config.event_log_path = event_log_path;
    config.compress_log = compress_log;
    config.log_max_bytes = log_max_size_mb * 1024ull * 1024ull;
    config.log_max_files = log_keep;
    config.disable_pinning = no_pin;
    config.db_path = cli_db_path;
    config.checkpoint_path = cli_checkpoint_path;
    config.resume_checkpoint_path = cli_resume_path;
    config.checkpoint_interval_events = cli_checkpoint_interval;
    config.market_aggression = cli_market_aggression;
    config.qty_scale = cli_qty_scale;
    config.fill_rng_seed = cli_fill_rng_seed;
    config.spread_step_factor = cli_spread_step;
    config.rolling_window = cli_rolling_window;
    config.risk_free_rate = cli_risk_free_rate;

    // Apply risk limits
    config.risk.max_position_value = risk_max_position_value;
    config.risk.max_drawdown = risk_max_drawdown;
    config.risk.max_loss_per_trade = risk_max_loss_per_trade;
    config.risk.max_open_orders = risk_max_open_orders;
    config.risk.max_portfolio_exposure = risk_max_portfolio_exposure;
    config.risk.max_daily_loss = risk_max_daily_loss;
    config.risk.daily_reset_hour = risk_daily_reset_hour;
    config.risk.max_trades_per_hour = risk_max_trades_per_hour;
    config.risk.max_orders_per_minute = risk_max_orders_per_minute;
    config.risk_unwind = risk_unwind;

    // Thread preset: CLI override or auto-detect
    if (!thread_preset_str.empty())
        config.threading = string_to_preset(thread_preset_str);
    else
        config.threading = select_preset(detect_physical_cores());

    if (!spin_policy_str.empty())
        config.worker_spin_policy = string_to_spin_policy(spin_policy_str);

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
    // else: config.fee_model stays nullptr (zero fees)

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
    // else: default backtest

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
    export_results(eng.get_analytics(), cli_output, cli_output_format);

    return 0;
}
