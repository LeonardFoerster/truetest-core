
#include "truetest_api.h"

#include "api/market_series_symbol_policy.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/csv_data_source.h"
#include "data/data_handler.h"
#include "data/data_wrapper.h"
#include "data/market_series.h"
#include "strategy/strategy_interface.h"
#include "strategy/strategy_registry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {

thread_local std::string g_last_error;

void set_last_error(const std::string& msg)
{
    g_last_error = msg;
}
void clear_last_error()
{
    g_last_error.clear();
}

constexpr std::array<std::string_view, 12> c_api_config_fields = {
    "data_path",      "symbol",         "strategy",         "initial_balance",   "seed",
    "rolling_window", "risk_free_rate", "periods_per_year", "market_aggression", "fill_rng_seed",
    "event_log_path", "params",
};

bool parse_strict_config_json(const char* config_json, nlohmann::json& config)
{
    std::vector<std::unordered_set<std::string>> object_keys;
    bool has_duplicate = false;
    std::string duplicate_key;
    auto reject_duplicate_keys = [&](int, nlohmann::json::parse_event_t event,
                                     nlohmann::json& parsed) {
        switch (event) {
        case nlohmann::json::parse_event_t::object_start:
            object_keys.emplace_back();
            break;
        case nlohmann::json::parse_event_t::key:
            if (object_keys.empty() ||
                !object_keys.back().insert(parsed.get<std::string>()).second) {
                if (!has_duplicate) duplicate_key = parsed.get<std::string>();
                has_duplicate = true;
            }
            break;
        case nlohmann::json::parse_event_t::object_end:
            if (!object_keys.empty()) object_keys.pop_back();
            break;
        default:
            break;
        }
        return true;
    };

    config = nlohmann::json::parse(config_json, reject_duplicate_keys);
    if (has_duplicate) {
        set_last_error("duplicate JSON object key '" + duplicate_key + "'");
        return false;
    }
    return true;
}

bool validate_config_fields(const nlohmann::json& config)
{
    if (!config.is_object()) {
        set_last_error("config root must be a JSON object");
        return false;
    }
    for (auto it = config.begin(); it != config.end(); ++it) {
        if (std::find(c_api_config_fields.begin(), c_api_config_fields.end(),
                      std::string_view{it.key()}) == c_api_config_fields.end()) {
            set_last_error("unknown config field '" + it.key() + "'");
            return false;
        }
    }
    return true;
}

template <typename Predicate>
bool read_optional_finite_double(const nlohmann::json& config, const char* key, double& destination,
                                 Predicate valid, const char* requirement)
{
    if (!config.contains(key)) return true;
    const auto& value = config.at(key);
    if (!value.is_number()) {
        set_last_error("config field '" + std::string{key} + "' must be a number");
        return false;
    }
    const double parsed = value.get<double>();
    if (!std::isfinite(parsed) || !valid(parsed)) {
        set_last_error("config field '" + std::string{key} + "' " + requirement);
        return false;
    }
    destination = parsed;
    return true;
}

template <typename UInt>
bool read_optional_unsigned(const nlohmann::json& config, const char* key, UInt& destination,
                            bool require_positive)
{
    static_assert(std::is_unsigned_v<UInt>);
    if (!config.contains(key)) return true;
    const auto& value = config.at(key);
    if (!value.is_number_unsigned()) {
        set_last_error("config field '" + std::string{key} + "' must be an unsigned integer");
        return false;
    }
    const auto raw = value.get<nlohmann::json::number_unsigned_t>();
    if (raw > static_cast<nlohmann::json::number_unsigned_t>(std::numeric_limits<UInt>::max())) {
        set_last_error("config field '" + std::string{key} +
                       "' exceeds its supported integer range");
        return false;
    }
    const UInt parsed = static_cast<UInt>(raw);
    if (require_positive && parsed == 0) {
        set_last_error("config field '" + std::string{key} + "' must be positive");
        return false;
    }
    destination = parsed;
    return true;
}

struct EngineWrapper
{
    engine_config config;
    std::string data_path;
    std::optional<std::string> expected_symbol;
    std::string strategy_name{"mean-reversion"};
    std::vector<std::pair<std::string, double>> strategy_params;

    std::shared_ptr<data_handler> dh;
    std::shared_ptr<IStrategy> strategy;
    std::unique_ptr<engine> eng;

    std::mutex run_mutex;
    bool run_attempted = false;
    int run_code = 0;
    std::string run_error;
    bool has_run = false;
};

int finish_run(EngineWrapper& wrapper, int code, std::string error)
{
    wrapper.run_code = code;
    wrapper.run_error = std::move(error);
    wrapper.has_run = code == 0;
    if (code != 0)
        set_last_error(wrapper.run_error);
    return code;
}

char* dup_c_string(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}


}

extern "C" {

const char* tt_version(void) { return "0.1.0"; }

const char* tt_last_error(void) { return g_last_error.c_str(); }

tt_engine_handle tt_create_engine(const char* config_json)
{
    clear_last_error();

    if (!config_json) {
        set_last_error("tt_create_engine: config_json is NULL");
        return nullptr;
    }

    try {
        nlohmann::json cfg;
        if (!parse_strict_config_json(config_json, cfg)) return nullptr;
        if (!validate_config_fields(cfg)) return nullptr;

        auto w = std::make_unique<EngineWrapper>();

        if (!cfg.contains("data_path") || !cfg["data_path"].is_string()) {
            set_last_error("config must contain string field 'data_path'");
            return nullptr;
        }
        w->data_path = cfg["data_path"].get<std::string>();
        if (w->data_path.empty() || w->data_path.find('\0') != std::string::npos) {
            set_last_error("config field 'data_path' must be a nonempty filesystem path");
            return nullptr;
        }

        if (cfg.contains("symbol")) {
            if (!cfg["symbol"].is_string()) {
                set_last_error("config field 'symbol' must be a string");
                return nullptr;
            }
            w->expected_symbol = cfg["symbol"].get<std::string>();
            if (!tt::api::valid_expected_symbol(*w->expected_symbol)) {
                set_last_error("config field 'symbol' must be nonempty and contain no "
                               "ASCII whitespace/control bytes");
                return nullptr;
            }
        }

        if (cfg.contains("strategy")) {
            if (!cfg["strategy"].is_string()) {
                set_last_error("config field 'strategy' must be a string");
                return nullptr;
            }
            w->strategy_name = cfg["strategy"].get<std::string>();
            if (w->strategy_name.empty()) {
                set_last_error("config field 'strategy' must be nonempty");
                return nullptr;
            }
        }

        if (!read_optional_finite_double(
                cfg, "initial_balance", w->config.initial_balance,
                [](double value) { return value > 0.0; }, "must be finite and positive") ||
            !read_optional_unsigned(cfg, "seed", w->config.seed, false) ||
            !read_optional_unsigned(cfg, "rolling_window", w->config.rolling_window, true) ||
            !read_optional_finite_double(
                cfg, "risk_free_rate", w->config.risk_free_rate, [](double) { return true; },
                "must be finite") ||
            !read_optional_unsigned(cfg, "periods_per_year", w->config.periods_per_year, true) ||
            !read_optional_finite_double(
                cfg, "market_aggression", w->config.market_aggression,
                [](double value) { return value > 1.0 && value < 2.0; },
                "must be finite and in the open interval (1, 2)") ||
            !read_optional_unsigned(cfg, "fill_rng_seed", w->config.fill_rng_seed, false))
            return nullptr;

        if (!cfg.contains("seed")) {
            set_last_error("Backtest requires explicit deterministic seed");
            return nullptr;
        }
        w->config.seed_explicitly_set = true;

        if (cfg.contains("seed") && cfg.contains("fill_rng_seed"))
        {
            set_last_error(
                "config field 'fill_rng_seed' cannot override the deterministic master-seed hierarchy");
            return nullptr;
        }

        if (cfg.contains("event_log_path")) {
            if (!cfg["event_log_path"].is_string()) {
                set_last_error("config field 'event_log_path' must be a string");
                return nullptr;
            }
            w->config.event_log_path = cfg["event_log_path"].get<std::string>();
            if (w->config.event_log_path.find('\0') != std::string::npos) {
                set_last_error("config field 'event_log_path' contains an embedded NUL");
                return nullptr;
            }
        }

        if (cfg.contains("params")) {
            if (!cfg["params"].is_object()) {
                set_last_error("config field 'params' must be an object");
                return nullptr;
            }
            for (auto it = cfg["params"].begin(); it != cfg["params"].end(); ++it) {
                if (it.key().empty() || !it.value().is_number()) {
                    set_last_error(
                        "every strategy param must have a nonempty name and numeric value");
                    return nullptr;
                }
                const double value = it.value().get<double>();
                if (!std::isfinite(value)) {
                    set_last_error("strategy param '" + it.key() + "' must be finite");
                    return nullptr;
                }
                w->strategy_params.emplace_back(it.key(), value);
            }
        }

        return w.release();
    } catch (const std::exception& e) {
        set_last_error(std::string("tt_create_engine: ") + e.what());
        return nullptr;
    } catch (...) {
        set_last_error("tt_create_engine: unknown non-standard exception");
        return nullptr;
    }
}

int tt_run(tt_engine_handle handle)
{
    clear_last_error();

    auto* w = static_cast<EngineWrapper*>(handle);
    if (!w) {
        set_last_error("tt_run: handle is NULL");
        return 1;
    }

    try {
        std::lock_guard run_lock(w->run_mutex);
        if (w->run_attempted) {
            if (w->run_code != 0) set_last_error(w->run_error);
            return w->run_code;
        }
        w->run_attempted = true;

        try {
            if (!StrategyRegistry::instance().has(w->strategy_name))
                return finish_run(*w, 2, "unknown strategy: " + w->strategy_name);

            w->strategy = StrategyRegistry::instance().create(w->strategy_name);
            for (const auto& [key, value] : w->strategy_params)
                w->strategy->set_param(key, value);

            w->dh = std::make_shared<data_handler>();
            // docs/internal/data-pipeline.md#D-05: load via DataWrapper façade (behaviour-identical
            // CSV path)
            try {
                DataLoadOptions load_options;
                load_options.fail_on_rejected_rows = true;
                auto wrapper = DataWrapper::from_path(w->data_path, load_options);
                if (!wrapper.load(*w->dh)) {
                    const auto& stats = wrapper.last_load_stats();
                    if (stats.rejected > 0) {
                        return finish_run(*w, 3,
                                          "failed to load data from '" + w->data_path +
                                              "': rejected " + std::to_string(stats.rejected) +
                                              " malformed or invalid record(s)");
                    }
                    return finish_run(*w, 3, "failed to load data from: " + w->data_path);
                }
            } catch (const std::exception& e) {
                return finish_run(*w, 3, std::string("failed to load data: ") + e.what());
            }

            const auto expected = w->expected_symbol
                                      ? std::optional<std::string_view>{*w->expected_symbol}
                                      : std::nullopt;
            const auto symbol_result = tt::api::enforce_series_symbol_policy(*w->dh, expected);
            if (!symbol_result.success()) {
                std::string error;
                switch (symbol_result.error) {
                case tt::api::series_symbol_error::unbound_without_expected_symbol:
                    error = "loaded data contains records without an instrument symbol; "
                            "provide config field 'symbol' or a symbol column";
                    break;
                case tt::api::series_symbol_error::expected_symbol_mismatch:
                    error = "loaded data symbol '" + std::string(symbol_result.observed_symbol) +
                            "' does not match configured symbol '" + *w->expected_symbol + "'";
                    break;
                case tt::api::series_symbol_error::invalid_expected_symbol:
                    error = "configured instrument symbol is invalid";
                    break;
                case tt::api::series_symbol_error::none:
                    break;
                }
                w->dh.reset();
                return finish_run(*w, 3, std::move(error));
            }

            w->eng = std::make_unique<engine>(w->dh, nullptr, w->strategy, std::move(w->config));
            // Stamp primary so order_meta / fill attribution carry strategy_name
            // (dispatch also falls back when empty; this keeps FR-08 on_fill and
            // portfolio attribution consistent with named strategies).
            w->eng->set_primary_strategy_name(w->strategy_name);
            w->eng->run();
            if (!w->eng->run_succeeded()) return finish_run(*w, 5, "engine run halted or failed");
            return finish_run(*w, 0, {});
        } catch (const std::exception& e) {
            return finish_run(*w, 4, std::string("tt_run: ") + e.what());
        } catch (...) {
            return finish_run(*w, 4, "tt_run: unknown non-standard exception");
        }
    } catch (const std::system_error& e) {
        set_last_error(std::string("tt_run: failed to lock engine handle: ") + e.what());
        return 4;
    } catch (...) {
        set_last_error("tt_run: failed before entering the protected engine run");
        return 4;
    }
}

const char* tt_get_results(tt_engine_handle handle)
{
    clear_last_error();

    auto* w = static_cast<EngineWrapper*>(handle);
    if (!w) {
        set_last_error("tt_get_results: engine has not been run");
        return nullptr;
    }

    try {
        std::lock_guard run_lock(w->run_mutex);
        if (!w->eng || !w->has_run) {
            set_last_error("tt_get_results: engine has not been run successfully");
            return nullptr;
        }

        auto report = w->eng->get_analytics().generate_report();
        char* result = dup_c_string(report.to_results_json());
        if (!result) set_last_error("tt_get_results: result allocation failed");
        return result;
    } catch (const std::exception& e) {
        set_last_error(std::string("tt_get_results: ") + e.what());
        return nullptr;
    } catch (...) {
        set_last_error("tt_get_results: unknown non-standard exception");
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
