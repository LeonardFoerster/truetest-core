#include <gtest/gtest.h>
#include "reproducibility/deterministic_seed.h"
#include "reproducibility/run_manifest.h"
#include "reproducibility/sha256.h"
#include <cstdlib>
#include <cstdio>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

static bool is_executable_file(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)
        && (st.st_mode & S_IXUSR);
}

// A configured test target must exercise binaries from its own build tree.
// Falling back to another tree by mtime can silently test a different source
// fingerprint. Ordered fallbacks exist only for ad-hoc/manual test binaries.
static std::string resolve_engine_binary(const std::string& binary)
{
#ifdef TRUETEST_ENGINE_DIR
    const std::string configured =
        std::string(TRUETEST_ENGINE_DIR) + "/" + binary;
    // A configured test must fail against its own tree. Falling back after a
    // missing binary could silently execute engine_live from another build.
    return configured;
#endif

    const char* candidates[] = {
        "./out/build/linux-tests/",
        "./build/",
        "./out/build/linux-release-native/",
    };
    for (const char* dir : candidates)
    {
        const std::string path = std::string(dir) + binary;
        if (is_executable_file(path)) return path;
    }
    return std::string("./build/") + binary;
}

// Helper: run engine binary with args, capture stdout+stderr, return exit code
static int run_engine(const std::string& binary,
                      const std::string& args,
                      std::string& output)
{
    const std::string path = resolve_engine_binary(binary);
    std::string cmd = path + " " + args + " 2>&1";
    std::array<char, 4096> buf;
    output.clear();

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;

    while (fgets(buf.data(), buf.size(), pipe))
        output += buf.data();

    int status = pclose(pipe);
    // pclose returns the process exit status encoded; extract it
    if (status < 0) return -1;
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return status;
}

#ifdef TRUETEST_DETERMINISTIC_BUILD_PROFILE
static int run_engine_with_loader_preload(
    const std::string& args, std::string& output)
{
    const std::string command = std::string(TRUETEST_DYNAMIC_LOADER)
        + " --preload " + TRUETEST_PRELOAD_FIXTURE + " "
        + resolve_engine_binary("engine_backtest") + " " + args + " 2>&1";
    std::array<char, 4096> buffer;
    output.clear();
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return -1;
    while (fgets(buffer.data(), buffer.size(), pipe))
        output += buffer.data();
    const int status = pclose(pipe);
    if (status < 0) return -1;
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return status;
}
#endif

static int run_truetest(const std::string& args, std::string& output)
{
    if (args.find("--seed") != std::string::npos)
        return run_engine("engine_backtest", args, output);
    return run_engine("engine_backtest", args + " --seed 424242", output);
}

class cli_temporary_directory final
{
public:
    cli_temporary_directory()
    {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path()
            / ("truetest-r6-cli-test-" + std::to_string(::getpid()) + "-"
               + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~cli_temporary_directory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

#ifndef TRUETEST_RESEARCH_ONLY
static int run_engine_live(const std::string& args, std::string& output)
{
    return run_engine("engine_live", args, output);
}
#endif

// ─── B1: CLI11 parsing ─────────────────────────────────────────────────────

TEST(CLI, HelpExitsCleanly)
{
    std::string out;
    int rc = run_truetest("--help", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("--strategy"), std::string::npos);
    EXPECT_NE(out.find("--provider"), std::string::npos);
    EXPECT_NE(out.find("--balance"), std::string::npos);
}

TEST(CLI, UnknownFlagRejected)
{
    std::string out;
    int rc = run_truetest("--nonexistent-flag", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("not expected"), std::string::npos);
}

TEST(CLI, BacktestWithoutExplicitSeedFailsClosed)
{
    std::string out;
    const int rc = run_engine("engine_backtest",
                              "--provider synthetic --no-pin --no-tui", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("Backtest requires an explicit deterministic seed"),
              std::string::npos);
}

#ifdef TRUETEST_DETERMINISTIC_BUILD_PROFILE
TEST(CLI, DeterministicProfileRejectsLoaderArgumentPreload)
{
    std::string output;
    const int rc = run_engine_with_loader_preload(
        "--monte-carlo --mc-trials 1 --seed 1 --no-pin --no-tui "
        "--status-format off", output);
    EXPECT_NE(rc, 0) << output;
    EXPECT_NE(output.find("unexpected loaded ELF object"), std::string::npos)
        << output;
}
#endif

TEST(CLI, ExplicitZeroSeedIsDisplayedAsDeterministicValue)
{
    std::string output;
    const int rc = run_engine(
        "engine_backtest", "--dry-run --seed 0", output);
    EXPECT_EQ(rc, 0) << output;
    EXPECT_NE(output.find("Seed:       0"), std::string::npos);
    EXPECT_EQ(output.find("Seed:       random"), std::string::npos);
}

TEST(CLI, NonBacktestTargetRejectsDeterministicManifestReplayBeforeLoading)
{
    cli_temporary_directory directory;
    std::string output;
#ifdef TRUETEST_RESEARCH_ONLY
    const int rc = run_engine(
        "engine_shadow",
        "--replay-run-manifest definitely-missing.json --artifacts-dir "
            + directory.path().string(), output);
#else
    const int rc = run_engine_live(
        "--replay-run-manifest definitely-missing.json --artifacts-dir "
            + directory.path().string(), output);
#endif
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find(
        "Deterministic run-manifest replay requires the engine_backtest target"),
        std::string::npos);
    EXPECT_EQ(output.find("cannot read"), std::string::npos);
}

TEST(CLI, NonBacktestTargetRejectsDeterministicManifestGeneration)
{
    cli_temporary_directory directory;
    const auto manifest = directory.path() / "forbidden_manifest.json";
    const auto artifacts = directory.path() / "forbidden_artifacts";
    std::string output;
    const std::string arguments =
        "--monte-carlo --mc-trials 1 --seed 123 "
        "--write-run-manifest " + manifest.string()
        + " --artifacts-dir " + artifacts.string()
        + " --no-pin --no-tui --status-format off";
#ifdef TRUETEST_RESEARCH_ONLY
    const int rc = run_engine("engine_shadow", arguments, output);
#else
    const int rc = run_engine_live(arguments, output);
#endif
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find(
        "Deterministic run-manifest generation requires the engine_backtest target"),
        std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(manifest));
    EXPECT_FALSE(std::filesystem::exists(artifacts));
}

TEST(CLI, DeterministicManifestGoldenAndPerTrialReplayMatch)
{
    cli_temporary_directory directory;
    const auto manifest = directory.path() / "run_manifest.json";
    const auto generated = directory.path() / "generated";
    const auto trial = directory.path() / "trial";

    std::string output;
    const std::string common =
        " --instrument BTCUSDT:tick=0.01,lot=0.000001,minq=0.000001,"
        "minn=0,maker=0,taker=0 --fee zero"
        " --no-pin --no-tui --status-format off";
    int rc = run_engine("engine_backtest",
        "--monte-carlo --mc-trials 2 --mc-parallel --mc-workers 2 "
        "--mc-params n_steps=24,sigma=0.4 "
        "--strategy ma-crossover --param fast_period=2 "
        "--param slow_period=4 --seed 123 --write-run-manifest "
        + manifest.string() + " --artifacts-dir " + generated.string()
        + common, output);
    ASSERT_EQ(rc, 0) << output;
    EXPECT_NE(output.find("Run fingerprint:"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(manifest));
    const auto lifecycle_path = generated / "trials" / "trial_000000"
        / "lifecycle.jsonl";
    ASSERT_TRUE(std::filesystem::is_regular_file(lifecycle_path));
    std::ifstream lifecycle_input(lifecycle_path, std::ios::binary);
    const std::string lifecycle(
        (std::istreambuf_iterator<char>(lifecycle_input)), {});
    EXPECT_NE(lifecycle.find("\"kind\":\"submit\""), std::string::npos);
    EXPECT_NE(lifecycle.find("\"kind\":\"fill\""), std::string::npos);

    // Generation plus four independent replay processes proves the MC
    // five-run contract, including reconstruction of the parallel schedule.
    for (int run = 1; run <= 4; ++run)
    {
        const auto replayed = directory.path()
            / ("replayed-" + std::to_string(run));
        rc = run_engine("engine_backtest",
            "--replay-run-manifest " + manifest.string()
            + " --artifacts-dir " + replayed.string()
            + " --verify-hashes", output);
        ASSERT_EQ(rc, 0) << "replay " << run << ": " << output;
        EXPECT_NE(output.find("MATCH event_log"), std::string::npos);
        EXPECT_NE(output.find("MATCH economic_result"), std::string::npos);
        EXPECT_NE(output.find("MATCH report"), std::string::npos);
        EXPECT_NE(output.find("MATCH trial_0.event_log"), std::string::npos);
        EXPECT_NE(output.find("MATCH trial_0.lifecycle"), std::string::npos);
        EXPECT_NE(output.find("MATCH trial_0.result"), std::string::npos);
        EXPECT_NE(output.find("MATCH trial_1.event_log"), std::string::npos);
        EXPECT_NE(output.find("MATCH trial_1.lifecycle"), std::string::npos);
        EXPECT_NE(output.find("MATCH trial_1.result"), std::string::npos);
        const auto receipt = truetest::reproducibility::parse_json_strict(
            truetest::reproducibility::read_text_file(
                replayed / "run_receipt.json"));
        EXPECT_TRUE(receipt.at("exact_reproduction").as_bool());
    }

    rc = run_engine("engine_backtest",
        "--replay-run-manifest " + manifest.string()
        + " --artifacts-dir " + trial.string()
        + " --trial 1 --verify-hashes", output);
    ASSERT_EQ(rc, 0) << output;
    EXPECT_NE(output.find("MATCH trial_1.event_log"), std::string::npos);
    EXPECT_NE(output.find("MATCH trial_1.lifecycle"), std::string::npos);
    EXPECT_NE(output.find("MATCH trial_1.result"), std::string::npos);
    const auto trial_receipt = truetest::reproducibility::parse_json_strict(
        truetest::reproducibility::read_text_file(
            trial / "run_receipt.json"));
    EXPECT_EQ(trial_receipt.at("verification_scope").as_string(), "trial");
    EXPECT_EQ(trial_receipt.at("trial_index").as_u64(), 1U);
    EXPECT_EQ(trial_receipt.at("actual_hashes").as_object().size(), 3U);
    EXPECT_FALSE(std::filesystem::exists(trial / "report.json"));
}

TEST(CLI, StandaloneSyntheticManifestUsesReplayableSingleTrialEnvelope)
{
    cli_temporary_directory directory;
    const auto manifest = directory.path() / "run_manifest.json";
    std::string output;
    int rc = run_engine(
        "engine_backtest",
        "--provider synthetic --mc-params n_steps=16,sigma=0 "
        "--strategy ma-crossover --seed 321 --write-run-manifest "
            + manifest.string() + " --artifacts-dir "
            + (directory.path() / "generated").string()
            + " --no-pin --no-tui --status-format off",
        output);
    ASSERT_EQ(rc, 0) << output;
    EXPECT_NE(output.find("using one-trial Monte Carlo replay envelope"),
              std::string::npos);
    ASSERT_TRUE(std::filesystem::is_regular_file(manifest));

    rc = run_engine(
        "engine_backtest",
        "--replay-run-manifest " + manifest.string()
            + " --artifacts-dir "
            + (directory.path() / "replayed").string()
            + " --verify-hashes",
        output);
    ASSERT_EQ(rc, 0) << output;
    EXPECT_NE(output.find("MATCH event_log"), std::string::npos);
    EXPECT_NE(output.find("MATCH economic_result"), std::string::npos);
    EXPECT_NE(output.find("MATCH report"), std::string::npos);
}

TEST(CLI, StandaloneSyntheticProviderRefusesASecondSeedAuthority)
{
    std::string output;
    const int rc = run_engine(
        "engine_backtest",
        "--provider synthetic --mc-params n_steps=8,seed=999 "
        "--seed 123 --no-pin --no-tui --status-format off",
        output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find("unknown synthetic provider parameter: seed"),
              std::string::npos);
}

TEST(CLI, SyntheticL2SidecarIsVersionedHashedAndReplayVerified)
{
    cli_temporary_directory directory;
    const auto manifest_path = directory.path() / "l2-manifest.json";
    const auto generated = directory.path() / "generated";
    std::string output;
    ASSERT_EQ(run_engine(
        "engine_backtest",
        "--monte-carlo --mc-trials 1 "
        "--mc-params n_steps=8,sigma=0,emit_l2=true,spread_bps=3,depth_noise=0 "
        "--strategy ma-crossover --seed 456 --write-run-manifest "
            + manifest_path.string() + " --artifacts-dir "
            + generated.string()
            + " --no-pin --no-tui --status-format off",
        output), 0) << output;
    EXPECT_NE(output.find("deterministic trial sidecar only"),
              std::string::npos);
    const auto l2_path = generated / "trials" / "trial_000000"
        / "synthetic_l2.jsonl";
    ASSERT_TRUE(std::filesystem::is_regular_file(l2_path));
    const auto manifest = truetest::reproducibility::RunManifestV1::load(
        manifest_path);
    const auto& l2_model = manifest.deterministic_inputs()
        .at("models").at("synthetic_l2");
    EXPECT_EQ(l2_model.at("version").as_string(), "synthetic-l2-v1");
    EXPECT_EQ(l2_model.at("config").at("consumption").as_string(),
              "observational-artifact-only-not-consumed-by-engine");
    EXPECT_FALSE(l2_model.at("config")
                     .at("affects_economic_execution").as_bool());
    EXPECT_EQ(manifest.expected_hashes().at("trial_0.synthetic_l2"),
              truetest::reproducibility::sha256_file_hex(l2_path));

    const auto replayed = directory.path() / "replayed";
    const int rc = run_engine(
        "engine_backtest",
        "--replay-run-manifest " + manifest_path.string()
            + " --artifacts-dir " + replayed.string()
            + " --verify-hashes",
        output);
    ASSERT_EQ(rc, 0) << output;
    EXPECT_NE(output.find("MATCH trial_0.synthetic_l2"),
              std::string::npos);
    EXPECT_EQ(truetest::reproducibility::sha256_file_hex(
                  replayed / "trials" / "trial_000000"
                      / "synthetic_l2.jsonl"),
              manifest.expected_hashes().at("trial_0.synthetic_l2"));

    const auto single_trial = directory.path() / "single-trial";
    ASSERT_EQ(run_engine(
        "engine_backtest",
        "--replay-run-manifest " + manifest_path.string()
            + " --artifacts-dir " + single_trial.string()
            + " --trial 0 --verify-hashes",
        output), 0) << output;
    EXPECT_NE(output.find("MATCH trial_0.synthetic_l2"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(single_trial / "report.json"));

    auto wrong_hashes = manifest.expected_hashes();
    wrong_hashes["trial_0.synthetic_l2"] = std::string(64, '0');
    const truetest::reproducibility::RunManifestV1 wrong_l2_hash(
        manifest.deterministic_inputs(), manifest.dataset_locations(), true,
        std::move(wrong_hashes));
    const auto wrong_manifest = directory.path() / "wrong-l2-hash.json";
    wrong_l2_hash.write_atomic(wrong_manifest);
    const int mismatch_rc = run_engine(
        "engine_backtest",
        "--replay-run-manifest " + wrong_manifest.string()
            + " --artifacts-dir "
            + (directory.path() / "l2-mismatch").string()
            + " --verify-hashes",
        output);
    EXPECT_EQ(mismatch_rc, 4);
    EXPECT_NE(output.find("MISMATCH trial_0.synthetic_l2"),
              std::string::npos);
}

TEST(CLI, HashVerificationRejectsANonExactManifestBeforeTrialStart)
{
    cli_temporary_directory directory;
    const auto manifest_path = directory.path() / "exact.json";
    const auto non_exact_path = directory.path() / "non-exact.json";
    std::string output;
    ASSERT_EQ(run_engine(
        "engine_backtest",
        "--monte-carlo --mc-trials 1 --mc-params n_steps=8,sigma=0 "
        "--strategy ma-crossover --seed 123 --write-run-manifest "
            + manifest_path.string() + " --artifacts-dir "
            + (directory.path() / "generated").string()
            + " --no-pin --no-tui --status-format off",
        output), 0) << output;

    const auto original =
        truetest::reproducibility::RunManifestV1::load(manifest_path);
    const truetest::reproducibility::RunManifestV1 non_exact(
        original.deterministic_inputs(), original.dataset_locations(), false,
        original.expected_hashes());
    non_exact.write_atomic(non_exact_path);

    const auto replayed = directory.path() / "replayed";
    const int rc = run_engine(
        "engine_backtest",
        "--replay-run-manifest " + non_exact_path.string()
            + " --artifacts-dir " + replayed.string()
            + " --verify-hashes",
        output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find(
        "--verify-hashes requires an exact lifecycle-replayable manifest"),
        std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(replayed / "trials"));
}

TEST(CLI, DeterministicManifestMismatchAndOverridesFailClosed)
{
    cli_temporary_directory directory;
    const auto manifest = directory.path() / "run_manifest.json";
    std::string output;
    const std::string common = " --no-pin --no-tui --status-format off";
    ASSERT_EQ(run_engine("engine_backtest",
        "--monte-carlo --mc-trials 1 --mc-params n_steps=16 "
        "--strategy ma-crossover --seed 7 "
        "--write-run-manifest " + manifest.string()
        + " --artifacts-dir " + (directory.path() / "generated").string()
        + common, output), 0) << output;

    std::ifstream input(manifest, std::ios::binary);
    std::string corrupted((std::istreambuf_iterator<char>(input)), {});
    const std::string marker = "\"event_log\":\"";
    const auto marker_position = corrupted.find(marker);
    ASSERT_NE(marker_position, std::string::npos);
    corrupted.replace(marker_position + marker.size(), 64, 64, '0');
    const auto corrupted_manifest = directory.path() / "corrupt.json";
    {
        std::ofstream output_file(corrupted_manifest, std::ios::binary);
        output_file << corrupted;
    }
    int rc = run_engine("engine_backtest",
        "--replay-run-manifest " + corrupted_manifest.string()
        + " --artifacts-dir " + (directory.path() / "mismatch").string()
        + " --verify-hashes", output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find("MISMATCH event_log"), std::string::npos);

    const std::string trial_marker = "\"trial_0.lifecycle\":\"";
    const auto trial_marker_position = corrupted.find(trial_marker);
    ASSERT_NE(trial_marker_position, std::string::npos);
    std::string corrupted_trial = corrupted;
    // Restore the aggregate event hash first so this replay isolates the
    // per-trial verification contract.
    {
        std::ifstream original_input(manifest, std::ios::binary);
        const std::string original(
            (std::istreambuf_iterator<char>(original_input)), {});
        const auto aggregate_position = original.find(marker);
        ASSERT_NE(aggregate_position, std::string::npos);
        corrupted_trial.replace(
            corrupted_trial.find(marker) + marker.size(), 64,
            original.substr(aggregate_position + marker.size(), 64));
    }
    corrupted_trial.replace(
        corrupted_trial.find(trial_marker) + trial_marker.size(),
        64, 64, '0');
    const auto corrupted_trial_manifest =
        directory.path() / "corrupt-trial.json";
    {
        std::ofstream output_file(
            corrupted_trial_manifest, std::ios::binary);
        output_file << corrupted_trial;
    }
    rc = run_engine("engine_backtest",
        "--replay-run-manifest " + corrupted_trial_manifest.string()
        + " --artifacts-dir "
        + (directory.path() / "trial-mismatch").string()
        + " --verify-hashes", output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find("MISMATCH trial_0.lifecycle"),
              std::string::npos);

    rc = run_engine("engine_backtest",
        "--replay-run-manifest " + manifest.string()
        + " --artifacts-dir " + (directory.path() / "override").string()
        + " --seed 7 --verify-hashes", output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find("Manifest replay refuses explicit option"),
              std::string::npos);
}

TEST(CLI, InvalidMonteCarloParameterNeverFallsBackSilently)
{
    std::string output;
    const int rc = run_engine("engine_backtest",
        "--monte-carlo --mc-trials 1 --seed 1 "
        "--mc-params n_steps=not-a-number --no-pin --no-tui", output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find("Invalid --mc-params"), std::string::npos);
}

TEST(CLI, MonteCarloStrategyParametersRequireCanonicalFiniteNumbers)
{
    for (const std::string_view parameter : {
             "period=1junk", "=1", "period=1=2", "period=nan"})
    {
        std::string output;
        const int rc = run_engine(
            "engine_backtest",
            "--monte-carlo --mc-trials 1 --seed 1 --param "
                + std::string(parameter) + " --no-pin --no-tui",
            output);
        EXPECT_NE(rc, 0) << parameter << '\n' << output;
        EXPECT_NE(output.find("Invalid --param"), std::string::npos)
            << parameter << '\n' << output;
    }
}

TEST(CLI, DeterministicMonteCarloCapturesEffectiveExecutionAndStrategyInputs)
{
    cli_temporary_directory directory;
    const auto manifest_path = directory.path() / "effective_manifest.json";
    const auto artifacts = directory.path() / "effective_artifacts";
    std::string output;
    const int rc = run_engine(
        "engine_backtest",
        "--monte-carlo --mc-trials 1 --mc-params n_steps=8,sigma=0 "
        "--strategy mean-reversion --sma-period 7 --sl 0.021 --tp 0.087 "
        "--aggression 1.25 --qty-scale 1000000 --seed 987 "
        "--instrument BTCUSDT:tick=0.01,lot=0.000001,minq=0.000001,"
        "minn=0,maker=0,taker=0 --fee zero --write-run-manifest "
        + manifest_path.string() + " --artifacts-dir " + artifacts.string()
        + " --no-pin --no-tui --status-format off",
        output);
    ASSERT_EQ(rc, 0) << output;

    const auto manifest = truetest::reproducibility::RunManifestV1::load(
        manifest_path);
    const auto& inputs = manifest.deterministic_inputs();
    const auto& execution = inputs.at("effective_config").at("engine")
        .at("execution");
    EXPECT_DOUBLE_EQ(execution.at("market_aggression").as_double(), 1.25);
    EXPECT_DOUBLE_EQ(execution.at("quantity_scale").as_double(), 1'000'000.0);
    EXPECT_EQ(inputs.at("effective_config").at("sma_period").as_u64(), 7U);
    const auto& strategy = inputs.at("strategy").at("config")
        .at("effective_parameters");
    EXPECT_DOUBLE_EQ(strategy.at("period").as_double(), 7.0);
    EXPECT_DOUBLE_EQ(strategy.at("sl_pct").as_double(), 0.021);
    EXPECT_DOUBLE_EQ(strategy.at("tp_pct").as_double(), 0.087);
}

TEST(CLI, MonteCarloRejectsInputsItsTrialConsumerCannotHonor)
{
    const std::array<std::pair<std::string, std::string>, 21> cases{{
        {"--format tick", "requires --format bar"},
        {"--provider local --path ignored.csv", "embedded synthetic dataset"},
        {"--queue-model l2-snapshot", "currently unsupported"},
        {"--fill-rng-seed 7", "refuses --fill-rng-seed"},
        {"--spread-step 0.01", "refuses --spread-step"},
        {"--mc-workers 2", "requires --mc-parallel"},
        {"--spin-policy spin", "custom thread/spin settings are unsupported"},
        {"--wire-latency-us 5", "refuses --wire-latency-us"},
        {"--order-latency-stddev-us 5", "requires --order-latency-us"},
        {"--walked-book-impact", "requires an L2-enabled trial consumer"},
        {"--output-format csv", "supports only JSON"},
        {"--replay-from 1", "refuses --replay-from/--replay-to"},
        {"--replay-from 0", "refuses --replay-from/--replay-to"},
        {"--replay-to 2", "refuses --replay-from/--replay-to"},
        {"--replay-to 9223372036854775807",
         "refuses --replay-from/--replay-to"},
        {"--preset futures-phase0", "accepts only the mc-robustness preset"},
        {"--fill-prob 2", "fill probability/fade/decay configuration is invalid"},
        {"--fill-decay 11", "disabled fill model refuses inert calibration"},
        {"--impact-adv 100", "impact requires positive k_bps and ADV together"},
        {"--strategy sma --param period=2.5",
         "cannot represent the requested deterministic value exactly"},
        {"--strategy sma --param period=10001",
         "outside its deterministic schema range"},
    }};
    for (const auto& [argument, expected] : cases)
    {
        std::string output;
        const int rc = run_engine(
            "engine_backtest",
            "--monte-carlo --mc-trials 1 --seed 1 " + argument,
            output);
        EXPECT_NE(rc, 0) << argument << '\n' << output;
        EXPECT_NE(output.find(expected), std::string::npos)
            << argument << '\n' << output;
    }
}

TEST(CLI, MonteCarloAcceptsItsActualExploratoryInlineThreadPreset)
{
    std::string output;
    const int rc = run_engine(
        "engine_backtest",
        "--monte-carlo --mc-trials 1 --mc-params n_steps=8,sigma=0 "
        "--seed 1 --thread-preset inline --no-tui --status-format off",
        output);
    EXPECT_EQ(rc, 0) << output;
}

TEST(CLI, MonteCarloManifestGenerationRejectsReplayOnlyHashVerification)
{
    cli_temporary_directory directory;
    std::string output;
    const int rc = run_engine(
        "engine_backtest",
        "--monte-carlo --mc-trials 1 --seed 1 --write-run-manifest "
            + (directory.path() / "manifest.json").string()
            + " --artifacts-dir "
            + (directory.path() / "artifacts").string()
            + " --verify-hashes",
        output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find(
        "--verify-hashes is only valid with --replay-run-manifest"),
        std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(
        directory.path() / "manifest.json"));
}

TEST(CLI, MonteCarloManifestGenerationRejectsUnhashedLegacyOutput)
{
    cli_temporary_directory directory;
    const auto manifest = directory.path() / "manifest.json";
    const auto legacy_output = directory.path() / "legacy.json";
    std::string output;
    const int rc = run_engine(
        "engine_backtest",
        "--monte-carlo --mc-trials 1 --seed 1 --write-run-manifest "
            + manifest.string() + " --artifacts-dir "
            + (directory.path() / "artifacts").string()
            + " --output " + legacy_output.string(),
        output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find(
        "manifest mode refuses --output"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(manifest));
    EXPECT_FALSE(std::filesystem::exists(legacy_output));
}

TEST(CLI, DeterministicLocalBacktestFiveRunsAndGoldenReplayMatch)
{
    cli_temporary_directory directory;
    const auto manifest = directory.path() / "run_manifest.json";
    const auto fixture = directory.path() / "tradeful.csv";
    {
        std::ofstream dataset(fixture, std::ios::binary);
        dataset << "date,open_time,symbol,open,high,low,close,volume\n"
                   "2024-01-01,1704067200000,OBS,100,101,99,100,1000\n"
                   "2024-01-01,1704067260000,OBS,100,100,89,90,1000\n"
                   "2024-01-01,1704067320000,OBS,90,92,87,88,1000\n"
                   "2024-01-01,1704067380000,OBS,88,112,87,110,1000\n"
                   "2024-01-01,1704067440000,OBS,80,82,70,75,1000\n";
    }
    const std::string common =
        " --thread-preset logging --no-pin --no-tui --status-format off";
    const std::string instrument =
        " --symbol OBS --instrument "
        "OBS:tick=0.01,lot=0.000001,minq=0.000001,minn=0,"
        "maker=0,taker=0 --fee zero";

    std::string output;
    int rc = run_engine("engine_backtest",
        "--provider local --path " + fixture.string()
        + " --format bar --strategy ma-crossover"
          " --param fast_period=2 --param slow_period=3"
          " --exec-bar-delay 0 --seed 12345"
        + instrument
        + " --write-run-manifest " + manifest.string()
        + " --artifacts-dir " + (directory.path() / "generated").string()
        + common, output);
    ASSERT_EQ(rc, 0) << output;
    EXPECT_TRUE(std::filesystem::is_regular_file(manifest));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        directory.path() / "generated" / "events.zst"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        directory.path() / "generated" / "lifecycle.jsonl"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        directory.path() / "generated" / "report.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        directory.path() / "generated" / "dataset" / "input_000000"));
    {
        std::ifstream lifecycle_file(
            directory.path() / "generated" / "lifecycle.jsonl",
            std::ios::binary);
        const std::string lifecycle(
            (std::istreambuf_iterator<char>(lifecycle_file)), {});
        EXPECT_NE(lifecycle.find("\"kind\":\"submit\""),
                  std::string::npos);
        EXPECT_NE(lifecycle.find("\"kind\":\"fill\""),
                  std::string::npos);
    }

    // Generation plus four independent replay processes is the five-run
    // reproducibility contract. Every replay compares all deterministic
    // artifacts against the generation manifest.
    for (int run = 1; run <= 4; ++run)
    {
        const auto replay = directory.path()
            / ("replay-" + std::to_string(run));
        rc = run_engine("engine_backtest",
            "--replay-run-manifest " + manifest.string()
            + " --artifacts-dir " + replay.string()
            + " --verify-hashes", output);
        ASSERT_EQ(rc, 0) << "replay " << run << ": " << output;
        EXPECT_NE(output.find("MATCH event_log"), std::string::npos);
        EXPECT_NE(output.find("MATCH lifecycle"), std::string::npos);
        EXPECT_NE(output.find("MATCH economic_result"), std::string::npos);
        EXPECT_NE(output.find("MATCH report"), std::string::npos);
    }
}

TEST(CLI, DeterministicLocalFailurePublishesReceiptButNoFinalManifest)
{
    cli_temporary_directory directory;
    const auto dataset = directory.path() / "invalid.csv";
    {
        std::ofstream output(dataset, std::ios::binary);
        output << "date,symbol,open,high,low,close,volume\n"
                  "bad,AAPL,100,101,99,100,1\n";
    }
    const auto manifest = directory.path() / "run_manifest.json";
    const auto artifacts = directory.path() / "artifacts";
    std::string output;
    const int rc = run_engine("engine_backtest",
        "--provider local --path " + dataset.string()
        + " --format bar --strategy sma --seed 5"
          " --symbol AAPL --instrument "
          "AAPL:tick=0.01,lot=0.000001,minq=0.000001,minn=0,"
          "maker=0.001,taker=0.001 --thread-preset logging --no-pin"
          " --no-tui --status-format off --write-run-manifest "
        + manifest.string() + " --artifacts-dir " + artifacts.string(),
        output);
    EXPECT_NE(rc, 0);
    EXPECT_FALSE(std::filesystem::exists(manifest));
    ASSERT_TRUE(std::filesystem::is_regular_file(
        artifacts / "run_receipt.json"));
    std::ifstream receipt_file(artifacts / "run_receipt.json");
    const std::string receipt((std::istreambuf_iterator<char>(receipt_file)), {});
    EXPECT_NE(receipt.find("\"status\":\"failed\""), std::string::npos);
}

TEST(CLI, DeterministicLocalReplayRejectsMonteCarloTrialScope)
{
    cli_temporary_directory directory;
    const auto manifest = directory.path() / "run_manifest.json";
    const std::string common =
        " --symbol AAPL --instrument "
        "AAPL:tick=0.01,lot=0.000001,minq=0.000001,minn=0,"
        "maker=0.001,taker=0.001 --thread-preset logging --no-pin "
        "--no-tui --status-format off";
    std::string output;
    ASSERT_EQ(run_engine("engine_backtest",
        "--provider local --path tests/fixtures/sample_ohlcv.csv"
        " --format bar --strategy sma --seed 5" + common
        + " --write-run-manifest " + manifest.string()
        + " --artifacts-dir "
        + (directory.path() / "generated").string(), output), 0) << output;

    const int rc = run_engine("engine_backtest",
        "--replay-run-manifest " + manifest.string()
        + " --artifacts-dir " + (directory.path() / "replayed").string()
        + " --trial 0 --verify-hashes", output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find(
        "--trial is only valid for Monte Carlo run manifests"),
        std::string::npos);
    EXPECT_EQ(output.find("DataBridge: loaded"), std::string::npos);
}

TEST(CLI, DeterministicLocalUnknownQueueModelsFailInsteadOfBecomingNoop)
{
    for (const char* const option : {
             "--queue-model banana", "--maker-queue-model banana"})
    {
        cli_temporary_directory directory;
        const auto manifest = directory.path() / "run_manifest.json";
        std::string output;
        const int rc = run_engine("engine_backtest",
            "--provider local --path tests/fixtures/sample_ohlcv.csv"
            " --format bar --strategy sma --seed 5 --symbol AAPL"
            " --instrument AAPL:tick=0.01,lot=0.000001,minq=0.000001,"
            "minn=0,maker=0.001,taker=0.001 --thread-preset logging"
            " --no-pin --no-tui --status-format off "
            + std::string(option)
            + " --write-run-manifest " + manifest.string()
            + " --artifacts-dir "
            + (directory.path() / "artifacts").string(), output);
        EXPECT_NE(rc, 0) << option;
        EXPECT_TRUE(output.find("unsupported deterministic local")
                        != std::string::npos
                    || output.find("not in") != std::string::npos)
            << output;
        EXPECT_FALSE(std::filesystem::exists(manifest));
    }
}

TEST(CLI, DeterministicLocalRejectsInertOrContradictoryModelInputs)
{
    const std::array<std::pair<std::string, std::string>, 38> cases{{
        {"--fill-prob 2", "fill configuration is invalid"},
        {"--fill-decay 11", "disabled fill model refuses inert calibration"},
        {"--order-latency-stddev-us 5",
         "stochastic latency requires a positive mean"},
        {"--wire-latency-us 1", "wire latency is unsupported"},
        {"--impact-adv 100", "positive k_bps and ADV together"},
        {"--impact-k-bps 1", "positive k_bps and ADV together"},
        {"--walked-book-impact", "requires a hashed L2 dataset"},
        {"--fill-rng-seed 7", "refuses --fill-rng-seed"},
        {"--spread-step 0.02", "refuses --spread-step"},
        {"--debug-fills", "refuses debug"},
        {"--api-key deterministic-test", "refuses credentials"},
        {"--checkpoint-interval 9", "provider-only checkpoint"},
        {"--backfill 9", "backfill"},
        {"--backfill-interval 1m", "backfill"},
        {"--margin-type CROSSED", "venue-risk"},
        {"--liquidation-warn-pct 0.05", "venue-risk"},
        {"--margin-type-strict", "venue-risk"},
        {"--max-notional 100", "venue-risk"},
        {"--max-leverage 2", "venue-risk"},
        {"--min-liq-distance-pct 0.05", "venue-risk"},
        {"--dead-man-countdown-ms 1000", "dead-man"},
        {"--dead-man-heartbeat-ms 100", "dead-man"},
        {"--disarm-deadman", "dead-man"},
        {"--reconcile-tolerance-bps 2", "reconciliation"},
        {"--kill-switch-deadline-ms 2000", "kill-switch"},
        {"--mc-model gbm", "mode-specific Monte Carlo"},
        {"--mc-model ''", "mode-specific Monte Carlo"},
        {"--mc-params ''", "mode-specific Monte Carlo"},
        {"--mc-parallel", "mode-specific Monte Carlo"},
        {"--mc-workers 2", "mode-specific Monte Carlo"},
        {"--mc-reuse-objects", "mode-specific Monte Carlo"},
        {"--mc-trials 0", "mode-specific Monte Carlo"},
        {"--replay-from 1", "event-log replay inputs"},
        {"--replay-from 0", "event-log replay inputs"},
        {"--replay-to 2", "event-log replay inputs"},
        {"--replay-to 9223372036854775807", "event-log replay inputs"},
        {"--strategy sma --param period=2.5",
         "cannot represent the requested deterministic value exactly"},
        {"--strategy sma --param period=10001",
         "outside its deterministic schema range"},
    }};
    for (const auto& [argument, expected] : cases)
    {
        cli_temporary_directory directory;
        const auto manifest = directory.path() / "manifest.json";
        std::string output;
        const int rc = run_engine("engine_backtest",
            "--provider local --path tests/fixtures/sample_ohlcv.csv"
            " --format bar --seed 5 --symbol AAPL --instrument "
            "AAPL:tick=0.01,lot=0.000001,minq=0.000001,minn=0,"
            "maker=0.001,taker=0.001 --thread-preset logging --no-pin "
            "--no-tui --status-format off " + argument
            + " --write-run-manifest " + manifest.string()
            + " --artifacts-dir "
            + (directory.path() / "artifacts").string(), output);
        EXPECT_NE(rc, 0) << argument << '\n' << output;
        EXPECT_NE(output.find(expected), std::string::npos)
            << argument << '\n' << output;
        EXPECT_FALSE(std::filesystem::exists(manifest));
    }
}

TEST(ConfigFile, DeterministicLocalRejectsExplicitEmptyMonteCarloFields)
{
    for (const char* const field : {"mc_model", "mc_params"})
    {
        cli_temporary_directory directory;
        const auto config_path = directory.path() / "local_config.json";
        const auto manifest = directory.path() / "manifest.json";
        {
            std::ofstream config(config_path, std::ios::binary);
            config << "{\"" << field << "\":\"\"}";
        }

        std::string output;
        const int rc = run_engine(
            "engine_backtest",
            "--config " + config_path.string()
                + " --provider local"
                  " --path tests/fixtures/sample_ohlcv.csv --format bar"
                  " --strategy sma --seed 5 --symbol AAPL"
                  " --instrument AAPL:tick=0.01,lot=0.000001,"
                  "minq=0.000001,minn=0,maker=0.001,taker=0.001"
                  " --thread-preset logging --no-pin --no-tui"
                  " --status-format off --write-run-manifest "
                + manifest.string()
                + " --artifacts-dir "
                + (directory.path() / "artifacts").string(),
            output);
        EXPECT_NE(rc, 0) << field << '\n' << output;
        EXPECT_NE(output.find("mode-specific Monte Carlo"),
                  std::string::npos)
            << field << '\n' << output;
        EXPECT_FALSE(std::filesystem::exists(manifest));
    }
}

TEST(CLI, DeterministicManifestWriteCannotBeBypassedByTopLevelControlModes)
{
    cli_temporary_directory directory;
    const std::string base =
        "--provider local --path tests/fixtures/sample_ohlcv.csv"
        " --format bar --strategy sma --seed 5 --symbol AAPL"
        " --instrument AAPL:tick=0.01,lot=0.000001,minq=0.000001,"
        "minn=0,maker=0.001,taker=0.001 --thread-preset logging"
        " --no-pin --no-tui --status-format off ";
    std::string output;
    for (const std::string& option : {
             std::string("--dry-run"), std::string("--dump-config"),
             std::string("--replay tests/fixtures/sample_ohlcv.csv")})
    {
        const auto manifest = directory.path()
            / ("manifest-" + std::to_string(option.size()) + ".json");
        const int rc = run_engine(
            "engine_backtest", base + option + " --write-run-manifest "
                + manifest.string(), output);
        EXPECT_NE(rc, 0) << option << '\n' << output;
        EXPECT_NE(output.find("cannot be combined"), std::string::npos)
            << option << '\n' << output;
        EXPECT_FALSE(std::filesystem::exists(manifest));
    }

    const auto manifest = directory.path() / "config-manifest.json";
    const auto config_path = directory.path() / "bypass-config.json";
    {
        std::ofstream config(config_path, std::ios::binary);
        config << "{\"provider\":\"local\",\"path\":\"tests/fixtures/"
                  "sample_ohlcv.csv\",\"format\":\"bar\",\"strategy\":"
                  "\"sma\",\"seed\":5,\"symbol\":\"AAPL\","
                  "\"instrument\":[\"AAPL:tick=0.01,lot=0.000001,"
                  "minq=0.000001,minn=0,maker=0.001,taker=0.001\"],"
                  "\"thread_preset\":\"logging\",\"write_run_manifest\":\""
               << manifest.string() << "\"}";
    }
    const int rc = run_engine(
        "engine_backtest", "--config " + config_path.string()
            + " --dry-run", output);
    EXPECT_NE(rc, 0) << output;
    EXPECT_NE(output.find("cannot be combined"), std::string::npos)
        << output;
    EXPECT_FALSE(std::filesystem::exists(manifest));
}

TEST(CLI, ManifestReplayRejectsEveryUnlistedExplicitOption)
{
    cli_temporary_directory directory;
    const auto manifest = directory.path() / "run_manifest.json";
    std::string output;
    ASSERT_EQ(run_engine("engine_backtest",
        "--monte-carlo --mc-trials 1 --mc-params n_steps=8 "
        "--strategy ma-crossover --seed 11 "
        "--write-run-manifest " + manifest.string()
        + " --artifacts-dir " + (directory.path() / "generated").string()
        + " --no-pin --no-tui --status-format off", output), 0) << output;

    for (const std::string option : {
             "--dry-run", "--dump-config", "--format bar",
             "--sma-period 9", "--max-daily-loss 1"})
    {
        const int rc = run_engine("engine_backtest",
            "--replay-run-manifest " + manifest.string()
            + " --artifacts-dir "
            + (directory.path() / ("rejected-" + std::to_string(option.size()))).string()
            + " " + option, output);
        EXPECT_NE(rc, 0) << option;
        EXPECT_NE(output.find("Manifest replay refuses explicit option"),
                  std::string::npos) << option;
    }
}

TEST(CLI, DeterministicLocalReplayRejectsChangedDatasetBeforeEngineStart)
{
    cli_temporary_directory directory;
    const auto dataset = directory.path() / "dataset.csv";
    ASSERT_TRUE(std::filesystem::copy_file(
        "tests/fixtures/sample_ohlcv.csv", dataset));
    const auto manifest = directory.path() / "run_manifest.json";
    const std::string common =
        " --symbol AAPL --instrument "
        "AAPL:tick=0.01,lot=0.000001,minq=0.000001,minn=0,"
        "maker=0.001,taker=0.001 --thread-preset logging --no-pin "
        "--no-tui --status-format off";

    std::string output;
    ASSERT_EQ(run_engine("engine_backtest",
        "--provider local --path " + dataset.string()
        + " --format bar --strategy sma --seed 99"
        + common + " --write-run-manifest " + manifest.string()
        + " --artifacts-dir " + (directory.path() / "generated").string(),
        output), 0) << output;

    {
        std::ofstream changed(dataset, std::ios::app | std::ios::binary);
        ASSERT_TRUE(changed.is_open());
        changed << "\nchanged-after-manifest";
    }
    const int rc = run_engine("engine_backtest",
        "--replay-run-manifest " + manifest.string()
        + " --artifacts-dir " + (directory.path() / "replayed").string()
        + " --verify-hashes", output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find("dataset identity does not match manifest"),
              std::string::npos);
    EXPECT_EQ(output.find("DataBridge: loaded"), std::string::npos);
}

TEST(CLI, DatasetMismatchOverrideIsVisibleAndNeverClaimsExactReplay)
{
    cli_temporary_directory directory;
    const auto dataset = directory.path() / "dataset.csv";
    ASSERT_TRUE(std::filesystem::copy_file(
        "tests/fixtures/sample_ohlcv.csv", dataset));
    const auto manifest = directory.path() / "run_manifest.json";
    const std::string common =
        " --symbol AAPL --instrument "
        "AAPL:tick=0.01,lot=0.000001,minq=0.000001,minn=0,"
        "maker=0.001,taker=0.001 --thread-preset logging --no-pin "
        "--no-tui --status-format off";
    std::string output;
    ASSERT_EQ(run_engine("engine_backtest",
        "--provider local --path " + dataset.string()
        + " --format bar --strategy sma --seed 99" + common
        + " --write-run-manifest " + manifest.string()
        + " --artifacts-dir " + (directory.path() / "generated").string(),
        output), 0) << output;

    {
        std::fstream changed(dataset,
            std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(changed.is_open());
        const std::string bytes((std::istreambuf_iterator<char>(changed)), {});
        const auto position = bytes.find("150.0");
        ASSERT_NE(position, std::string::npos);
        changed.clear();
        changed.seekp(static_cast<std::streamoff>(position));
        changed.write("151.0", 5);
        ASSERT_TRUE(changed.good());
    }

    const auto replayed = directory.path() / "changed";
    int rc = run_engine("engine_backtest",
        "--replay-run-manifest " + manifest.string()
        + " --artifacts-dir " + replayed.string()
        + " --allow-dataset-mismatch --verify-hashes", output);
    EXPECT_NE(rc, 0);
    const auto changed_receipt = truetest::reproducibility::parse_json_strict(
        truetest::reproducibility::read_text_file(
            replayed / "run_receipt.json"));
    EXPECT_TRUE(changed_receipt.at(
        "dataset_mismatch_override_used").as_bool());
    EXPECT_FALSE(changed_receipt.at("exact_reproduction").as_bool());
    EXPECT_NE(truetest::reproducibility::serialize_canonical_json(
                  changed_receipt.at("mismatches")).find("SHA-256 mismatch"),
              std::string::npos);

    ASSERT_TRUE(std::filesystem::remove(dataset));
    const auto missing = directory.path() / "missing";
    rc = run_engine("engine_backtest",
        "--replay-run-manifest " + manifest.string()
        + " --artifacts-dir " + missing.string()
        + " --allow-dataset-mismatch --verify-hashes", output);
    EXPECT_NE(rc, 0);
    const auto missing_receipt = truetest::reproducibility::parse_json_strict(
        truetest::reproducibility::read_text_file(
            missing / "run_receipt.json"));
    EXPECT_EQ(missing_receipt.at("run_fingerprint").as_string(),
              truetest::reproducibility::RunManifestV1::load(manifest)
                  .run_fingerprint());
    EXPECT_TRUE(missing_receipt.at(
        "dataset_mismatch_override_used").as_bool());
    EXPECT_FALSE(missing_receipt.at("exact_reproduction").as_bool());
    EXPECT_NE(truetest::reproducibility::serialize_canonical_json(
                  missing_receipt.at("mismatches")).find("file missing"),
              std::string::npos);
    EXPECT_EQ(output.find("DataBridge: loaded"), std::string::npos);
}

TEST(CLI, DeterministicLocalReplayRejectsUnsupportedDatasetSchema)
{
    cli_temporary_directory directory;
    const auto manifest_path = directory.path() / "run_manifest.json";
    const auto bad_manifest_path = directory.path() / "schema-v2.json";
    const std::string common =
        " --symbol AAPL --instrument "
        "AAPL:tick=0.01,lot=0.000001,minq=0.000001,minn=0,"
        "maker=0.001,taker=0.001 --thread-preset logging --no-pin "
        "--no-tui --status-format off";
    std::string output;
    ASSERT_EQ(run_engine("engine_backtest",
        "--provider local --path tests/fixtures/sample_ohlcv.csv"
        " --format bar --strategy sma --seed 17" + common
        + " --write-run-manifest " + manifest_path.string()
        + " --artifacts-dir " + (directory.path() / "generated").string(),
        output), 0) << output;

    const auto original =
        truetest::reproducibility::RunManifestV1::load(manifest_path);
    auto inputs = original.deterministic_inputs();
    inputs["dataset"]["schema_version"] = std::uint64_t{2};
    truetest::reproducibility::RunManifestV1 unsupported(
        std::move(inputs), original.dataset_locations(),
        original.exact_lifecycle_replayable(), original.expected_hashes());
    unsupported.write_atomic(bad_manifest_path);

    const int rc = run_engine("engine_backtest",
        "--replay-run-manifest " + bad_manifest_path.string()
        + " --artifacts-dir " + (directory.path() / "replayed").string()
        + " --verify-hashes", output);
    EXPECT_NE(rc, 0);
    EXPECT_NE(output.find(
        "unsupported deterministic local dataset schema version"),
        std::string::npos);
    EXPECT_EQ(output.find("DataBridge: loaded"), std::string::npos);
}

TEST(CLI, DeterministicLocalManifestRecordsEveryStrategyInstanceSeed)
{
    cli_temporary_directory directory;
    const auto manifest_path = directory.path() / "run_manifest.json";
    std::string output;
    ASSERT_EQ(run_engine("engine_backtest",
        "--provider local --path tests/fixtures/sample_ohlcv.csv"
        " --format bar --strategy sma,ma-crossover --seed 91"
        " --symbol AAPL --instrument "
        "AAPL:tick=0.01,lot=0.000001,minq=0.000001,minn=0,"
        "maker=0.001,taker=0.001 --thread-preset logging --no-pin "
        "--no-tui --status-format off --write-run-manifest "
        + manifest_path.string() + " --artifacts-dir "
        + (directory.path() / "generated").string(), output), 0) << output;

    const auto manifest =
        truetest::reproducibility::RunManifestV1::load(manifest_path);
    const auto& instances = manifest.deterministic_inputs()
        .at("seed_hierarchy").at("strategy_instances").as_array();
    ASSERT_EQ(instances.size(), 2U);
    const truetest::reproducibility::DeterministicSeedDeriver seeds(91U);
    EXPECT_EQ(instances[0].at("name").as_string(), "sma");
    EXPECT_EQ(instances[0].at("index").as_u64(), 0U);
    EXPECT_EQ(instances[0].at("seed").as_u64(),
              seeds.derive(truetest::reproducibility::SeedDomain::strategy,
                           0U));
    EXPECT_EQ(instances[1].at("name").as_string(), "ma-crossover");
    EXPECT_EQ(instances[1].at("index").as_u64(), 1U);
    EXPECT_EQ(instances[1].at("seed").as_u64(),
              seeds.derive(truetest::reproducibility::SeedDomain::strategy,
                           1U));
    const auto& inputs = manifest.deterministic_inputs();
    const auto& exits = inputs.at("effective_config").at("exit_defaults");
    EXPECT_EQ(exits.at("mode").as_string(), "floor");
    EXPECT_DOUBLE_EQ(exits.at("sl_pct").as_double(), 0.003);
    EXPECT_DOUBLE_EQ(exits.at("tp_pct").as_double(), 0.01);
    EXPECT_DOUBLE_EQ(exits.at("trail_pct").as_double(), 0.0);
    const auto& instrument = inputs.at("instrument");
    EXPECT_EQ(instrument.at("rounding_policy").as_string(),
              "price-nearest-tick-quantity-floor-lot-v1");
    EXPECT_EQ(instrument.at("minimum_validation_policy").as_string(),
              "qty-epsilon-1e-12-notional-epsilon-1e-9-v1");
}

TEST(CLI, InvalidStrategyDetectedByDryRun)
{
    std::string out;
    int rc = run_truetest("--dry-run --strategy bogus", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Unknown strategy"), std::string::npos);
}

TEST(CLI, InvalidModeDetectedByDryRun)
{
    std::string out;
    int rc = run_truetest("--dry-run --mode foobar", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Unknown mode"), std::string::npos);
}

TEST(CLI, FuturesPhase0PresetUsesFractionalLiquidationDistance)
{
    std::string out;
    int rc = run_truetest("--preset futures-phase0 --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"min_liquidation_distance_pct\": 0.07"),
              std::string::npos);
}

TEST(CLI, RejectsPercentageScaleLiquidationDistance)
{
    std::string out;
    int rc = run_truetest(
        "--provider synthetic --min-liq-distance-pct 7", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("use 0.07 for 7%"), std::string::npos);
}

TEST(CLI, RejectsNonFiniteLiquidationDistance)
{
    std::string out;
    int rc = run_truetest(
        "--provider synthetic --min-liq-distance-pct nan", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("finite fraction"), std::string::npos);
}

TEST(CLI, RejectsCheckpointResumeV1BeforeRun)
{
    std::string out;
    int rc = run_truetest("--resume any-v1-checkpoint.bin --dry-run", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("diagnostic portfolio snapshots"), std::string::npos);
}

// --simple-tui opts ConsoleDashboard's ANSI-box TUI over TabbedDashboard on
// shadow/live. Research builds exercise shadow because engine_live must not
// exist; live-capable builds retain the engine_live smoke test.
#ifdef TRUETEST_RESEARCH_ONLY
TEST(CLI, SimpleTuiFlagInHelpOnBacktestAndShadow)
{
    std::string out;
    int rc = run_truetest("--help", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("--simple-tui"), std::string::npos);

    out.clear();
    rc = run_engine("engine_shadow", "--help", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("--simple-tui"), std::string::npos);
}
#else
TEST(CLI, SimpleTuiFlagInHelpOnBacktestAndLive)
{
    std::string out;
    int rc = run_truetest("--help", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("--simple-tui"), std::string::npos);

    out.clear();
    rc = run_engine_live("--help", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("--simple-tui"), std::string::npos);
}
#endif

TEST(CLI, SimpleTuiAcceptedOnDryRun)
{
    std::string out;
    int rc = run_truetest(
        "--dry-run --strategy sma --provider synthetic --simple-tui", out);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(out.find("not expected"), std::string::npos);
}

// ─── B2: JSON config file ──────────────────────────────────────────────────

TEST(ConfigFile, LoadsValuesFromJSON)
{
    // Write temp config
    std::string cfg_path = "/tmp/truetest_test_config.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"strategy":"sma","balance":77777,"sma_period":99})";
    }

    std::string out;
    int rc = run_truetest("--config " + cfg_path + " --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"strategy\": \"sma\""), std::string::npos);
    EXPECT_NE(out.find("\"balance\": 77777.0"), std::string::npos);
    EXPECT_NE(out.find("\"sma_period\": 99"), std::string::npos);

    std::remove(cfg_path.c_str());
}

TEST(ConfigFile, CLIOverridesConfigFile)
{
    std::string cfg_path = "/tmp/truetest_test_override.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"balance":99999,"strategy":"sma"})";
    }

    std::string out;
    int rc = run_truetest("--config " + cfg_path + " --balance 1234 --dump-config", out);
    EXPECT_EQ(rc, 0);
    // CLI --balance should win over config file
    EXPECT_NE(out.find("\"balance\": 1234.0"), std::string::npos);
    // Config file strategy should still apply
    EXPECT_NE(out.find("\"strategy\": \"sma\""), std::string::npos);

    std::remove(cfg_path.c_str());
}

TEST(ConfigFile, DeterministicMonteCarloPreservesStrategyInstrumentAndRiskInputs)
{
    cli_temporary_directory directory;
    const auto config_path = directory.path() / "mc_config.json";
    const auto manifest_path = directory.path() / "mc_manifest.json";
    const auto artifacts = directory.path() / "mc_artifacts";
    {
        std::ofstream config(config_path, std::ios::binary);
        config
            << "{\"monte_carlo\":true,\"mc_trials\":1,\"seed\":77,"
               "\"mc_params\":\"n_steps=8,sigma=0\","
               "\"strategy\":\"mean-reversion\","
               "\"params\":[\"period=9\"],\"fee\":\"zero\","
               "\"instrument\":[\"BTCUSDT:tick=0.01,lot=0.000001,"
               "minq=0.000001,minn=0,maker=0,taker=0\"],"
               "\"risk\":{\"max_gross_leverage\":0.8,\"unwind\":true},"
               "\"write_run_manifest\":\"" << manifest_path.string()
            << "\",\"artifacts_dir\":\"" << artifacts.string()
            << "\"}";
    }

    std::string output;
    const int rc = run_engine(
        "engine_backtest",
        "--config " + config_path.string()
            + " --no-pin --no-tui --status-format off",
        output);
    ASSERT_EQ(rc, 0) << output;
    const auto manifest = truetest::reproducibility::RunManifestV1::load(
        manifest_path);
    const auto& inputs = manifest.deterministic_inputs();
    EXPECT_DOUBLE_EQ(inputs.at("instrument").at("lot_size").as_double(),
                     0.000001);
    EXPECT_DOUBLE_EQ(inputs.at("effective_config").at("engine").at("risk")
                         .at("max_gross_leverage").as_double(),
                     0.8);
    EXPECT_TRUE(inputs.at("effective_config").at("engine").at("risk")
                    .at("unwind").as_bool());
    EXPECT_DOUBLE_EQ(inputs.at("strategy").at("config")
                         .at("effective_parameters").at("period").as_double(),
                     9.0);
}

TEST(ConfigFile, InvalidJSONExitsWithError)
{
    std::string cfg_path = "/tmp/truetest_test_bad.json";
    {
        std::ofstream f(cfg_path);
        f << "{invalid json!!!";
    }

    std::string out;
    int rc = run_truetest("--config " + cfg_path + " --dump-config", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("parse error"), std::string::npos);

    std::remove(cfg_path.c_str());
}

TEST(ConfigFile, MissingConfigFileExitsWithError)
{
    std::string out;
    int rc = run_truetest("--config /tmp/nonexistent_12345.json --dump-config", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("Cannot open config file"), std::string::npos);
}

TEST(ConfigFile, RiskLimitsFromJSON)
{
    std::string cfg_path = "/tmp/truetest_test_risk.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"risk":{"max_drawdown":0.15,"max_open_orders":50}})";
    }

    std::string out;
    int rc = run_truetest("--config " + cfg_path + " --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"max_drawdown\": 0.15"), std::string::npos);
    EXPECT_NE(out.find("\"max_open_orders\": 50"), std::string::npos);

    std::remove(cfg_path.c_str());
}

// ─── B2: --dump-config ─────────────────────────────────────────────────────

TEST(DumpConfig, OutputsValidJSON)
{
    std::string out;
    int rc = run_truetest("--dump-config", out);
    EXPECT_EQ(rc, 0);
    // Should contain opening brace (JSON object)
    EXPECT_NE(out.find("{"), std::string::npos);
    EXPECT_NE(out.find("\"strategy\""), std::string::npos);
    EXPECT_NE(out.find("\"balance\""), std::string::npos);
    EXPECT_NE(out.find("\"risk\""), std::string::npos);
}

TEST(DumpConfig, DefaultsToCryptoMinuteAnnualization)
{
    std::string out;
    const int rc = run_truetest("--dump-config", out);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(out.find("\"periods_per_year\": 525600"), std::string::npos);
}

TEST(DumpConfig, DefaultsToNonzeroCryptoFeesAndAllowsExplicitZero)
{
    std::string out;
    ASSERT_EQ(run_truetest("--dump-config", out), 0);
    EXPECT_NE(out.find("\"fee\": \"tiered\""), std::string::npos);
    EXPECT_NE(out.find("\"maker_rate\": 0.001"), std::string::npos);
    EXPECT_NE(out.find("\"taker_rate\": 0.001"), std::string::npos);

    ASSERT_EQ(run_truetest("--dry-run --fee zero", out), 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
}

TEST(DumpConfig, ReflectsCLIValues)
{
    std::string out;
    int rc = run_truetest("--dump-config --balance 42000 --seed 999", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"balance\": 42000.0"), std::string::npos);
    EXPECT_NE(out.find("\"seed\": 999"), std::string::npos);
}

// Order-latency CLI binding: realism-wiring Task 1. The latency model
// itself is unit-tested in test_latency_model.cpp; this guards that the
// flags reach the config dump and the engine binary builds with the
// underlying StochasticLatencyModel/FixedLatencyModel constructors.
TEST(DumpConfig, ReflectsOrderLatency)
{
    std::string out;
    int rc = run_truetest(
        "--dump-config --order-latency-us 5000 --order-latency-stddev-us 1500",
        out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"order_latency_us\": 5000"), std::string::npos);
    EXPECT_NE(out.find("\"order_latency_stddev_us\": 1500"), std::string::npos);
}

TEST(DryRun, OrderLatencyAcceptedInBacktest)
{
    std::string out;
    int rc = run_truetest(
        "--dry-run --strategy sma --mode backtest --order-latency-us 1000", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
}

// Impact model CLI binding: realism-wiring Task 2. SquareRootImpactModel
// is unit-tested in test_impact_model.cpp; this guards the CLI surface.
TEST(DumpConfig, ReflectsImpactModel)
{
    std::string out;
    int rc = run_truetest(
        "--dump-config --impact-k-bps 12 --impact-adv 1000000", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"impact_k_bps\": 12.0"), std::string::npos);
    EXPECT_NE(out.find("\"impact_adv\": 1000000.0"), std::string::npos);
}

TEST(DumpConfig, ReflectsWalkedBookImpact)
{
    std::string out;
    int rc = run_truetest("--dump-config --walked-book-impact", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"walked_book_impact\": true"), std::string::npos);
}

// --impact-k-bps without --impact-adv must hard-fail rather than silently
// fall through (ZeroImpactModel hides the misconfiguration). Provide a
// full config so we don't fall into interactive setup before the gate.
TEST(CLI, ImpactKBpsRequiresAdv)
{
    std::string out;
    int rc = run_truetest(
        "--provider local --path market_data.csv "
        "--strategy sma --mode backtest --impact-k-bps 10", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("requires --impact-adv"), std::string::npos);
}

TEST(DumpConfig, ReflectsFillModel)
{
    std::string out;
    int rc = run_truetest(
        "--dump-config --fill-prob 0.9 --fill-fade 0.05 --fill-decay 12", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"fill_prob\": 0.9"), std::string::npos);
    EXPECT_NE(out.find("\"fill_fade\": 0.05"), std::string::npos);
    EXPECT_NE(out.find("\"fill_decay\": 12.0"), std::string::npos);
}

TEST(DumpConfig, ReflectsMmCalibration)
{
    std::string out;
    int rc = run_truetest(
        "--dump-config --mm-levels 5 --mm-base-depth 50 "
        "--mm-spread-pct 0.001 --mm-vol-mult 2", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"mm_levels\": 5"), std::string::npos);
    EXPECT_NE(out.find("\"mm_base_depth\": 50"), std::string::npos);
    EXPECT_NE(out.find("\"mm_spread_pct\": 0.001"), std::string::npos);
    EXPECT_NE(out.find("\"mm_vol_mult\": 2.0"), std::string::npos);
}

// --fill-fade without --fill-prob is a silent no-op model — hard-fail.
TEST(CLI, FillFadeRequiresFillProb)
{
    std::string out;
    int rc = run_truetest(
        "--provider local --path market_data.csv "
        "--strategy sma --mode backtest --fill-fade 0.1", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("requires --fill-prob"), std::string::npos);
}

// Deprecated fill-pricing flags stay accepted (scripts keep running) but
// must announce that they no longer do anything.
TEST(CLI, RealisticFillsDeprecationWarning)
{
    std::string out;
    int rc = run_truetest(
        "--provider local --path tests/fixtures/sample_ohlcv.csv --strategy sma "
        "--mode backtest --realistic-fills --status-format off --seed 1 "
        "--no-pin --no-tui", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("deprecated"), std::string::npos);
}

TEST(CLI, BarSpreadBpsDeprecationWarning)
{
    std::string out;
    int rc = run_truetest(
        "--provider local --path tests/fixtures/sample_ohlcv.csv --strategy sma "
        "--mode backtest --bar-spread-bps 10 --status-format off --seed 1 "
        "--no-pin --no-tui", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("no longer affects recorded fill prices"), std::string::npos);
}

// ─── B3: --dry-run ─────────────────────────────────────────────────────────

TEST(DryRun, ValidConfigExitsZero)
{
    std::string out;
    int rc = run_truetest("--dry-run --strategy sma --mode backtest", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
    EXPECT_NE(out.find("Strategy:   sma"), std::string::npos);
}

TEST(DryRun, ShowsBalanceAndRisk)
{
    std::string out;
    int rc = run_truetest("--dry-run --balance 50000 --risk-fraction 0.05", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("$50000"), std::string::npos);
    EXPECT_NE(out.find("5%"), std::string::npos);
}

TEST(DryRun, InvalidFeeModelExitsOne)
{
    std::string out;
    int rc = run_truetest("--dry-run --fee badmodel", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Unknown fee model"), std::string::npos);
}

TEST(DryRun, FixedFeeRequiresPositiveValue)
{
    std::string out;
    EXPECT_EQ(run_truetest("--dry-run --fee fixed", out), 1);
    EXPECT_NE(out.find("requires a finite --fee-value > 0"), std::string::npos);
}

TEST(DryRun, ProtectedModesRequireAuthoritativeFeeAndPeriodInputs)
{
    std::string out;
    EXPECT_EQ(run_truetest("--dry-run --mode shadow --fee zero", out), 1);
    EXPECT_NE(out.find("research-only"), std::string::npos);

    EXPECT_EQ(run_truetest("--dry-run --mode shadow --fee tiered "
                           "--maker-rate 0.001 --taker-rate 0.001", out), 1);
    EXPECT_NE(out.find("explicit --periods-per-year"), std::string::npos);

    EXPECT_EQ(run_truetest("--dry-run --mode shadow --fee tiered "
                           "--maker-rate 0.001 --taker-rate 0.001 "
                           "--periods-per-year 0", out), 1);
    EXPECT_NE(out.find("must be positive"), std::string::npos);

    EXPECT_EQ(run_truetest("--dry-run --mode shadow --stream publicTrade "
                           "--fee tiered --maker-rate 0.001 --taker-rate 0.001", out), 1);
    EXPECT_NE(out.find("explicit --periods-per-year"), std::string::npos);

    EXPECT_EQ(run_truetest("--dry-run --mode shadow --fee tiered "
                           "--maker-rate 0.001 --taker-rate 0.001 "
                           "--periods-per-year 525600", out), 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
}

TEST(DryRun, LiveFlagRejectedOnBacktestBinary)
{
    std::string out;
    int rc = run_truetest("--dry-run --live", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Live mode is only permitted on the engine_live binary"),
              std::string::npos);
}

TEST(DryRun, ModeLiveRejectedOnBacktestBinary)
{
    std::string out;
    int rc = run_truetest("--dry-run --mode live", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Live mode is only permitted on the engine_live binary"),
              std::string::npos);
}

#ifndef TRUETEST_RESEARCH_ONLY
TEST(DryRun, LiveBinaryMainnetFuturesRequiresVenueCaps)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live",
        out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Refusing mainnet futures live mode without venue risk caps"),
              std::string::npos);
}

TEST(DryRun, LiveBinaryMainnetFuturesRequiresDailyLoss)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --max-notional 25",
        out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Refusing mainnet futures live mode with --max-daily-loss disabled"),
              std::string::npos);
}

TEST(DryRun, LiveBinaryMainnetFuturesAcceptsCapsAndDailyLoss)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --max-notional 25 --max-daily-loss 5 "
        "--log-events /tmp/truetest-live-dry-run.bin --fee tiered "
        "--maker-rate 0.001 --taker-rate 0.001 --periods-per-year 525600",
        out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
}

TEST(DryRun, LiveBinaryMainnetFuturesRequiresDurableEventLog)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --max-notional 25 --max-daily-loss 5",
        out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("without a durable binary event log"),
              std::string::npos);
}

TEST(DryRun, LiveBinaryMainnetFuturesRejectsNonRegularFileEventLog)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --max-notional 25 --max-daily-loss 5 "
        "--log-events /dev/null",
        out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("target is not a regular file"), std::string::npos);
}

TEST(DryRun, LiveBinaryMainnetFuturesAcceptsFreshRegularFileEventLogPath)
{
    const std::string path = "/tmp/truetest-h07-dry-run-"
        + std::to_string(::getpid()) + ".bin";
    std::filesystem::remove(path);
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --max-notional 25 --max-daily-loss 5 "
        "--log-events " + path + " --fee tiered --maker-rate 0.001 "
        "--taker-rate 0.001 --periods-per-year 525600",
        out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
    std::filesystem::remove(path);
}

TEST(DryRun, LiveBinaryFuturesRejectsNonFiniteRiskCaps)
{
    for (const char* bad_flag : {
             "--max-notional inf --max-daily-loss 5",
             "--max-leverage inf --max-daily-loss 5",
             "--max-notional 25 --max-daily-loss inf"})
    {
        std::string out;
        const std::string args =
            std::string("--dry-run --provider binance-futures ")
            + "--symbol BTCUSDT --mode live --live " + bad_flag
            + " --log-events /tmp/truetest-live-dry-run.bin";
        const int rc = run_engine_live(args, out);
        EXPECT_EQ(rc, 1) << bad_flag;
        EXPECT_NE(out.find("must be finite values"), std::string::npos)
            << bad_flag;
    }
}

TEST(DryRun, LiveBinaryFuturesRejectsRotatedAuthoritativeLog)
{
    std::string out;
    const int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --max-notional 25 --max-daily-loss 5 "
        "--log-events /tmp/truetest-live-dry-run.bin "
        "--log-max-size 1 --log-keep 0",
        out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("rotation enabled"), std::string::npos);
}

TEST(DryRun, LiveBinaryTestnetFuturesAllowsWarningOnlyCaps)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --testnet --fee tiered --maker-rate 0.001 "
        "--taker-rate 0.001 --periods-per-year 525600",
        out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("No venue risk caps set"), std::string::npos);
    EXPECT_NE(out.find("--max-daily-loss is 0"), std::string::npos);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
}
#endif

// ─── B4: QuestDB persistence flags ─────────────────────────────────────────

#ifdef HAS_QUESTDB

TEST(CLI, PersistFlagAccepted)
{
    std::string out;
    int rc = run_truetest("--persist --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"persist\": true"), std::string::npos);
}

TEST(CLI, RunTagFlagAccepted)
{
    std::string out;
    int rc = run_truetest("--persist --run-tag my_run --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"run_tag\": \"my_run\""), std::string::npos);
}

TEST(CLI, QuestdbPortsAccepted)
{
    std::string out;
    int rc = run_truetest("--persist --questdb-ilp-port 19009 "
                          "--questdb-http-port 19000 --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"questdb_ilp_port\": 19009"), std::string::npos);
    EXPECT_NE(out.find("\"questdb_http_port\": 19000"), std::string::npos);
}

#else

TEST(CLI, PersistFlagRejectedWhenQuestDbDisabled)
{
    std::string out;
    int rc = run_truetest("--persist --dump-config", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("--persist"), std::string::npos);
    EXPECT_NE(out.find("not expected"), std::string::npos);
}

#endif // HAS_QUESTDB

// ─── B5: Maker queue model (Phase 1) ───────────────────────────────────────

TEST(CLI, MakerQueueModelAcceptedWithoutDepth)
{
    // Without L2, QueueAware is conservative (no front-of-queue); bar sweep still works.
    std::string out;
    int rc = run_truetest("--maker-queue-model uniform --dry-run "
                          "--strategy sma --provider synthetic", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
    EXPECT_NE(out.find("without --depth-stream"), std::string::npos);
    EXPECT_NE(out.find("no optimistic join-front"), std::string::npos);
}

TEST(CLI, MakerQueueModelAcceptedWithDepth)
{
    std::string out;
    int rc = run_truetest("--maker-queue-model uniform --depth-stream depth20 "
                          "--dry-run --strategy sma --provider local --path market_data.csv", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
    EXPECT_EQ(out.find("without --depth-stream"), std::string::npos);
}
