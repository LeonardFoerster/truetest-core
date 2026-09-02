#include "deterministic_trial_artifacts.h"

#include "engine/engine_config.h"
#include "engine/event_log_contract.h"
#include "reproducibility/deterministic_seed.h"
#include "reproducibility/run_manifest.h"
#include "reproducibility/sha256.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace truetest::simulation {

namespace repro = truetest::reproducibility;

namespace {

repro::CanonicalJsonValue finite_value(double value, const char* field)
{
    if (!std::isfinite(value))
        throw std::invalid_argument(
            std::string("non-finite deterministic trial field: ") + field);
    return value;
}

std::string trial_directory_name(std::size_t trial_index)
{
    std::array<char, 48> buffer{};
    const int written = std::snprintf(
        buffer.data(), buffer.size(), "trial_%06llu",
        static_cast<unsigned long long>(trial_index));
    if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size())
        throw std::overflow_error("trial index cannot be formatted");
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

repro::CanonicalJsonValue strategy_params_json(const McRunConfig& config)
{
    repro::CanonicalJsonValue::Array values;
    values.reserve(config.strategy_params.size());
    for (const auto& [name, value] : config.strategy_params)
    {
        values.push_back(repro::CanonicalJsonValue::object({
            {"name", name},
            {"value", finite_value(value, "strategy parameter")},
        }));
    }
    return values;
}

repro::CanonicalJsonValue effective_engine_config_json(
    const McRunConfig& config)
{
    const engine_config defaults;
    const risk_limits& risk = config.risk;
    const pool_prewarm_settings& pools = defaults.pool_prewarm;
    return repro::CanonicalJsonValue::object({
        {"analytics", repro::CanonicalJsonValue::object({
            {"max_equity_points", static_cast<std::uint64_t>(
                config.max_equity_points)},
            {"periods_per_year", static_cast<std::uint64_t>(
                config.periods_per_year)},
            {"risk_free_rate", config.risk_free_rate},
            {"rolling_window", static_cast<std::uint64_t>(
                config.rolling_window)},
        })},
        {"event_log", repro::CanonicalJsonValue::object({
            {"compression", config.persist_trial_lifecycle
                ? "zstd" : "disabled"},
            {"file_version", static_cast<std::uint64_t>(
                truetest::engine_support::event_log_file_version)},
            {"max_bytes", std::uint64_t{0}},
            {"max_files", std::uint64_t{1}},
        })},
        {"execution", repro::CanonicalJsonValue::object({
            {"execution_bar_delay", static_cast<std::uint64_t>(
                config.execution_bar_delay)},
            {"fill_seed_source", "trial-fill-domain-v1"},
            {"market_aggression", config.market_aggression},
            {"quantity_scale", config.quantity_scale},
            {"spread_step_factor", defaults.spread_step_factor},
            {"wire_latency_us", config.wire_latency_us},
        })},
        {"exit_defaults", repro::CanonicalJsonValue::object({
            {"mode", truetest::exits::exit_policy_mode_name(
                config.exit_defaults.mode)},
            {"sl_pct", config.exit_defaults.sl_pct},
            {"tp_pct", config.exit_defaults.tp_pct},
            {"trail_pct", config.exit_defaults.trail_pct},
        })},
        {"market_maker", repro::CanonicalJsonValue::object({
            {"base_depth", config.mm_base_depth},
            {"base_spread_pct", config.mm_spread_pct},
            {"levels_per_side", config.mm_levels_per_side},
            {"max_half_spread_pct", config.mm_max_half_spread_pct},
            {"volatility_spread_multiplier", config.mm_vol_spread_mult},
        })},
        {"mode", "backtest"},
        {"pool_prewarm", repro::CanonicalJsonValue::object({
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
        {"risk", repro::CanonicalJsonValue::object({
            {"daily_reset_hour", risk.daily_reset_hour},
            {"max_daily_loss", risk.max_daily_loss},
            {"max_drawdown", risk.max_drawdown},
            {"max_funding_8h_rate", risk.max_funding_8h_rate},
            {"max_gross_leverage", risk.max_gross_leverage},
            {"max_loss_per_trade", risk.max_loss_per_trade},
            {"max_mark_age_ms", risk.max_mark_age_ms},
            {"max_open_orders", risk.max_open_orders},
            {"max_orders_per_minute", risk.max_orders_per_minute},
            {"max_portfolio_exposure", risk.max_portfolio_exposure},
            {"max_position_pct_of_equity", risk.max_position_pct_of_equity},
            {"max_position_value", risk.max_position_value},
            {"max_spread_bps", risk.max_spread_bps},
            {"max_symbol_inventory_qty", risk.max_symbol_inventory_qty},
            {"max_trades_per_hour", risk.max_trades_per_hour},
            {"require_fresh_mark", risk.require_fresh_mark},
            {"soft_portfolio_limits", config.risk_soft_portfolio_limits},
            {"unwind", config.risk_unwind},
        })},
        {"threading", repro::CanonicalJsonValue::object({
            {"disable_pinning", config.persist_trial_lifecycle
                ? true : defaults.disable_pinning},
            {"drop_policy", "block"},
            {"max_consecutive_worker_errors", static_cast<std::uint64_t>(
                defaults.max_consecutive_worker_errors)},
            {"pin_event_loop", defaults.pin_event_loop},
            {"pin_logging", defaults.pin_logging},
            {"pin_market_maker", defaults.pin_mm},
            {"pin_risk", defaults.pin_risk},
            {"pin_stats", defaults.pin_stats},
            {"preset", config.persist_trial_lifecycle
                ? std::string("logging") : std::string("inline")},
            {"ring_buffer_capacity", static_cast<std::uint64_t>(
                defaults.ring_buffer_capacity)},
            {"show_progress", false},
            {"spin_policy", "adaptive"},
        })},
    });
}

repro::CanonicalJsonValue artifact_hashes_json(const TrialResult& result)
{
    const auto synthetic_l2 = result.synthetic_l2_sha256.empty()
        ? repro::CanonicalJsonValue(nullptr)
        : repro::CanonicalJsonValue(result.synthetic_l2_sha256);
    return repro::CanonicalJsonValue::object({
        {"event_log_sha256", result.event_log_sha256},
        {"lifecycle_sha256", result.lifecycle_sha256},
        {"synthetic_l2_sha256", synthetic_l2},
        {"trial_result_sha256", result.trial_result_sha256},
    });
}

std::vector<const TrialResult*> stable_trials(const McAggregate& aggregate)
{
    std::vector<const TrialResult*> ordered;
    ordered.reserve(aggregate.trials.size());
    for (const auto& trial : aggregate.trials)
        ordered.push_back(&trial);
    std::sort(ordered.begin(), ordered.end(),
              [](const TrialResult* lhs, const TrialResult* rhs) {
                  return lhs->trial_id < rhs->trial_id;
              });
    for (std::size_t index = 1; index < ordered.size(); ++index)
        if (ordered[index - 1]->trial_id == ordered[index]->trial_id)
            throw std::invalid_argument(
                "deterministic report contains duplicate trial indices");
    return ordered;
}

repro::CanonicalJsonValue aggregate_metrics_json(const McAggregate& aggregate)
{
    return repro::CanonicalJsonValue::object({
        {"mean_max_drawdown", finite_value(aggregate.mean_max_dd, "mean_max_dd")},
        {"mean_pnl", finite_value(aggregate.mean_pnl, "mean_pnl")},
        {"mean_sharpe", finite_value(aggregate.mean_sharpe, "mean_sharpe")},
        {"median_pnl", finite_value(aggregate.median_pnl, "median_pnl")},
        {"median_profit_factor", finite_value(
            aggregate.median_profit_factor, "median_profit_factor")},
        {"median_profit_factor_valid", finite_value(
            aggregate.median_profit_factor_valid,
            "median_profit_factor_valid")},
        {"median_sharpe", finite_value(aggregate.median_sharpe, "median_sharpe")},
        {"median_win_rate", finite_value(aggregate.median_win_rate, "median_win_rate")},
        {"p5_pnl", finite_value(aggregate.p5_pnl, "p5_pnl")},
        {"p95_pnl", finite_value(aggregate.p95_pnl, "p95_pnl")},
        {"profit_factor_pooled", finite_value(
            aggregate.profit_factor_pooled, "profit_factor_pooled")},
        {"profit_factor_pooled_unbounded",
         aggregate.profit_factor_pooled_unbounded},
        {"profit_factor_mean", finite_value(
            aggregate.profit_factor_mean, "profit_factor_mean")},
        {"profit_factor_mean_valid", finite_value(
            aggregate.profit_factor_mean_valid,
            "profit_factor_mean_valid")},
        {"trial_count", static_cast<std::uint64_t>(aggregate.n_trials)},
        {"trials_profitable", static_cast<std::uint64_t>(
            aggregate.trials_with_positive_pnl)},
        {"trials_with_profit_factor_gt_1", static_cast<std::uint64_t>(
            aggregate.trials_with_profit_factor_gt_1)},
        {"unbounded_profit_factor_trials", static_cast<std::uint64_t>(
            aggregate.unbounded_profit_factor_trials)},
        {"valid_profit_factor_trials", static_cast<std::uint64_t>(
            aggregate.valid_profit_factor_trials)},
        {"valid_sharpe_trials", static_cast<std::uint64_t>(
            aggregate.valid_sharpe_trials)},
        {"win_rate_mean", finite_value(aggregate.win_rate_mean, "win_rate_mean")},
        {"worst_max_drawdown", finite_value(
            aggregate.worst_max_dd, "worst_max_dd")},
    });
}

} // namespace

TrialArtifactPaths trial_artifact_paths(const McRunConfig& config,
                                        std::size_t trial_index)
{
    if (config.artifacts_directory.empty())
        throw std::invalid_argument(
            "deterministic trial persistence requires an artifacts directory");
    const std::filesystem::path directory = config.artifacts_directory
        / "trials" / trial_directory_name(trial_index);
    TrialArtifactPaths result;
    result.directory = directory;
    result.event_log = directory / "events.zst";
    result.event_log_partial = directory / "events.zst.partial";
    result.lifecycle = directory / "lifecycle.jsonl";
    result.synthetic_l2 = directory / "synthetic_l2.jsonl";
    result.synthetic_l2_partial = directory / "synthetic_l2.jsonl.partial";
    result.result = directory / "result.json";
    result.manifest = directory / "trial_manifest.json";
    return result;
}

void prepare_trial_artifacts(const TrialArtifactPaths& paths)
{
    const std::array targets{
        paths.event_log, paths.event_log_partial, paths.lifecycle,
        std::filesystem::path(paths.lifecycle.string() + ".partial"),
        paths.synthetic_l2, paths.synthetic_l2_partial,
        paths.result, std::filesystem::path(paths.result.string() + ".partial"),
        paths.manifest,
        std::filesystem::path(paths.manifest.string() + ".partial")};
    std::error_code error;
    for (const auto& target : targets)
    {
        if (std::filesystem::exists(target, error))
            throw std::runtime_error(
                "refusing to overwrite or resume trial artifact: "
                + target.string());
        if (error)
            throw std::runtime_error(
                "cannot inspect trial artifact path: " + target.string());
    }
    std::filesystem::create_directories(paths.directory, error);
    if (error)
        throw std::runtime_error(
            "cannot create trial artifact directory: "
            + paths.directory.string());
}

void write_trial_synthetic_l2_partial(
    const TrialArtifactPaths& paths,
    const std::vector<provider::l2_snapshot>& snapshots)
{
    if (snapshots.empty())
        throw std::invalid_argument(
            "enabled synthetic L2 persistence requires snapshots");
    std::ofstream output(paths.synthetic_l2_partial,
                         std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error(
            "cannot create synthetic L2 trial staging artifact: "
            + paths.synthetic_l2_partial.string());

    const auto levels_json = [](const auto& levels, std::string_view side) {
        repro::CanonicalJsonValue::Array values;
        values.reserve(levels.size());
        for (const auto& level : levels)
        {
            if (!(level.price > 0.0) || !std::isfinite(level.price)
                || level.quantity <= 0)
                throw std::invalid_argument(
                    "synthetic L2 contains an invalid " + std::string(side)
                    + " level");
            values.push_back(repro::CanonicalJsonValue::object({
                {"price", level.price},
                {"quantity", level.quantity},
            }));
        }
        return values;
    };

    std::optional<std::int64_t> previous_timestamp_ns;
    for (const auto& snapshot : snapshots)
    {
        if (snapshot.symbol.empty() || snapshot.bids.empty()
            || snapshot.asks.empty() || snapshot.quantity_scale == 0)
            throw std::invalid_argument(
                "synthetic L2 snapshot is incomplete");
        const auto timestamp_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                snapshot.timestamp.time_since_epoch()).count();
        if (previous_timestamp_ns && timestamp_ns <= *previous_timestamp_ns)
            throw std::invalid_argument(
                "synthetic L2 snapshots are not strictly time ordered");
        previous_timestamp_ns = timestamp_ns;
        const auto line = repro::CanonicalJsonValue::object({
            {"asks", levels_json(snapshot.asks, "ask")},
            {"bids", levels_json(snapshot.bids, "bid")},
            {"last_update_id", snapshot.last_update_id},
            {"quantity_scale", snapshot.quantity_scale},
            {"snapshot_schema_version", std::uint64_t{1}},
            {"symbol", snapshot.symbol},
            {"timestamp_unix_ns", timestamp_ns},
        });
        output << repro::serialize_canonical_json(line) << '\n';
        if (!output)
            throw std::runtime_error(
                "cannot write synthetic L2 trial staging artifact");
    }
    output.close();
    if (!output)
        throw std::runtime_error(
            "cannot finalize synthetic L2 trial staging artifact");
}

void finalize_trial_synthetic_l2(const TrialArtifactPaths& paths)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(paths.synthetic_l2_partial, error)
        || error)
        throw std::runtime_error(
            "synthetic L2 trial artifact was not staged");
    if (std::filesystem::exists(paths.synthetic_l2, error))
        throw std::runtime_error(
            "refusing to overwrite synthetic L2 trial artifact");
    if (error)
        throw std::runtime_error(
            "cannot inspect synthetic L2 trial artifact destination");
    std::filesystem::rename(
        paths.synthetic_l2_partial, paths.synthetic_l2, error);
    if (error)
        throw std::runtime_error(
            "cannot atomically publish synthetic L2 trial artifact: "
            + error.message());
}

void finalize_trial_event_log(const TrialArtifactPaths& paths)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(paths.event_log_partial, error)
        || error)
        throw std::runtime_error(
            "trial event log was not finalized by the engine: "
            + paths.event_log_partial.string());
    if (std::filesystem::exists(paths.event_log, error))
        throw std::runtime_error(
            "refusing to overwrite trial event log: "
            + paths.event_log.string());
    if (error)
        throw std::runtime_error(
            "cannot inspect trial event-log destination: "
            + paths.event_log.string());
    std::filesystem::rename(paths.event_log_partial, paths.event_log, error);
    if (error)
        throw std::runtime_error(
            "cannot atomically publish trial event log: " + error.message());
}

repro::CanonicalJsonValue trial_result_payload(const TrialResult& result)
{
    return repro::CanonicalJsonValue::object({
        {"accounting_reconciled", result.accounting_reconciled},
        {"final_equity", finite_value(result.final_equity, "final_equity")},
        {"initial_equity", finite_value(result.initial_equity, "initial_equity")},
        {"max_drawdown", finite_value(result.max_drawdown, "max_drawdown")},
        {"profit_factor", result.profit_factor_valid
             ? finite_value(result.profit_factor, "profit_factor")
             : repro::CanonicalJsonValue(nullptr)},
        {"profit_factor_unbounded", result.profit_factor_unbounded},
        {"profit_factor_valid", result.profit_factor_valid},
        {"seed_used", result.seed_used},
        {"sharpe_ratio", result.sharpe_ratio_valid
             ? finite_value(result.sharpe_ratio, "sharpe_ratio")
             : repro::CanonicalJsonValue(nullptr)},
        {"sharpe_ratio_valid", result.sharpe_ratio_valid},
        {"total_loss", finite_value(result.total_loss, "total_loss")},
        {"total_pnl", finite_value(result.total_pnl, "total_pnl")},
        {"total_trades", static_cast<std::uint64_t>(result.total_trades)},
        {"total_win", finite_value(result.total_win, "total_win")},
        {"trial_index", result.trial_id},
        {"win_rate", finite_value(result.win_rate, "win_rate")},
        {"winning_trades", static_cast<std::uint64_t>(result.winning_trades)},
    });
}

std::string trial_result_sha256(const TrialResult& result)
{
    return repro::sha256_hex(repro::serialize_canonical_json(
        repro::CanonicalJsonValue::object({
            {"result", trial_result_payload(result)},
            {"trial_result_schema_version", kTrialResultSchemaVersion},
        })));
}

repro::CanonicalJsonValue effective_trial_config_json(const McRunConfig& config)
{
    return repro::CanonicalJsonValue::object({
        {"generator", config.generator_name},
        {"generator_config", repro::CanonicalJsonValue::object({
            {"base_spread_bps", finite_value(
                config.generator_config.base_spread_bps, "base_spread_bps")},
            {"depth_noise", finite_value(
                config.generator_config.depth_noise, "depth_noise")},
            {"dt", finite_value(config.generator_config.dt, "dt")},
            {"emit_synthetic_l2", config.generator_config.emit_synthetic_l2},
            {"initial_price", finite_value(
                config.generator_config.initial_price, "initial_price")},
            {"mu", finite_value(config.generator_config.mu, "mu")},
            {"n_steps", config.generator_config.n_steps},
            {"sigma", finite_value(config.generator_config.sigma, "sigma")},
            {"symbol", config.generator_config.symbol},
        })},
        {"initial_balance", finite_value(config.initial_balance, "initial_balance")},
        {"engine", effective_engine_config_json(config)},
        {"maker_queue_model", config.maker_queue_model.empty()
             ? std::string("none") : config.maker_queue_model},
        {"models", repro::CanonicalJsonValue::object({
            {"bar_spread_bps", finite_value(
                config.bar_spread_bps, "bar_spread_bps")},
            {"fee_model", config.fee_model.empty()
                 ? std::string("zero") : config.fee_model},
            {"fee_value", finite_value(config.fee_value, "fee_value")},
            {"impact_adv", finite_value(config.impact_adv, "impact_adv")},
            {"impact_k_bps", finite_value(config.impact_k_bps, "impact_k_bps")},
            {"fill_decay", finite_value(config.fill_decay, "fill_decay")},
            {"fill_fade", finite_value(config.fill_fade, "fill_fade")},
            {"fill_probability", finite_value(
                config.fill_probability, "fill_probability")},
            {"maker_rate", finite_value(config.maker_rate, "maker_rate")},
            {"mm_spread_pct", finite_value(
                config.mm_spread_pct, "mm_spread_pct")},
            {"order_latency_us", finite_value(
                config.order_latency_us, "order_latency_us")},
            {"order_latency_stddev_us", finite_value(
                config.order_latency_stddev_us,
                "order_latency_stddev_us")},
            {"taker_rate", finite_value(config.taker_rate, "taker_rate")},
            {"walked_book_impact", config.walked_book_impact},
        })},
        {"risk_fraction", finite_value(config.risk_fraction, "risk_fraction")},
        {"sma_period", static_cast<std::uint64_t>(config.sma_period)},
        {"strategy", config.strategy_name},
        {"strategy_parameters", strategy_params_json(config)},
    });
}

repro::CanonicalJsonValue component_seed_json(std::uint64_t master_seed,
                                               std::size_t trial_index)
{
    const repro::DeterministicSeedDeriver deriver(master_seed);
    const auto index = static_cast<std::uint64_t>(trial_index);
    return repro::CanonicalJsonValue::object({
        {"fill_model", deriver.trial_component_seed(index, repro::SeedDomain::fill_model)},
        {"impact_model", deriver.trial_component_seed(index, repro::SeedDomain::impact_model)},
        {"latency_model", deriver.trial_component_seed(index, repro::SeedDomain::latency_model)},
        {"market_maker", deriver.trial_component_seed(index, repro::SeedDomain::market_maker)},
        {"queue_model", deriver.trial_component_seed(index, repro::SeedDomain::queue_model)},
        {"strategy", deriver.trial_component_seed(index, repro::SeedDomain::strategy)},
        {"synthetic_l2", deriver.trial_component_seed(index, repro::SeedDomain::synthetic_l2)},
        {"synthetic_price", deriver.trial_component_seed(index, repro::SeedDomain::synthetic_price)},
        {"synthetic_volume", deriver.trial_component_seed(index, repro::SeedDomain::synthetic_volume)},
    });
}

void write_completed_trial_artifacts(const McRunConfig& config,
                                     const TrialArtifactPaths& paths,
                                     const TrialResult& result)
{
    const auto payload = trial_result_payload(result);
    const auto result_hash_preimage = repro::CanonicalJsonValue::object({
        {"result", payload},
        {"trial_result_schema_version", kTrialResultSchemaVersion},
    });
    const std::string calculated_result_hash = repro::sha256_hex(
        repro::serialize_canonical_json(result_hash_preimage));
    if (calculated_result_hash != result.trial_result_sha256)
        throw std::logic_error("trial result hash changed before persistence");
    if (!repro::is_lower_hex_sha256(result.event_log_sha256)
        || !repro::is_lower_hex_sha256(result.lifecycle_sha256))
        throw std::logic_error("completed trial lacks artifact SHA-256 values");
    if (config.generator_config.emit_synthetic_l2
        != repro::is_lower_hex_sha256(result.synthetic_l2_sha256))
        throw std::logic_error(
            "completed trial synthetic L2 hash does not match its effective model state");

    repro::write_text_file_atomic(paths.result,
        repro::serialize_canonical_json(repro::CanonicalJsonValue::object({
            {"result", payload},
            {"trial_result_sha256", result.trial_result_sha256},
            {"trial_result_schema_version", kTrialResultSchemaVersion},
        })));

    const auto synthetic_l2_name = config.generator_config.emit_synthetic_l2
        ? repro::CanonicalJsonValue("synthetic_l2.jsonl")
        : repro::CanonicalJsonValue(nullptr);
    const auto synthetic_l2_hash = config.generator_config.emit_synthetic_l2
        ? repro::CanonicalJsonValue(result.synthetic_l2_sha256)
        : repro::CanonicalJsonValue(nullptr);
    const auto manifest = repro::CanonicalJsonValue::object({
        {"artifacts", repro::CanonicalJsonValue::object({
            {"event_log", "events.zst"},
            {"event_log_sha256", result.event_log_sha256},
            {"lifecycle", "lifecycle.jsonl"},
            {"lifecycle_sha256", result.lifecycle_sha256},
            {"result", "result.json"},
            {"synthetic_l2", synthetic_l2_name},
            {"synthetic_l2_sha256", synthetic_l2_hash},
            {"trial_result_sha256", result.trial_result_sha256},
        })},
        {"component_seeds", component_seed_json(config.base_seed,
                                                  result.trial_id)},
        {"effective_trial_config", effective_trial_config_json(config)},
        {"error", ""},
        {"run_fingerprint", config.run_fingerprint},
        {"status", "complete"},
        {"trial_index", result.trial_id},
        {"trial_manifest_schema_version", std::uint64_t{1}},
        {"trial_seed", result.seed_used},
    });
    repro::write_text_file_atomic(
        paths.manifest, repro::serialize_canonical_json(manifest));
}

void write_failed_trial_artifact(const McRunConfig& config,
                                 std::size_t trial_index,
                                 std::string_view error)
{
    if (!config.persist_trial_lifecycle)
        return;
    const auto paths = trial_artifact_paths(config, trial_index);
    std::error_code filesystem_error;
    std::filesystem::create_directories(paths.directory, filesystem_error);
    if (filesystem_error)
        throw std::runtime_error(
            "cannot create failed-trial artifact directory: "
            + filesystem_error.message());
    if (std::filesystem::exists(paths.manifest, filesystem_error))
        return;
    const auto deriver = repro::DeterministicSeedDeriver(config.base_seed);
    const auto manifest = repro::CanonicalJsonValue::object({
        {"artifacts", repro::CanonicalJsonValue::object({})},
        {"component_seeds", component_seed_json(config.base_seed, trial_index)},
        {"effective_trial_config", effective_trial_config_json(config)},
        {"error", error},
        {"run_fingerprint", config.run_fingerprint},
        {"status", "failed"},
        {"trial_index", static_cast<std::uint64_t>(trial_index)},
        {"trial_manifest_schema_version", std::uint64_t{1}},
        {"trial_seed", deriver.trial_seed(
            static_cast<std::uint64_t>(trial_index))},
    });
    repro::write_text_file_atomic(
        paths.manifest, repro::serialize_canonical_json(manifest));
}

repro::CanonicalJsonValue economic_result_payload(const McAggregate& aggregate)
{
    repro::CanonicalJsonValue::Array trials;
    const auto ordered = stable_trials(aggregate);
    trials.reserve(ordered.size());
    for (const TrialResult* trial : ordered)
        trials.push_back(trial_result_payload(*trial));
    return repro::CanonicalJsonValue::object({
        {"aggregate", aggregate_metrics_json(aggregate)},
        {"trials", std::move(trials)},
    });
}

std::string economic_result_sha256(const McAggregate& aggregate)
{
    return repro::sha256_hex(repro::serialize_canonical_json(
        repro::CanonicalJsonValue::object({
            {"economic_result", economic_result_payload(aggregate)},
            {"economic_result_schema_version", kEconomicResultSchemaVersion},
        })));
}

std::string aggregate_event_log_sha256(const McAggregate& aggregate)
{
    repro::CanonicalJsonValue::Array trials;
    const auto ordered = stable_trials(aggregate);
    trials.reserve(ordered.size());
    for (const TrialResult* trial : ordered)
    {
        if (!repro::is_lower_hex_sha256(trial->event_log_sha256)
            || !repro::is_lower_hex_sha256(trial->lifecycle_sha256))
            throw std::invalid_argument(
                "aggregate event-log hash requires full trial artifacts");
        trials.push_back(repro::CanonicalJsonValue::object({
            {"event_log_sha256", trial->event_log_sha256},
            {"lifecycle_sha256", trial->lifecycle_sha256},
            {"synthetic_l2_sha256", trial->synthetic_l2_sha256.empty()
                ? repro::CanonicalJsonValue(nullptr)
                : repro::CanonicalJsonValue(trial->synthetic_l2_sha256)},
            {"trial_index", trial->trial_id},
        }));
    }
    return repro::sha256_hex(repro::serialize_canonical_json(trials));
}

std::string deterministic_report_json(const McAggregate& aggregate,
                                      const McRunConfig& config)
{
    repro::CanonicalJsonValue::Array trials;
    const auto ordered = stable_trials(aggregate);
    trials.reserve(ordered.size());
    for (const TrialResult* trial : ordered)
    {
        trials.push_back(repro::CanonicalJsonValue::object({
            {"artifact_hashes", artifact_hashes_json(*trial)},
            {"economic_result", trial_result_payload(*trial)},
        }));
    }
    // wall_time_ms is intentionally absent. It belongs in run_receipt.json,
    // never in a deterministic report or economic-result hash.
    return repro::serialize_canonical_json(repro::CanonicalJsonValue::object({
        {"aggregate", aggregate_metrics_json(aggregate)},
        {"effective_config", effective_trial_config_json(config)},
        {"report_schema_version", std::uint64_t{1}},
        {"run_fingerprint", config.run_fingerprint},
        {"trials", std::move(trials)},
    }));
}

std::string deterministic_report_sha256(const McAggregate& aggregate,
                                        const McRunConfig& config)
{
    return repro::sha256_hex(deterministic_report_json(aggregate, config));
}

} // namespace truetest::simulation
