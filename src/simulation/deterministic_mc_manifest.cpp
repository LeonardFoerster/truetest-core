#include "deterministic_mc_manifest.h"

#include "deterministic_trial_artifacts.h"
#include "reproducibility/deterministic_seed.h"
#include "simulation/monte_carlo_aggregate.h"
#include "strategy/deterministic_parameter_config.h"
#include "strategy/strategy_registry.h"
#include "strategy/strategy_interface.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string_view>

namespace truetest::simulation {

namespace repro = truetest::reproducibility;

static_assert(kMaxMcGeneratorPaths
              <= repro::kRunManifestV1MaxMonteCarloTrials,
              "MC generator capacity exceeds the manifest schema-v1 trial bound");

namespace {

using Json = repro::CanonicalJsonValue;

Json versioned_model(std::string id, std::string version,
                     Json::Object config = {})
{
    return Json::object({
        {"config", std::move(config)},
        {"id", std::move(id)},
        {"version", std::move(version)},
    });
}

const Json::Array& array(const Json& value, std::string_view field)
{
    if (!value.is_array())
        throw std::invalid_argument(
            "deterministic MC manifest field is not an array: "
            + std::string(field));
    return value.as_array();
}

std::string string_field(const Json& value, std::string_view field,
                         bool empty_allowed = false)
{
    const auto& child = value.at(field);
    if (!child.is_string() || (!empty_allowed && child.as_string().empty()))
        throw std::invalid_argument(
            "deterministic MC manifest field is not a valid string: "
            + std::string(field));
    return child.as_string();
}

bool bool_field(const Json& value, std::string_view field)
{
    const auto& child = value.at(field);
    if (!child.is_bool())
        throw std::invalid_argument(
            "deterministic MC manifest field is not boolean: "
            + std::string(field));
    return child.as_bool();
}

double finite_field(const Json& value, std::string_view field)
{
    const double result = value.at(field).as_double();
    if (!std::isfinite(result))
        throw std::invalid_argument(
            "deterministic MC manifest contains a non-finite field: "
            + std::string(field));
    return result;
}

Json effective_strategy_parameters(const McRunConfig& config)
{
    const auto& registry = StrategyRegistry::instance();
    if (!registry.has(config.strategy_name))
        throw std::invalid_argument(
            "deterministic MC has unknown strategy '" + config.strategy_name
            + "'");
    auto strategy = registry.create(config.strategy_name);
    if (!strategy)
        throw std::invalid_argument(
            "deterministic MC strategy factory returned null for '"
            + config.strategy_name + "'");
    configure_deterministic_mc_strategy(*strategy, config);
    const auto values =
        truetest::strategy_config::effective_parameter_values(*strategy);

    Json::Object output;
    for (const auto& [name, value] : values)
        output.emplace(name, value);
    return output;
}

Json explicit_strategy_parameters(const McRunConfig& config)
{
    Json::Array parameters;
    parameters.reserve(config.strategy_params.size());
    for (const auto& [name, value] : config.strategy_params)
        parameters.push_back(Json::object({{"name", name}, {"value", value}}));
    return parameters;
}

Json seed_hierarchy(const McRunConfig& config)
{
    const repro::DeterministicSeedDeriver seeds(config.base_seed);
    Json::Array trials;
    trials.reserve(config.n_trials);
    for (std::size_t trial = 0; trial < config.n_trials; ++trial)
    {
        trials.push_back(Json::object({
            {"component_seeds", component_seed_json(config.base_seed, trial)},
            {"index", static_cast<std::uint64_t>(trial)},
            {"seed", seeds.trial_seed(static_cast<std::uint64_t>(trial))},
        }));
    }
    return Json::object({
        {"derivation_version", std::uint64_t{repro::kSeedDerivationVersion}},
        {"domains", Json::object({
            {"fill_model", seeds.derive(repro::SeedDomain::fill_model)},
            {"impact_model", seeds.derive(repro::SeedDomain::impact_model)},
            {"latency_model", seeds.derive(repro::SeedDomain::latency_model)},
            {"market_maker", seeds.derive(repro::SeedDomain::market_maker)},
            {"queue_model", seeds.derive(repro::SeedDomain::queue_model)},
            {"run", seeds.derive(repro::SeedDomain::run)},
            {"strategy", seeds.derive(repro::SeedDomain::strategy)},
            {"synthetic_l2", seeds.derive(repro::SeedDomain::synthetic_l2)},
            {"synthetic_price", seeds.derive(repro::SeedDomain::synthetic_price)},
            {"synthetic_volume", seeds.derive(repro::SeedDomain::synthetic_volume)},
        })},
        {"master_seed", config.base_seed},
        {"trials", std::move(trials)},
    });
}

void validate_supported_model(const Json& models, std::string_view field,
                              std::string_view expected_id,
                              std::string_view expected_version)
{
    const auto& model = models.at(field);
    if (string_field(model, "id") != expected_id
        || string_field(model, "version") != expected_version)
        throw std::invalid_argument(
            "unsupported deterministic model/version for "
            + std::string(field));
}

void validate_seed_hierarchy(const Json& hierarchy,
                             std::uint64_t master_seed,
                             std::size_t trial_count)
{
    if (hierarchy.at("derivation_version").as_u64()
        != repro::kSeedDerivationVersion)
        throw std::invalid_argument("unsupported seed derivation version");
    if (hierarchy.at("master_seed").as_u64() != master_seed)
        throw std::invalid_argument("manifest master seed is contradictory");
    const auto& trials = array(hierarchy.at("trials"), "seed trials");
    if (trials.size() != trial_count)
        throw std::invalid_argument("manifest trial seed count is contradictory");
    const repro::DeterministicSeedDeriver deriver(master_seed);
    for (std::size_t index = 0; index < trials.size(); ++index)
    {
        const auto& trial = trials[index];
        if (trial.at("index").as_u64() != index
            || trial.at("seed").as_u64()
                != deriver.trial_seed(static_cast<std::uint64_t>(index))
            || trial.at("component_seeds")
                != component_seed_json(master_seed, index))
            throw std::invalid_argument(
                "manifest contains a contradictory derived trial seed");
    }
}

std::string first_mismatch_path(const Json& expected, const Json& actual,
                                std::string path)
{
    if (expected == actual)
        return {};
    if (expected.is_object() && actual.is_object())
    {
        for (const auto& [key, value] : expected.as_object())
        {
            const auto found = actual.as_object().find(key);
            const std::string child_path = path + "." + key;
            if (found == actual.as_object().end())
                return child_path + " (missing)";
            if (const std::string mismatch = first_mismatch_path(
                    value, found->second, child_path); !mismatch.empty())
                return mismatch;
        }
        for (const auto& [key, value] : actual.as_object())
        {
            (void)value;
            if (!expected.as_object().contains(key))
                return path + "." + key + " (unexpected)";
        }
    }
    else if (expected.is_array() && actual.is_array())
    {
        if (expected.as_array().size() != actual.as_array().size())
            return path + " (array size)";
        for (std::size_t index = 0; index < expected.as_array().size(); ++index)
            if (const std::string mismatch = first_mismatch_path(
                    expected.as_array()[index], actual.as_array()[index],
                    path + "[" + std::to_string(index) + "]");
                !mismatch.empty())
                return mismatch;
    }
    return path + " (value or type)";
}

} // namespace

void configure_deterministic_mc_strategy(IStrategy& strategy,
                                         const McRunConfig& config)
{
    using truetest::strategy_config::set_parameter_if_declared;
    const double entry_fee =
        (config.fee_model == "zero" || config.fee_model == "fixed") ? 0.0
        : config.taker_rate > 0.0 ? config.taker_rate
        : config.maker_rate > 0.0 ? config.maker_rate : 0.0;
    (void)set_parameter_if_declared(strategy, "entry_fee_rate", entry_fee);
    (void)set_parameter_if_declared(strategy, "exit_fee_rate", entry_fee);
    (void)set_parameter_if_declared(
        strategy, "fixed_fee_per_leg",
        config.fee_model == "fixed" ? config.fee_value : 0.0);
    const double model_slip_bps = config.mm_spread_pct > 0.0
        ? config.mm_spread_pct * 10'000.0 : 0.0;
    const double legacy_slip_bps = config.bar_spread_bps > 0.0
        ? config.bar_spread_bps * 0.5 : 0.0;
    const double slip_bps = std::max(model_slip_bps, legacy_slip_bps);
    (void)set_parameter_if_declared(strategy, "entry_slip_bps", slip_bps);
    (void)set_parameter_if_declared(strategy, "exit_slip_bps", slip_bps);

    (void)set_parameter_if_declared(
        strategy, "equity", config.initial_balance);
    (void)set_parameter_if_declared(
        strategy, "risk_fraction", config.risk_fraction);
    if ((config.strategy_name == "sma"
         || config.strategy_name == "mean-reversion")
        && config.sma_period != 20U)
        (void)set_parameter_if_declared(
            strategy, "period", static_cast<double>(config.sma_period));
    (void)set_parameter_if_declared(
        strategy, "sl_pct", config.exit_defaults.sl_pct);
    (void)set_parameter_if_declared(
        strategy, "tp_pct", config.exit_defaults.tp_pct);
    for (const auto& [name, value] : config.strategy_params)
        (void)set_parameter_if_declared(strategy, name, value, true);
}

repro::RunManifestV1 make_deterministic_mc_manifest(
    const McRunConfig& config)
{
    if (!config.master_seed_explicitly_set)
        throw std::invalid_argument(
            "deterministic MC manifest requires an explicit master seed");
    if (config.parallel_trials && config.max_parallel_threads == 0)
        throw std::invalid_argument(
            "deterministic parallel MC requires an explicit worker count");
    if (!config.persist_trial_lifecycle)
        throw std::invalid_argument(
            "deterministic MC manifest requires full trial lifecycle persistence");
    if (config.lifecycle_record_capacity == 0)
        throw std::invalid_argument(
            "deterministic MC manifest requires an explicit lifecycle record capacity");
    validate_mc_batch_capacity(config.n_trials, config.generator_config);
    validate_mc_effective_config(config);

    const Json effective = effective_trial_config_json(config);
    const Json generator_descriptor = Json::object({
        {"config", effective.at("generator_config")},
        {"id", config.generator_name},
        {"version", "synthetic-gbm-v1"},
    });
    const std::string descriptor_bytes = repro::serialize_canonical_json(
        generator_descriptor);
    const std::int64_t interval = mc_step_interval_ms(config.generator_config);
    const std::int64_t final_timestamp = kSyntheticFirstCloseTimeMs
        + (config.generator_config.n_steps - 1) * interval;
    const auto dataset = repro::snapshot_embedded_dataset(
        "synthetic:" + config.generator_config.symbol, 1,
        "synthetic-generator-descriptor", descriptor_bytes,
        "unix-ms:" + std::to_string(kSyntheticFirstCloseTimeMs) + ":UTC",
        "unix-ms:" + std::to_string(final_timestamp) + ":UTC",
        "stable-generated-index-order",
        "bar-only-engine-input-with-observational-l2-sidecar-v1",
        "0ms");

    const std::string fee_id = config.fee_model.empty()
        ? "zero" : config.fee_model;
    const std::string queue_id = config.maker_queue_model.empty()
        ? "none" : config.maker_queue_model;
    Json::Object strategy_config;
    strategy_config.emplace("effective_parameters",
                            effective_strategy_parameters(config));
    strategy_config.emplace("explicit_overrides",
                            explicit_strategy_parameters(config));

    const Json deterministic_inputs = Json::object({
        {"build", repro::build_identity_json(repro::current_build_identity())},
        {"dataset", repro::dataset_identity_json(dataset)},
        {"determinism_envelope", Json::object({
            {"cross_compiler_bit_identity", false},
            {"cross_cpu_bit_identity", false},
            {"event_log_binary_encoding",
             "little-endian-ieee754-binary64-v3"},
            {"floating_point_reduction", kMonteCarloFloatingPointReduction},
            {"prng", "xoshiro256starstar-v1"},
            {"sampling", "truetest-uniform-box-muller-v1"},
            {"scope", "same-source-build-toolchain-libc-stdlib-architecture"},
        })},
        {"effective_config", effective},
        {"fee_schedule", Json::object({
            {"fee_model", fee_id},
            {"fixed_fee", config.fee_value},
            {"funding", Json::object({{"enabled", false}, {"version", "disabled-v1"}})},
            {"maker_rate", config.maker_rate},
            {"snapshot_version", "mc-fee-snapshot-v1"},
            {"taker_rate", config.taker_rate},
        })},
        {"instrument", Json::object({
            {"asset_type", "synthetic-spot"},
            {"lot_size", config.instrument.lot_size},
            {"maker_rate", config.instrument.maker_rate},
            {"minimum_quantity", config.instrument.min_qty},
            {"minimum_notional", config.instrument.min_notional},
            {"minimum_validation_policy",
             "qty-epsilon-1e-12-notional-epsilon-1e-9-v1"},
            {"rounding_policy",
             "price-nearest-tick-quantity-floor-lot-v1"},
            {"symbol", config.instrument.symbol},
            {"taker_rate", config.instrument.taker_rate},
            {"tick_size", config.instrument.tick_size},
            {"venue", "synthetic"},
        })},
        {"models", Json::object({
            {"fee", versioned_model(fee_id, "fee-model-v1", {
                {"fixed_fee", config.fee_value},
                {"maker_rate", config.maker_rate},
                {"taker_rate", config.taker_rate},
            })},
            {"fill", versioned_model(
                config.fill_probability > 0.0 ? "realistic" : "local-book",
                "local-book-fill-v1", {
                    {"decay", config.fill_decay},
                    {"fade", config.fill_fade},
                    {"probability", config.fill_probability},
                })},
            {"impact", versioned_model(
                config.impact_k_bps > 0.0 ? "square-root" : "zero",
                "impact-model-v1", {
                    {"adv", config.impact_adv},
                    {"k_bps", config.impact_k_bps},
                    {"walked_book", config.walked_book_impact},
                })},
            {"latency", versioned_model(
                config.order_latency_stddev_us > 0.0
                    ? "deterministic-stochastic"
                    : config.order_latency_us > 0.0 ? "fixed" : "zero",
                "latency-model-v1", {
                    {"mean_microseconds", config.order_latency_us},
                    {"stddev_microseconds", config.order_latency_stddev_us},
                    {"wire_microseconds", config.wire_latency_us},
                })},
            {"queue", versioned_model(queue_id, "maker-queue-v1")},
            {"simulator", versioned_model(
                "truetest-event-engine", "simulation-engine-v1", {
                    {"event_ordering", "stable-generated-index-order"},
                    {"tie_breaking", "bar-only-stable-generated-index-v1"},
                })},
            {"synthetic_gbm", versioned_model(
                config.generator_name, "synthetic-gbm-v1",
                effective.at("generator_config").as_object())},
            {"synthetic_l2", versioned_model(
                config.generator_config.emit_synthetic_l2 ? "stylized-l2" : "disabled",
                "synthetic-l2-v1", {
                    {"affects_economic_execution", false},
                    {"artifact_schema", "synthetic-l2-jsonl-v1"},
                    {"base_spread_bps", config.generator_config.base_spread_bps},
                    {"consumption", config.generator_config.emit_synthetic_l2
                        ? "observational-artifact-only-not-consumed-by-engine"
                        : "disabled"},
                    {"depth_noise", config.generator_config.depth_noise},
                    {"enabled", config.generator_config.emit_synthetic_l2},
                })},
        })},
        {"monte_carlo", Json::object({
            {"lifecycle_record_capacity", static_cast<std::uint64_t>(
                config.lifecycle_record_capacity)},
            {"lifecycle_persistence", "full"},
            {"separate_trial_replay", true},
            {"trial_count", static_cast<std::uint64_t>(config.n_trials)},
        })},
        {"run_mode", "monte_carlo"},
        {"seed_hierarchy", seed_hierarchy(config)},
        {"strategy", versioned_model(
            config.strategy_name, "strategy-source-contract-v1",
            std::move(strategy_config))},
        {"target", "engine_backtest"},
        {"threading", Json::object({
            {"campaign_coordinator_threads",
                static_cast<std::uint64_t>(config.parallel_trials ? 1U : 0U)},
            {"completion_order_affects_results", false},
            {"logging_workers_per_active_trial", std::uint64_t{1}},
            {"max_active_trials", static_cast<std::uint64_t>(
                config.parallel_trials ? config.max_parallel_threads : 1U)},
            {"max_process_threads", static_cast<std::uint64_t>(
                (config.parallel_trials ? 1U : 0U)
                + 2U * (config.parallel_trials
                    ? config.max_parallel_threads : 1U))},
            {"parallel_trials", config.parallel_trials},
            {"reuse_objects_between_trials", config.reuse_objects_between_trials},
            {"trial_engine_preset", "logging"},
            {"trial_engine_threads", std::uint64_t{2}},
            {"trial_workers", static_cast<std::uint64_t>(
                config.parallel_trials ? config.max_parallel_threads : 1U)},
        })},
    });

    return repro::RunManifestV1(
        deterministic_inputs, repro::dataset_locations_json(dataset), true);
}

McRunConfig mc_config_from_manifest(
    const repro::RunManifestV1& manifest,
    const std::filesystem::path& artifacts_directory)
{
    const auto& inputs = manifest.deterministic_inputs();
    if (string_field(inputs, "target") != "engine_backtest"
        || string_field(inputs, "run_mode") != "monte_carlo")
        throw std::invalid_argument(
            "run manifest is not a deterministic engine_backtest Monte Carlo run");
    const auto& models = inputs.at("models");
    validate_supported_model(models, "simulator", "truetest-event-engine",
                             "simulation-engine-v1");
    validate_supported_model(models, "synthetic_gbm", "gbm",
                             "synthetic-gbm-v1");
    const auto& synthetic_l2_model = models.at("synthetic_l2");
    const std::string synthetic_l2_id = string_field(
        synthetic_l2_model, "id");
    if ((synthetic_l2_id != "disabled" && synthetic_l2_id != "stylized-l2")
        || string_field(synthetic_l2_model, "version") != "synthetic-l2-v1")
        throw std::invalid_argument(
            "deterministic MC manifest uses an unsupported synthetic-L2 model");
    const auto& fill_model = models.at("fill");
    const std::string fill_id = string_field(fill_model, "id");
    if ((fill_id != "local-book" && fill_id != "realistic")
        || string_field(fill_model, "version") != "local-book-fill-v1")
        throw std::invalid_argument(
            "unsupported deterministic model/version for fill");

    McRunConfig config;
    const auto& mc = inputs.at("monte_carlo");
    if (string_field(mc, "lifecycle_persistence") != "full"
        || !bool_field(mc, "separate_trial_replay"))
        throw std::invalid_argument(
            "deterministic replay requires full per-trial lifecycle persistence");
    config.n_trials = static_cast<std::size_t>(mc.at("trial_count").as_u64());

    const auto& effective = inputs.at("effective_config");
    config.generator_name = string_field(effective, "generator");
    const auto& generator = effective.at("generator_config");
    config.generator_config.symbol = string_field(generator, "symbol");
    config.generator_config.initial_price = finite_field(generator, "initial_price");
    config.generator_config.n_steps = generator.at("n_steps").as_i64();
    config.generator_config.dt = finite_field(generator, "dt");
    config.generator_config.mu = finite_field(generator, "mu");
    config.generator_config.sigma = finite_field(generator, "sigma");
    config.generator_config.emit_synthetic_l2 = bool_field(
        generator, "emit_synthetic_l2");
    config.generator_config.base_spread_bps = finite_field(
        generator, "base_spread_bps");
    config.generator_config.depth_noise = finite_field(generator, "depth_noise");
    config.initial_balance = finite_field(effective, "initial_balance");
    config.risk_fraction = finite_field(effective, "risk_fraction");
    config.sma_period = static_cast<std::size_t>(
        effective.at("sma_period").as_u64());
    const auto& engine = effective.at("engine");
    const auto& analytics = engine.at("analytics");
    config.max_equity_points = static_cast<std::size_t>(
        analytics.at("max_equity_points").as_u64());
    config.periods_per_year = static_cast<std::size_t>(
        analytics.at("periods_per_year").as_u64());
    config.risk_free_rate = finite_field(analytics, "risk_free_rate");
    config.rolling_window = static_cast<std::size_t>(
        analytics.at("rolling_window").as_u64());
    const auto& execution = engine.at("execution");
    config.execution_bar_delay = static_cast<std::size_t>(
        execution.at("execution_bar_delay").as_u64());
    config.market_aggression = finite_field(
        execution, "market_aggression");
    config.quantity_scale = finite_field(execution, "quantity_scale");
    config.wire_latency_us = finite_field(execution, "wire_latency_us");
    const auto& exits = engine.at("exit_defaults");
    const auto exit_mode = truetest::exits::parse_exit_policy_mode(
        string_field(exits, "mode"));
    if (!exit_mode)
        throw std::invalid_argument(
            "unsupported deterministic exit-policy mode");
    config.exit_defaults.mode = *exit_mode;
    config.exit_defaults.sl_pct = finite_field(exits, "sl_pct");
    config.exit_defaults.tp_pct = finite_field(exits, "tp_pct");
    config.exit_defaults.trail_pct = finite_field(exits, "trail_pct");
    const auto& market_maker = engine.at("market_maker");
    config.mm_base_depth = static_cast<int>(
        market_maker.at("base_depth").as_i64());
    config.mm_spread_pct = finite_field(
        market_maker, "base_spread_pct");
    config.mm_levels_per_side = static_cast<int>(
        market_maker.at("levels_per_side").as_i64());
    config.mm_max_half_spread_pct = finite_field(
        market_maker, "max_half_spread_pct");
    config.mm_vol_spread_mult = finite_field(
        market_maker, "volatility_spread_multiplier");
    const auto& risk = engine.at("risk");
    config.risk.daily_reset_hour = static_cast<int>(
        risk.at("daily_reset_hour").as_i64());
    config.risk.max_daily_loss = finite_field(risk, "max_daily_loss");
    config.risk.max_drawdown = finite_field(risk, "max_drawdown");
    config.risk.max_funding_8h_rate = finite_field(
        risk, "max_funding_8h_rate");
    config.risk.max_gross_leverage = finite_field(
        risk, "max_gross_leverage");
    config.risk.max_loss_per_trade = finite_field(
        risk, "max_loss_per_trade");
    config.risk.max_mark_age_ms = risk.at("max_mark_age_ms").as_i64();
    config.risk.max_open_orders = static_cast<int>(
        risk.at("max_open_orders").as_i64());
    config.risk.max_orders_per_minute = static_cast<int>(
        risk.at("max_orders_per_minute").as_i64());
    config.risk.max_portfolio_exposure = finite_field(
        risk, "max_portfolio_exposure");
    config.risk.max_position_pct_of_equity = finite_field(
        risk, "max_position_pct_of_equity");
    config.risk.max_position_value = finite_field(
        risk, "max_position_value");
    config.risk.max_spread_bps = finite_field(risk, "max_spread_bps");
    config.risk.max_symbol_inventory_qty = finite_field(
        risk, "max_symbol_inventory_qty");
    config.risk.max_trades_per_hour = static_cast<int>(
        risk.at("max_trades_per_hour").as_i64());
    config.risk.require_fresh_mark = bool_field(risk, "require_fresh_mark");
    config.risk_soft_portfolio_limits = bool_field(
        risk, "soft_portfolio_limits");
    config.risk_unwind = bool_field(risk, "unwind");
    config.maker_queue_model = string_field(
        effective, "maker_queue_model", true);
    const auto& model_config = effective.at("models");
    config.fee_model = string_field(model_config, "fee_model", true);
    config.fee_value = finite_field(model_config, "fee_value");
    config.bar_spread_bps = finite_field(model_config, "bar_spread_bps");
    config.maker_rate = finite_field(model_config, "maker_rate");
    config.mm_spread_pct = finite_field(model_config, "mm_spread_pct");
    config.taker_rate = finite_field(model_config, "taker_rate");
    config.impact_k_bps = finite_field(model_config, "impact_k_bps");
    config.impact_adv = finite_field(model_config, "impact_adv");
    config.fill_decay = finite_field(model_config, "fill_decay");
    config.fill_fade = finite_field(model_config, "fill_fade");
    config.fill_probability = finite_field(
        model_config, "fill_probability");
    config.order_latency_us = finite_field(model_config, "order_latency_us");
    config.order_latency_stddev_us = finite_field(
        model_config, "order_latency_stddev_us");
    config.walked_book_impact = bool_field(model_config, "walked_book_impact");
    config.strategy_name = string_field(effective, "strategy");
    for (const auto& parameter : array(
             effective.at("strategy_parameters"), "strategy_parameters"))
        config.strategy_params.emplace_back(
            string_field(parameter, "name"), finite_field(parameter, "value"));

    const auto& instrument = inputs.at("instrument");
    if (string_field(instrument, "rounding_policy")
            != "price-nearest-tick-quantity-floor-lot-v1"
        || string_field(instrument, "minimum_validation_policy")
            != "qty-epsilon-1e-12-notional-epsilon-1e-9-v1")
        throw std::invalid_argument(
            "unsupported deterministic instrument validation policy");
    config.instrument.symbol = string_field(instrument, "symbol");
    config.instrument.tick_size = finite_field(instrument, "tick_size");
    config.instrument.lot_size = finite_field(instrument, "lot_size");
    config.instrument.min_qty = finite_field(
        instrument, "minimum_quantity");
    config.instrument.min_notional = finite_field(
        instrument, "minimum_notional");
    config.instrument.maker_rate = finite_field(instrument, "maker_rate");
    config.instrument.taker_rate = finite_field(instrument, "taker_rate");

    const auto& threading = inputs.at("threading");
    if (bool_field(threading, "completion_order_affects_results"))
        throw std::invalid_argument(
            "manifest requests unsupported completion-order-dependent reduction");
    config.parallel_trials = bool_field(threading, "parallel_trials");
    config.reuse_objects_between_trials = bool_field(
        threading, "reuse_objects_between_trials");
    config.max_parallel_threads = static_cast<unsigned>(
        threading.at("trial_workers").as_u64());

    const auto& hierarchy = inputs.at("seed_hierarchy");
    config.base_seed = hierarchy.at("master_seed").as_u64();
    config.master_seed_explicitly_set = true;
    validate_seed_hierarchy(hierarchy, config.base_seed, config.n_trials);
    validate_mc_batch_capacity(config.n_trials, config.generator_config);

    config.persist_trial_lifecycle = true;
    config.artifacts_directory = artifacts_directory;
    config.run_fingerprint = manifest.run_fingerprint();
    config.lifecycle_record_capacity = static_cast<std::size_t>(
        mc.at("lifecycle_record_capacity").as_u64());

    // The effective config, model snapshots, dataset descriptor, and full
    // seed hierarchy are redundant by design. Rebuilding all deterministic
    // inputs catches an internally contradictory but freshly re-hashed file.
    const auto reconstructed = make_deterministic_mc_manifest(config);
    if (repro::serialize_canonical_json(reconstructed.deterministic_inputs())
        != repro::serialize_canonical_json(inputs))
    {
        throw std::invalid_argument(
            "run manifest contains contradictory deterministic inputs at "
            + first_mismatch_path(
                inputs, reconstructed.deterministic_inputs(), "$"));
    }
    return config;
}

ManifestEnvironmentVerification verify_mc_manifest_environment(
    const repro::RunManifestV1& manifest,
    const std::filesystem::path& manifest_directory)
{
    ManifestEnvironmentVerification result;
    const auto& inputs = manifest.deterministic_inputs();
    const auto expected_build = repro::build_identity_from_json(
        inputs.at("build"));
    if (expected_build != repro::current_build_identity())
    {
        result.build_exact = false;
        result.mismatches.emplace_back(
            "build identity differs from run manifest");
    }

    const auto dataset = repro::dataset_snapshot_from_json(
        inputs.at("dataset"), manifest.dataset_locations());
    const auto dataset_verification = repro::verify_dataset(
        dataset, manifest_directory);
    result.dataset_exact = dataset_verification.exact;
    result.mismatches.insert(result.mismatches.end(),
        dataset_verification.mismatches.begin(),
        dataset_verification.mismatches.end());
    result.exact = result.mismatches.empty();
    return result;
}

} // namespace truetest::simulation
