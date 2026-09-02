#include "engine/deterministic_lifecycle_sink.h"
#include "reproducibility/run_manifest.h"
#include "reproducibility/sha256.h"
#include "simulation/deterministic_trial_artifacts.h"
#include "simulation/deterministic_mc_manifest.h"
#include "simulation/monte_carlo_controller.h"
#include "simulation/monte_carlo_aggregate.h"
#include "helpers/alloc_counter.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace sim = truetest::simulation;
namespace repro = truetest::reproducibility;

namespace {

class temporary_artifacts final
{
public:
    explicit temporary_artifacts(std::string_view label)
    {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path()
            / ("truetest-r6-" + std::string(label) + "-"
               + std::to_string(::getpid()) + "-"
               + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~temporary_artifacts()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

sim::McRunConfig deterministic_config(
    const std::filesystem::path& artifacts, std::size_t trials = 2)
{
    sim::McRunConfig config;
    config.n_trials = trials;
    config.generator_config.n_steps = 48;
    config.generator_config.sigma = 0.4;
    config.base_seed = 0x123456789abcdef0ULL;
    config.master_seed_explicitly_set = true;
    // This fixture intentionally exercises a successful campaign. Strategies
    // that currently trip the independent canonical-fill safety gate belong
    // to the pre-existing accounting defect surface, not to an R6 golden.
    config.strategy_name = "ma-crossover";
    config.persist_trial_lifecycle = true;
    config.artifacts_directory = artifacts;
    config.run_fingerprint = repro::sha256_hex("deterministic-fixture-v1");
    config.lifecycle_record_capacity = 32'768;
    return config;
}

TEST(DeterministicMcConfig, UnknownFeeModelCannotBecomeZeroFeeSilently)
{
    temporary_artifacts artifacts("unknown-fee");
    auto config = deterministic_config(artifacts.path(), 1);
    config.fee_model = "mystery";
    EXPECT_THROW((void)sim::make_deterministic_mc_manifest(config),
                 std::invalid_argument);
    EXPECT_THROW((void)sim::MonteCarloController(config),
                 std::invalid_argument);
}

TEST(DeterministicTrialArtifacts,
     ObservationalSyntheticL2DoesNotEnterEngineOrEconomicHashes)
{
    temporary_artifacts baseline_artifacts("l2-baseline");
    auto baseline_config = deterministic_config(baseline_artifacts.path(), 1);
    baseline_config.generator_config.n_steps = 24;
    const auto baseline = sim::MonteCarloController(baseline_config).run_trial(0);

    temporary_artifacts l2_artifacts("l2-observational");
    auto l2_config = baseline_config;
    l2_config.artifacts_directory = l2_artifacts.path();
    l2_config.generator_config.emit_synthetic_l2 = true;
    const auto with_l2 = sim::MonteCarloController(l2_config).run_trial(0);

    EXPECT_EQ(with_l2.seed_used, baseline.seed_used);
    EXPECT_EQ(with_l2.event_log_sha256, baseline.event_log_sha256);
    EXPECT_EQ(with_l2.lifecycle_sha256, baseline.lifecycle_sha256);
    EXPECT_EQ(with_l2.trial_result_sha256, baseline.trial_result_sha256);
    EXPECT_TRUE(repro::is_lower_hex_sha256(with_l2.synthetic_l2_sha256));
    EXPECT_TRUE(baseline.synthetic_l2_sha256.empty());

    sim::McAggregate baseline_aggregate;
    baseline_aggregate.trials.push_back(baseline);
    sim::summarize_monte_carlo_trials(baseline_aggregate);
    sim::McAggregate l2_aggregate;
    l2_aggregate.trials.push_back(with_l2);
    sim::summarize_monte_carlo_trials(l2_aggregate);
    EXPECT_EQ(sim::economic_result_sha256(l2_aggregate),
              sim::economic_result_sha256(baseline_aggregate));
}

struct campaign_hashes
{
    std::string events;
    std::string economic;
    std::string report;
};

campaign_hashes hash_campaign(const sim::McAggregate& aggregate,
                              const sim::McRunConfig& config)
{
    return {
        sim::aggregate_event_log_sha256(aggregate),
        sim::economic_result_sha256(aggregate),
        sim::deterministic_report_sha256(aggregate, config),
    };
}

void add_complete_expected_hash_fixture(
    repro::RunManifestV1& manifest, std::size_t trial_count)
{
    for (const std::string_view kind : {
             "event_log", "economic_result", "report"})
        manifest.set_expected_hash(
            std::string(kind), repro::sha256_hex(kind));
    for (std::size_t trial = 0; trial < trial_count; ++trial)
        for (const std::string_view kind : {
                 "event_log", "lifecycle", "result"})
            manifest.set_expected_hash(
                "trial_" + std::to_string(trial) + "." + std::string(kind),
                repro::sha256_hex(
                    std::to_string(trial) + "." + std::string(kind)));
}

} // namespace

TEST(DeterministicLifecycleSink, HotCallbacksStayAllocationFreeAndOverflowLoud)
{
    EXPECT_LE(DeterministicLifecycleSink::record_size_bytes(), 912U);
    DeterministicLifecycleSink sink(2);
    truetest::test::alloc::snapshot allocations;
    {
        truetest::test::alloc::measure_window measured;
        sink.record_event("intent_create", "BTCUSDT", "sma", 1,
                          "info", "created", "{}");
        sink.record_event("submit", "BTCUSDT", "sma", 1,
                          "info", "submitted", "{}");
        allocations = measured.total();
    }
    EXPECT_EQ(allocations.count, 0U);
    EXPECT_EQ(allocations.bytes, 0U);
    EXPECT_FALSE(sink.overflowed());
    EXPECT_EQ(sink.record_count(), 2U);
    const std::string first = sink.canonical_json_lines();
    EXPECT_EQ(first, sink.canonical_json_lines());
    EXPECT_NE(first.find("\"kind\":\"intent_create\""), std::string::npos);

    sink.record_event("fill", "BTCUSDT", "sma", 1,
                      "info", "filled", "{}");
    EXPECT_TRUE(sink.overflowed());
    EXPECT_THROW((void)sink.canonical_json_lines(), std::runtime_error);
}

TEST(DeterministicLifecycleSink, TypedExitLifecycleIsAllocationFreeAndComplete)
{
    const auto decision = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(1'704'067'200'123'456'789LL));
    DeterministicLifecycleSink sink(1);
    truetest::test::alloc::snapshot allocations;
    {
        truetest::test::alloc::measure_window measured;
        sink.record_exit_lifecycle(exit_lifecycle_record{
            18'446'744'073'709'551'000ULL,
            18'446'744'073'709'551'001ULL,
            18'446'744'073'709'551'002ULL,
            18'446'744'073'709'551'003ULL,
            decision, decision + std::chrono::nanoseconds(1),
            decision + std::chrono::nanoseconds(2),
            decision + std::chrono::nanoseconds(3),
            9.876543210123e150, 4.938271605061e150,
            4.938271605062e150, order_exit_reason::trailing_stop,
            order_status::partially_filled, order_status::filled,
            "BTCUSDT", "ma-crossover", "risk-approved", "terminal"});
        allocations = measured.total();
    }
    EXPECT_EQ(allocations.count, 0U);
    EXPECT_EQ(allocations.bytes, 0U);
    EXPECT_EQ(sink.record_count(), 1U);
    EXPECT_FALSE(sink.overflowed());

    const std::string lifecycle = sink.canonical_json_lines();
    for (const std::string_view expected : {
             "\"kind\":\"exit_lifecycle\"", "\"signal_id\":",
             "\"decision_ts_ns\":1704067200123456789",
             "\"submit_ts_ns\":1704067200123456790",
             "\"eligible_ts_ns\":1704067200123456791",
             "\"fill_ts_ns\":1704067200123456792",
             "\"requested_qty\":", "\"filled_qty\":",
             "\"remaining_qty\":", "\"exit_reason\":3",
             "\"state_before\":\"partial\"",
             "\"state_after\":\"filled\"",
             "\"risk_outcome\":\"risk-approved\"",
             "\"phase\":\"terminal\""})
        EXPECT_NE(lifecycle.find(expected), std::string::npos) << expected;
}

TEST(DeterministicLifecycleSink, CapturesCompleteOrderLifecycleVocabulary)
{
    const auto timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1'704'067'200'000LL));
    order_event order(timestamp, "BTCUSDT", order_type::limit,
                      order_side::buy, 2.0, 65'000.0);
    order.set_order_id(77);
    order.set_strategy_name("ma-crossover");
    order_event rejected_order(timestamp, "BTCUSDT", order_type::limit,
                               order_side::buy, 3.0, 64'000.0);
    rejected_order.set_order_id(88);
    rejected_order.set_strategy_name("ma-crossover");
    fill_event partial(timestamp, "BTCUSDT", 77, order_side::buy,
                       1.0, 65'000.0, 0.1, 1.0, 9001,
                       "ma-crossover");
    fill_event complete(timestamp, "BTCUSDT", 77, order_side::buy,
                        1.0, 65'001.0, 0.1, 0.0, 9002,
                        "ma-crossover");

    DeterministicLifecycleSink sink(16);
    truetest::test::alloc::snapshot allocations;
    {
        truetest::test::alloc::measure_window measured;
        sink.record_order_submitted(order, "pending");
        sink.record_status_transition(77, order_status::pending,
                                      order_status::open,
                                      "acknowledgement/working");
        sink.record_fill(partial, 77, "ma-crossover", "simulated");
        sink.record_fill(complete, 77, "ma-crossover", "simulated");
        sink.record_cancellation(77, "BTCUSDT", "ma-crossover",
                                 "operator");
        sink.record_status_transition(77, order_status::open,
                                      order_status::cancelled,
                                      "operator acknowledged");
        sink.record_order_submitted(rejected_order, "rejected");
        sink.record_rejection(rejected_order, "risk", "test reject");
        sink.record_status_transition(78, order_status::open,
                                      order_status::expired, "tif");
        sink.record_status_transition(79, order_status::unknown,
                                      order_status::unknown,
                                      "unclassified venue state");
        allocations = measured.total();
    }
    EXPECT_EQ(allocations.count, 0U);
    EXPECT_EQ(allocations.bytes, 0U);
    EXPECT_FALSE(sink.overflowed());

    const std::string lifecycle = sink.canonical_json_lines();
    for (const std::string_view required : {
             "\"kind\":\"intent_create\"",
             "\"kind\":\"submit\"",
             "\"new_status\":\"open\"",
             "acknowledgement/working",
             "\"kind\":\"partial_fill\"",
             "\"kind\":\"fill\"",
             "\"kind\":\"cancel_request\"",
             "\"kind\":\"cancelled\"",
             "\"kind\":\"reject\"",
             "\"new_status\":\"expired\"",
             "\"new_status\":\"unknown\""})
        EXPECT_NE(lifecycle.find(required), std::string::npos) << required;

    std::size_t cursor = 0;
    while (cursor < lifecycle.size())
    {
        const auto end = lifecycle.find('\n', cursor);
        const std::string_view line(lifecycle.data() + cursor,
            (end == std::string::npos ? lifecycle.size() : end) - cursor);
        if (line.find("\"order_id\":88") != std::string_view::npos)
        {
            EXPECT_EQ(line.find("\"kind\":\"submit\""),
                      std::string_view::npos);
        }
        if (line.find("\"order_id\":78") != std::string_view::npos)
        {
            EXPECT_EQ(line.find("\"kind\":\"cancelled\""),
                      std::string_view::npos);
        }
        if (end == std::string::npos)
            break;
        cursor = end + 1U;
    }
}

TEST(DeterministicTrialArtifacts, CompletionOrderDoesNotAffectHashes)
{
    sim::TrialResult first;
    first.trial_id = 0;
    first.seed_used = 10;
    first.initial_equity = 1000.0;
    first.final_equity = 1010.0;
    first.total_pnl = 10.0;
    first.accounting_reconciled = true;
    first.event_log_sha256 = repro::sha256_hex("events-0");
    first.lifecycle_sha256 = repro::sha256_hex("lifecycle-0");
    first.trial_result_sha256 = sim::trial_result_sha256(first);

    sim::TrialResult second = first;
    second.trial_id = 1;
    second.seed_used = 11;
    second.final_equity = 990.0;
    second.total_pnl = -10.0;
    second.event_log_sha256 = repro::sha256_hex("events-1");
    second.lifecycle_sha256 = repro::sha256_hex("lifecycle-1");
    second.trial_result_sha256 = sim::trial_result_sha256(second);

    sim::McAggregate ordered;
    ordered.trials = {first, second};
    sim::summarize_monte_carlo_trials(ordered);
    sim::McAggregate reversed;
    reversed.trials = {second, first};
    sim::summarize_monte_carlo_trials(reversed);

    sim::McRunConfig config;
    config.run_fingerprint = repro::sha256_hex("run");
    EXPECT_EQ(sim::economic_result_sha256(ordered),
              sim::economic_result_sha256(reversed));
    EXPECT_EQ(sim::aggregate_event_log_sha256(ordered),
              sim::aggregate_event_log_sha256(reversed));
    EXPECT_EQ(sim::deterministic_report_sha256(ordered, config),
              sim::deterministic_report_sha256(reversed, config));
}

TEST(DeterministicTrialArtifacts, EveryEconomicAggregateFieldAffectsHashes)
{
    sim::TrialResult trial;
    trial.trial_id = 0;
    trial.seed_used = 10;
    trial.initial_equity = 1'000.0;
    trial.final_equity = 1'010.0;
    trial.total_pnl = 10.0;
    trial.accounting_reconciled = true;
    trial.event_log_sha256 = repro::sha256_hex("events");
    trial.lifecycle_sha256 = repro::sha256_hex("lifecycle");
    trial.trial_result_sha256 = sim::trial_result_sha256(trial);

    sim::McAggregate baseline;
    baseline.trials = {trial};
    sim::summarize_monte_carlo_trials(baseline);
    sim::McRunConfig config;
    config.run_fingerprint = repro::sha256_hex("run");
    const std::string economic = sim::economic_result_sha256(baseline);
    const std::string report = sim::deterministic_report_sha256(
        baseline, config);
    const auto hash_for_schema = [&](std::uint64_t version) {
        return repro::sha256_hex(repro::serialize_canonical_json(
            repro::CanonicalJsonValue::object({
                {"economic_result", sim::economic_result_payload(baseline)},
                {"economic_result_schema_version", version},
            })));
    };
    EXPECT_EQ(economic, hash_for_schema(sim::kEconomicResultSchemaVersion));
    EXPECT_NE(economic,
              hash_for_schema(sim::kEconomicResultSchemaVersion + 1U));
    auto expect_changed = [&](auto mutate) {
        auto changed = baseline;
        mutate(changed);
        EXPECT_NE(sim::economic_result_sha256(changed), economic);
        EXPECT_NE(sim::deterministic_report_sha256(changed, config), report);
    };

    expect_changed([](sim::McAggregate& value) {
        value.profit_factor_mean += 1.0;
    });
    expect_changed([](sim::McAggregate& value) {
        value.median_profit_factor += 1.0;
    });
    expect_changed([](sim::McAggregate& value) {
        value.profit_factor_mean_valid += 1.0;
    });
    expect_changed([](sim::McAggregate& value) {
        ++value.trials_with_profit_factor_gt_1;
    });
    expect_changed([](sim::McAggregate& value) {
        ++value.unbounded_profit_factor_trials;
    });
}

TEST(DeterministicTrialArtifacts, TrialResultHashCommitsToSchemaEnvelope)
{
    sim::TrialResult result;
    result.trial_id = 7;
    result.seed_used = 11;
    result.initial_equity = 1'000.0;
    result.final_equity = 1'010.0;
    result.total_pnl = 10.0;
    result.accounting_reconciled = true;
    const auto payload = sim::trial_result_payload(result);
    const auto hash_for_schema = [&](std::uint64_t version) {
        return repro::sha256_hex(repro::serialize_canonical_json(
            repro::CanonicalJsonValue::object({
                {"result", payload},
                {"trial_result_schema_version", version},
            })));
    };

    EXPECT_EQ(sim::trial_result_sha256(result),
              hash_for_schema(sim::kTrialResultSchemaVersion));
    EXPECT_NE(sim::trial_result_sha256(result),
              hash_for_schema(sim::kTrialResultSchemaVersion + 1U));
}

TEST(DeterministicTrialArtifacts, FiveRunsProduceIdenticalHashes)
{
    std::optional<campaign_hashes> expected;
    for (int run = 0; run < 5; ++run)
    {
        temporary_artifacts artifacts("five-run");
        auto config = deterministic_config(artifacts.path());
        sim::MonteCarloController controller(config);
        const auto aggregate = controller.run();
        const auto actual = hash_campaign(aggregate, config);
        if (!expected)
            expected = actual;
        else
        {
            EXPECT_EQ(actual.events, expected->events);
            EXPECT_EQ(actual.economic, expected->economic);
            EXPECT_EQ(actual.report, expected->report);
        }
        for (std::size_t trial = 0; trial < config.n_trials; ++trial)
        {
            const auto paths = sim::trial_artifact_paths(config, trial);
            EXPECT_TRUE(std::filesystem::is_regular_file(paths.event_log));
            EXPECT_TRUE(std::filesystem::is_regular_file(paths.lifecycle));
            EXPECT_TRUE(std::filesystem::is_regular_file(paths.result));
            EXPECT_TRUE(std::filesystem::is_regular_file(paths.manifest));
            EXPECT_FALSE(std::filesystem::exists(paths.event_log_partial));
        }
    }
}

TEST(DeterministicTrialArtifacts, PerTrialReplayMatchesCampaignTrial)
{
    temporary_artifacts campaign_artifacts("campaign");
    auto campaign_config = deterministic_config(campaign_artifacts.path(), 3);
    sim::MonteCarloController campaign_controller(campaign_config);
    const auto campaign = campaign_controller.run();

    temporary_artifacts replay_artifacts("trial-replay");
    auto replay_config = campaign_config;
    replay_config.artifacts_directory = replay_artifacts.path();
    sim::MonteCarloController replay_controller(replay_config);
    const auto replayed = replay_controller.run_trial(1);

    ASSERT_EQ(campaign.trials.size(), 3U);
    EXPECT_EQ(replayed.seed_used, campaign.trials[1].seed_used);
    EXPECT_EQ(replayed.event_log_sha256,
              campaign.trials[1].event_log_sha256);
    EXPECT_EQ(replayed.lifecycle_sha256,
              campaign.trials[1].lifecycle_sha256);
    EXPECT_EQ(replayed.trial_result_sha256,
              campaign.trials[1].trial_result_sha256);
}

TEST(DeterministicTrialArtifacts, ParallelWorkerCountDoesNotAffectResults)
{
    temporary_artifacts serial_artifacts("serial");
    auto serial_config = deterministic_config(serial_artifacts.path(), 3);
    sim::MonteCarloController serial_controller(serial_config);
    const auto serial = serial_controller.run();

    temporary_artifacts parallel_artifacts("parallel");
    auto parallel_config = serial_config;
    parallel_config.artifacts_directory = parallel_artifacts.path();
    parallel_config.parallel_trials = true;
    parallel_config.max_parallel_threads = 2;
    sim::MonteCarloController parallel_controller(parallel_config);
    const auto parallel = parallel_controller.run();

    const auto serial_hashes = hash_campaign(serial, serial_config);
    const auto parallel_hashes = hash_campaign(parallel, parallel_config);
    EXPECT_EQ(serial_hashes.events, parallel_hashes.events);
    EXPECT_EQ(serial_hashes.economic, parallel_hashes.economic);
    // Thread count is a recorded input, so compare the economic/report body
    // only after restoring the same effective deterministic config.
    EXPECT_EQ(sim::deterministic_report_sha256(parallel, serial_config),
              serial_hashes.report);
}

TEST(DeterministicTrialArtifacts, ForcedParallelCompletionOrderDoesNotAffectResults)
{
    temporary_artifacts serial_artifacts("forced-order-serial");
    auto serial_config = deterministic_config(serial_artifacts.path(), 3);
    sim::MonteCarloController serial_controller(serial_config);
    const auto serial = serial_controller.run();

    temporary_artifacts parallel_artifacts("forced-order-parallel");
    auto parallel_config = serial_config;
    parallel_config.artifacts_directory = parallel_artifacts.path();

    constexpr std::array<std::size_t, 3> forced_order{2, 0, 1};
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t next_position = 0;
    std::vector<std::size_t> observed_order;
    std::vector<sim::TrialResult> completion_order_results;
    std::array<std::exception_ptr, 3> errors{};
    std::vector<std::jthread> workers;
    workers.reserve(forced_order.size());

    for (std::size_t trial = 0; trial < forced_order.size(); ++trial)
    {
        workers.emplace_back([&, trial] {
            std::optional<sim::TrialResult> result;
            try
            {
                sim::MonteCarloController controller(parallel_config);
                result = controller.run_trial(trial);
            }
            catch (...)
            {
                errors[trial] = std::current_exception();
            }

            std::unique_lock lock(mutex);
            ready.wait(lock, [&] {
                return forced_order[next_position] == trial;
            });
            observed_order.push_back(trial);
            if (result)
                completion_order_results.push_back(std::move(*result));
            ++next_position;
            lock.unlock();
            ready.notify_all();
        });
    }
    workers.clear();
    for (const auto& error : errors)
        if (error) std::rethrow_exception(error);

    EXPECT_EQ(observed_order,
              (std::vector<std::size_t>{2, 0, 1}));
    sim::McAggregate permuted;
    permuted.trials = std::move(completion_order_results);
    sim::summarize_monte_carlo_trials(permuted);
    const auto serial_hashes = hash_campaign(serial, serial_config);
    const auto permuted_hashes = hash_campaign(permuted, parallel_config);
    EXPECT_EQ(permuted_hashes.events, serial_hashes.events);
    EXPECT_EQ(permuted_hashes.economic, serial_hashes.economic);
    EXPECT_EQ(permuted_hashes.report, serial_hashes.report);
}

TEST(DeterministicTrialArtifacts, FreshAndReusedObjectsProduceSameTrialHashes)
{
    temporary_artifacts fresh_artifacts("fresh");
    auto fresh_config = deterministic_config(fresh_artifacts.path(), 3);
    sim::MonteCarloController fresh_controller(fresh_config);
    const auto fresh = fresh_controller.run();

    temporary_artifacts reused_artifacts("reuse");
    auto reused_config = fresh_config;
    reused_config.artifacts_directory = reused_artifacts.path();
    reused_config.reuse_objects_between_trials = true;
    sim::MonteCarloController reused_controller(reused_config);
    const auto reused = reused_controller.run();

    ASSERT_EQ(fresh.trials.size(), reused.trials.size());
    for (std::size_t trial = 0; trial < fresh.trials.size(); ++trial)
    {
        EXPECT_EQ(fresh.trials[trial].event_log_sha256,
                  reused.trials[trial].event_log_sha256);
        EXPECT_EQ(fresh.trials[trial].lifecycle_sha256,
                  reused.trials[trial].lifecycle_sha256);
        EXPECT_EQ(fresh.trials[trial].trial_result_sha256,
                  reused.trials[trial].trial_result_sha256);
    }
}

TEST(DeterministicTrialArtifacts, FailedTrialIsExplicitAndNeverOverwrites)
{
    temporary_artifacts artifacts("failed");
    auto config = deterministic_config(artifacts.path(), 1);
    sim::write_failed_trial_artifact(config, 0, "fixture failure");
    const auto paths = sim::trial_artifact_paths(config, 0);
    const auto parsed = repro::parse_json_strict(
        repro::read_text_file(paths.manifest));
    EXPECT_EQ(parsed.at("status").as_string(), "failed");
    EXPECT_EQ(parsed.at("error").as_string(), "fixture failure");
    EXPECT_THROW(sim::prepare_trial_artifacts(paths), std::runtime_error);
}

TEST(DeterministicMcManifest, RoundTripReconstructsCompleteEffectiveConfig)
{
    temporary_artifacts artifacts("manifest-roundtrip");
    auto config = deterministic_config(artifacts.path(), 3);
    config.strategy_params.emplace_back("fast_period", 17.0);
    config.initial_balance = 12'345.0;
    config.risk_fraction = 0.015;
    config.sma_period = 37;
    config.rolling_window = 91;
    config.risk_free_rate = 0.0175;
    config.periods_per_year = 365;
    config.max_equity_points = 8'192;
    config.execution_bar_delay = 3;
    config.market_aggression = 1.25;
    config.quantity_scale = 1'000'000.0;
    config.exit_defaults.mode =
        truetest::exits::exit_policy_mode::engine_only;
    config.exit_defaults.sl_pct = 0.031;
    config.exit_defaults.tp_pct = 0.077;
    config.risk.max_position_value = 7'500.0;
    config.risk.max_drawdown = 0.23;
    config.risk.max_open_orders = 9;
    config.risk.max_orders_per_minute = 17;
    config.risk_unwind = true;
    config.risk_soft_portfolio_limits = false;
    config.instrument.tick_size = 0.25;
    config.instrument.lot_size = 0.01;
    config.instrument.min_qty = 0.02;
    config.instrument.min_notional = 10.0;
    config.instrument.maker_rate = 0.0002;
    config.instrument.taker_rate = 0.0006;
    config.fee_model = "tiered";
    config.maker_rate = config.instrument.maker_rate;
    config.taker_rate = config.instrument.taker_rate;
    config.order_latency_us = 45.0;
    config.order_latency_stddev_us = 7.5;
    config.wire_latency_us = 0.0;
    config.impact_k_bps = 0.8;
    config.impact_adv = 500'000.0;
    config.fill_probability = 0.73;
    config.fill_fade = 0.11;
    config.fill_decay = 6.5;
    config.mm_spread_pct = 0.003;
    config.mm_levels_per_side = 7;
    config.mm_base_depth = 250;
    config.mm_vol_spread_mult = 0.4;
    config.mm_max_half_spread_pct = 0.025;
    auto manifest = sim::make_deterministic_mc_manifest(config);
    add_complete_expected_hash_fixture(manifest, config.n_trials);
    const auto parsed = repro::RunManifestV1::parse(manifest.serialize());
    const auto verification = sim::verify_mc_manifest_environment(
        parsed, artifacts.path());
    EXPECT_TRUE(verification.exact);

    temporary_artifacts replay("manifest-replay");
    const auto reconstructed = sim::mc_config_from_manifest(
        parsed, replay.path());
    EXPECT_EQ(reconstructed.base_seed, config.base_seed);
    EXPECT_EQ(reconstructed.n_trials, config.n_trials);
    EXPECT_EQ(reconstructed.generator_config.n_steps,
              config.generator_config.n_steps);
    EXPECT_EQ(reconstructed.strategy_params, config.strategy_params);
    EXPECT_DOUBLE_EQ(reconstructed.initial_balance, config.initial_balance);
    EXPECT_EQ(reconstructed.sma_period, config.sma_period);
    EXPECT_EQ(reconstructed.rolling_window, config.rolling_window);
    EXPECT_EQ(reconstructed.execution_bar_delay,
              config.execution_bar_delay);
    EXPECT_DOUBLE_EQ(reconstructed.market_aggression,
                     config.market_aggression);
    EXPECT_DOUBLE_EQ(reconstructed.quantity_scale,
                     config.quantity_scale);
    EXPECT_EQ(reconstructed.exit_defaults.mode, config.exit_defaults.mode);
    EXPECT_DOUBLE_EQ(reconstructed.risk.max_position_value,
                     config.risk.max_position_value);
    EXPECT_EQ(reconstructed.risk.max_open_orders,
              config.risk.max_open_orders);
    EXPECT_DOUBLE_EQ(reconstructed.instrument.tick_size,
                     config.instrument.tick_size);
    EXPECT_DOUBLE_EQ(reconstructed.instrument.min_notional,
                     config.instrument.min_notional);
    EXPECT_DOUBLE_EQ(reconstructed.order_latency_stddev_us,
                     config.order_latency_stddev_us);
    EXPECT_DOUBLE_EQ(reconstructed.fill_probability,
                     config.fill_probability);
    EXPECT_EQ(reconstructed.mm_levels_per_side,
              config.mm_levels_per_side);
    EXPECT_EQ(reconstructed.run_fingerprint, manifest.run_fingerprint());
    EXPECT_TRUE(reconstructed.persist_trial_lifecycle);
    EXPECT_EQ(parsed.deterministic_inputs().at("determinism_envelope")
                  .at("floating_point_reduction").as_string(),
              sim::kMonteCarloFloatingPointReduction);
    const auto& threading = parsed.deterministic_inputs().at("threading");
    EXPECT_EQ(threading.at("trial_engine_preset").as_string(), "logging");
    EXPECT_EQ(threading.at("trial_engine_threads").as_u64(), 2U);
    EXPECT_EQ(threading.at("logging_workers_per_active_trial").as_u64(), 1U);
    EXPECT_EQ(threading.at("max_process_threads").as_u64(), 2U);
    const auto& engine_threading = parsed.deterministic_inputs()
        .at("effective_config").at("engine").at("threading");
    EXPECT_EQ(engine_threading.at("preset").as_string(), "logging");
    EXPECT_TRUE(engine_threading.at("disable_pinning").as_bool());
    EXPECT_FALSE(engine_threading.at("show_progress").as_bool());
    EXPECT_EQ(engine_threading.at("ring_buffer_capacity").as_u64(),
              65'536U);
    EXPECT_EQ(engine_threading.at("max_consecutive_worker_errors").as_u64(),
              5U);
    EXPECT_EQ(engine_threading.at("pin_event_loop").as_i64(), -1);
    EXPECT_EQ(engine_threading.at("pin_logging").as_i64(), -1);
    EXPECT_EQ(engine_threading.at("pin_market_maker").as_i64(), -1);
    EXPECT_EQ(engine_threading.at("pin_risk").as_i64(), -1);
    EXPECT_EQ(engine_threading.at("pin_stats").as_i64(), -1);
}

TEST(DeterministicMcManifest, PlatformStrategyDefaultsAreEffectiveInputs)
{
    temporary_artifacts artifacts("strategy-platform-defaults");
    auto config = deterministic_config(artifacts.path(), 1);
    config.strategy_name = "mean-reversion";
    config.sma_period = 7;
    config.exit_defaults.sl_pct = 0.021;
    config.exit_defaults.tp_pct = 0.087;

    const auto manifest = sim::make_deterministic_mc_manifest(config);
    const auto& inputs = manifest.deterministic_inputs();
    const auto& effective = inputs.at("strategy").at("config")
        .at("effective_parameters");
    EXPECT_DOUBLE_EQ(effective.at("period").as_double(), 7.0);
    EXPECT_DOUBLE_EQ(effective.at("sl_pct").as_double(), 0.021);
    EXPECT_DOUBLE_EQ(effective.at("tp_pct").as_double(), 0.087);
    EXPECT_EQ(inputs.at("effective_config").at("sma_period").as_u64(),
              7U);
}

TEST(DeterministicMcManifest, UnknownStrategyFailsAsConfigurationError)
{
    temporary_artifacts artifacts("unknown-strategy");
    auto config = deterministic_config(artifacts.path(), 1);
    config.strategy_name = "not-a-registered-strategy";
    try
    {
        (void)sim::make_deterministic_mc_manifest(config);
        FAIL() << "unknown deterministic strategy was accepted";
    }
    catch (const std::invalid_argument& error)
    {
        EXPECT_NE(std::string_view(error.what()).find("unknown strategy"),
                  std::string_view::npos);
    }
}

TEST(DeterministicMcConfig, DormantExecutionKnobsFailClosed)
{
    temporary_artifacts artifacts("dormant-knobs");
    const auto baseline = deterministic_config(artifacts.path(), 1);

    auto wire = baseline;
    wire.wire_latency_us = 12.0;
    EXPECT_THROW((void)sim::make_deterministic_mc_manifest(wire),
                 std::invalid_argument);

    auto incomplete_latency = baseline;
    incomplete_latency.order_latency_stddev_us = 4.0;
    EXPECT_THROW((void)sim::make_deterministic_mc_manifest(incomplete_latency),
                 std::invalid_argument);

    auto walked_book = baseline;
    walked_book.walked_book_impact = true;
    EXPECT_THROW((void)sim::make_deterministic_mc_manifest(walked_book),
                 std::invalid_argument);

    auto dormant_l2 = baseline;
    dormant_l2.generator_config.depth_noise = 0.25;
    EXPECT_THROW((void)sim::make_deterministic_mc_manifest(dormant_l2),
                 std::invalid_argument);
}

TEST(DeterministicMcManifest, ContradictorySeedHierarchyFailsClosed)
{
    temporary_artifacts artifacts("manifest-seed-negative");
    auto config = deterministic_config(artifacts.path(), 2);
    const auto valid = sim::make_deterministic_mc_manifest(config);
    auto inputs = valid.deterministic_inputs();
    inputs["seed_hierarchy"]["trials"].as_array()[1]["seed"] = 123U;
    const repro::RunManifestV1 contradictory(
        std::move(inputs), valid.dataset_locations(), true);
    EXPECT_THROW((void)sim::mc_config_from_manifest(
                     contradictory, artifacts.path() / "replay"),
                 std::invalid_argument);
}

TEST(DeterministicMcManifest, ContradictoryDatasetSchemaFailsClosed)
{
    temporary_artifacts artifacts("manifest-dataset-schema-negative");
    auto config = deterministic_config(artifacts.path(), 1);
    const auto valid = sim::make_deterministic_mc_manifest(config);
    auto inputs = valid.deterministic_inputs();
    inputs["dataset"]["schema_version"] = 2U;
    const repro::RunManifestV1 contradictory(
        std::move(inputs), valid.dataset_locations(), true);
    EXPECT_THROW((void)sim::mc_config_from_manifest(
                     contradictory, artifacts.path() / "replay"),
                 std::invalid_argument);
}

TEST(DeterministicMcManifest, ContradictoryEffectiveThreadingFailsClosed)
{
    temporary_artifacts artifacts("manifest-threading-negative");
    auto config = deterministic_config(artifacts.path(), 1);
    const auto valid = sim::make_deterministic_mc_manifest(config);
    auto inputs = valid.deterministic_inputs();
    inputs["effective_config"]["engine"]["threading"]
        ["ring_buffer_capacity"] = 1U;
    const repro::RunManifestV1 contradictory(
        std::move(inputs), valid.dataset_locations(), true);
    EXPECT_THROW((void)sim::mc_config_from_manifest(
                     contradictory, artifacts.path() / "replay"),
                 std::invalid_argument);
}

TEST(DeterministicMcManifest, BuildMismatchIsReportedBeforeReplay)
{
    temporary_artifacts artifacts("manifest-build-negative");
    auto config = deterministic_config(artifacts.path(), 1);
    const auto valid = sim::make_deterministic_mc_manifest(config);
    auto inputs = valid.deterministic_inputs();
    inputs["build"]["compiler"] = "different-compiler";
    const repro::RunManifestV1 foreign(
        std::move(inputs), valid.dataset_locations(), true);
    const auto verification = sim::verify_mc_manifest_environment(
        foreign, artifacts.path());
    EXPECT_FALSE(verification.exact);
    ASSERT_FALSE(verification.mismatches.empty());
    EXPECT_NE(verification.mismatches.front().find("build identity"),
              std::string::npos);
}

TEST(DeterministicMcManifest, ParallelModeRequiresExplicitWorkerCount)
{
    temporary_artifacts artifacts("manifest-workers-negative");
    auto config = deterministic_config(artifacts.path(), 2);
    config.parallel_trials = true;
    config.max_parallel_threads = 0;
    EXPECT_THROW((void)sim::make_deterministic_mc_manifest(config),
                 std::invalid_argument);
}

TEST(DeterministicMcManifest, ExactModeRequiresFullLifecyclePersistence)
{
    temporary_artifacts artifacts("manifest-lifecycle-negative");
    auto config = deterministic_config(artifacts.path(), 1);
    config.persist_trial_lifecycle = false;
    EXPECT_THROW((void)sim::make_deterministic_mc_manifest(config),
                 std::invalid_argument);
}
