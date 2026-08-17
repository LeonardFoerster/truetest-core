
#include "truetest_api.h"

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/csv_data_source.h"
#include "data/data_handler.h"
#include "data/data_wrapper.h"
#include "data/market_series.h"
#include "strategy/strategy_interface.h"
#include "strategy/strategy_registry.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <exception>
#include <memory>
#include <string>

namespace {

thread_local std::string g_last_error;

void set_last_error(const std::string& msg) { g_last_error = msg; }
void clear_last_error() { g_last_error.clear(); }

struct EngineWrapper
{
    engine_config config;
    std::string data_path;
    std::string strategy_name{"mean-reversion"};
    std::vector<std::pair<std::string, double>> strategy_params;

    std::shared_ptr<data_handler> dh;
    std::shared_ptr<IStrategy> strategy;
    std::unique_ptr<engine> eng;

    bool has_run = false;
};

char* dup_c_string(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

std::string report_to_json(const AnalyticsReport& r)
{
    nlohmann::json j;
    j["initial_equity"]       = r.initial_equity;
    j["final_equity"]         = r.final_equity;
    j["cumulative_return"]    = r.cumulative_return;
    j["sharpe_ratio"]         = r.sharpe_ratio;
    j["sortino_ratio"]        = r.sortino_ratio;
    j["max_drawdown"]         = r.max_drawdown;
    j["calmar_ratio"]         = r.calmar_ratio;
    j["rolling_sharpe"]       = r.rolling_sharpe;
    j["rolling_max_drawdown"] = r.rolling_max_drawdown;
    j["win_rate"]             = r.win_rate;
    j["profit_factor"]        = r.profit_factor;
    j["total_trades"]         = r.total_trades;
    j["total_orders"]         = r.total_orders;
    j["total_fills"]          = r.total_fills;
    j["avg_win"]              = r.avg_win;
    j["avg_loss"]             = r.avg_loss;
    j["largest_winner"]       = r.largest_winner;
    j["largest_loser"]        = r.largest_loser;
    j["time_in_market_pct"]   = r.time_in_market_pct;
    j["avg_slippage"]         = r.avg_slippage;
    j["buy_and_hold_return"]  = r.buy_and_hold_return;
    j["strategy_vs_benchmark"] = r.strategy_vs_benchmark;
    j["alpha"]                = r.alpha;
    j["beta"]                 = r.beta;
    j["information_ratio"]    = r.information_ratio;
    j["tracking_error"]       = r.tracking_error;

    nlohmann::json eq = nlohmann::json::array();
    for (const auto& p : r.equity_curve)
    {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            p.timestamp.time_since_epoch()).count();
        eq.push_back({static_cast<long long>(ms), p.equity});
    }
    j["equity_curve"] = std::move(eq);

    nlohmann::json per_sym = nlohmann::json::object();
    for (const auto& [sym, sa] : r.per_symbol)
    {
        per_sym[sym] = {
            {"total_pnl",     sa.total_pnl},
            {"trade_count",   sa.trade_count},
            {"win_rate",      sa.win_rate()},
            {"profit_factor", sa.profit_factor()},
        };
    }
    j["per_symbol"] = std::move(per_sym);

    nlohmann::json per_strat = nlohmann::json::object();
    for (const auto& [name, sa] : r.per_strategy)
    {
        per_strat[name] = {
            {"total_pnl",     sa.total_pnl},
            {"trade_count",   sa.trade_count},
            {"win_rate",      sa.win_rate()},
            {"profit_factor", sa.profit_factor()},
        };
    }
    j["per_strategy"] = std::move(per_strat);

    return j.dump();
}

}

extern "C" {

const char* tt_version(void) { return "0.1.0"; }

const char* tt_last_error(void) { return g_last_error.c_str(); }

tt_engine_handle tt_create_engine(const char* config_json)
{
    clear_last_error();

    if (!config_json)
    {
        set_last_error("tt_create_engine: config_json is NULL");
        return nullptr;
    }

    try
    {
        auto cfg = nlohmann::json::parse(config_json);

        auto w = std::make_unique<EngineWrapper>();

        if (!cfg.contains("data_path") || !cfg["data_path"].is_string())
        {
            set_last_error("config must contain string field 'data_path'");
            return nullptr;
        }
        w->data_path = cfg["data_path"].get<std::string>();

        if (cfg.contains("strategy") && cfg["strategy"].is_string())
            w->strategy_name = cfg["strategy"].get<std::string>();

        if (cfg.contains("initial_balance") && cfg["initial_balance"].is_number())
            w->config.initial_balance = cfg["initial_balance"].get<double>();
        if (cfg.contains("seed") && cfg["seed"].is_number_unsigned())
            w->config.seed = cfg["seed"].get<uint64_t>();
        if (cfg.contains("rolling_window") && cfg["rolling_window"].is_number_unsigned())
            w->config.rolling_window = cfg["rolling_window"].get<std::size_t>();
        if (cfg.contains("risk_free_rate") && cfg["risk_free_rate"].is_number())
            w->config.risk_free_rate = cfg["risk_free_rate"].get<double>();
        if (cfg.contains("market_aggression") && cfg["market_aggression"].is_number())
            w->config.market_aggression = cfg["market_aggression"].get<double>();
        if (cfg.contains("qty_scale") && cfg["qty_scale"].is_number())
            w->config.qty_scale = cfg["qty_scale"].get<double>();
        if (cfg.contains("fill_rng_seed") && cfg["fill_rng_seed"].is_number_unsigned())
            w->config.fill_rng_seed = cfg["fill_rng_seed"].get<unsigned>();
        if (cfg.contains("spread_step_factor") && cfg["spread_step_factor"].is_number())
            w->config.spread_step_factor = cfg["spread_step_factor"].get<double>();
        if (cfg.contains("event_log_path") && cfg["event_log_path"].is_string())
            w->config.event_log_path = cfg["event_log_path"].get<std::string>();

        if (cfg.contains("params") && cfg["params"].is_object())
        {
            for (auto it = cfg["params"].begin(); it != cfg["params"].end(); ++it)
            {
                if (it.value().is_number())
                    w->strategy_params.emplace_back(it.key(), it.value().get<double>());
            }
        }

        return w.release();
    }
    catch (const std::exception& e)
    {
        set_last_error(std::string("tt_create_engine: ") + e.what());
        return nullptr;
    }
}

int tt_run(tt_engine_handle handle)
{
    clear_last_error();

    auto* w = static_cast<EngineWrapper*>(handle);
    if (!w)
    {
        set_last_error("tt_run: handle is NULL");
        return 1;
    }

    try
    {
        if (!StrategyRegistry::instance().has(w->strategy_name))
        {
            set_last_error("unknown strategy: " + w->strategy_name);
            return 2;
        }

        w->strategy = StrategyRegistry::instance().create(w->strategy_name);
        for (const auto& [key, value] : w->strategy_params)
            w->strategy->set_param(key, value);

        w->dh = std::make_shared<data_handler>();
        // docs/internal/data-pipeline.md#D-05: load via DataWrapper façade (behaviour-identical CSV path)
        try
        {
            auto wrapper = DataWrapper::from_path(w->data_path);
            if (!wrapper.load(*w->dh))
            {
                set_last_error("failed to load data from: " + w->data_path);
                return 3;
            }
        }
        catch (const std::exception& e)
        {
            set_last_error(std::string("failed to load data: ") + e.what());
            return 3;
        }

        w->eng = std::make_unique<engine>(
            w->dh, nullptr, w->strategy, std::move(w->config));
        // Stamp primary so order_meta / fill attribution carry strategy_name
        // (dispatch also falls back when empty; this keeps FR-08 on_fill and
        // portfolio attribution consistent with named strategies).
        w->eng->set_primary_strategy_name(w->strategy_name);
        w->eng->run();
        w->has_run = true;
        return 0;
    }
    catch (const std::exception& e)
    {
        set_last_error(std::string("tt_run: ") + e.what());
        return 4;
    }
}

const char* tt_get_results(tt_engine_handle handle)
{
    clear_last_error();

    auto* w = static_cast<EngineWrapper*>(handle);
    if (!w || !w->eng || !w->has_run)
    {
        set_last_error("tt_get_results: engine has not been run");
        return nullptr;
    }

    try
    {
        auto report = w->eng->get_analytics().generate_report();
        return dup_c_string(report_to_json(report));
    }
    catch (const std::exception& e)
    {
        set_last_error(std::string("tt_get_results: ") + e.what());
        return nullptr;
    }
}

void tt_free_string(const char* str)
{
    if (str) std::free(const_cast<char*>(str));
}

void tt_destroy(tt_engine_handle handle)
{
    if (!handle) return;
    delete static_cast<EngineWrapper*>(handle);
}

}
