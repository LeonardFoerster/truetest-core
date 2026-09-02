#include "reproducibility/run_manifest.h"
#include "reproducibility/deterministic_seed.h"
#include "reproducibility/sha256.h"
#include "simulation/deterministic_mc_manifest.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cfenv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace repro = truetest::reproducibility;

namespace {

class temporary_directory final
{
public:
    temporary_directory()
    {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path()
            / ("truetest-r6-manifest-" + std::to_string(::getpid()) + "-"
               + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~temporary_directory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

    std::filesystem::path write(std::string name, std::string bytes) const
    {
        const auto output_path = path_ / std::move(name);
        std::ofstream output(output_path, std::ios::binary);
        output << bytes;
        output.close();
        return output_path;
    }

private:
    std::filesystem::path path_;
};

repro::CanonicalJsonValue model(std::string id, std::string version,
                                repro::CanonicalJsonValue::Object config = {})
{
    return repro::CanonicalJsonValue::object({
        {"config", std::move(config)},
        {"id", std::move(id)},
        {"version", std::move(version)},
    });
}

repro::CanonicalJsonValue valid_inputs(const repro::DatasetSnapshot& dataset,
                                       std::uint64_t master_seed = 42)
{
    const repro::DeterministicSeedDeriver seeds(master_seed);
    return repro::CanonicalJsonValue::object({
        {"build", repro::build_identity_json(repro::current_build_identity())},
        {"dataset", repro::dataset_identity_json(dataset)},
        {"determinism_envelope", repro::CanonicalJsonValue::object({
            {"cpu", "identical-architecture"},
            {"floating_point", "ieee754-binary64-fixed-order"},
            {"libm", "identical-toolchain"},
            {"scope", "same-build-toolchain-libc-stdlib-architecture"},
        })},
        {"effective_config", repro::CanonicalJsonValue::object({
            {"balance", 10'000.0},
            {"execution_bar_delay", 1},
            {"provider", "local"},
        })},
        {"fee_schedule", repro::CanonicalJsonValue::object({
            {"maker_rate", 0.001},
            {"taker_rate", 0.001},
            {"version", "tiered-fee-v1"},
        })},
        {"instrument", repro::CanonicalJsonValue::object({
            {"asset_type", "spot"},
            {"lot_size", 0.00000001},
            {"minimum_notional", 0.0},
            {"symbol", "BTCUSDT"},
            {"tick_size", 0.01},
            {"venue", "local"},
        })},
        {"models", repro::CanonicalJsonValue::object({
            {"fee", model("tiered", "1")},
            {"fill", model("perfect", "1")},
            {"impact", model("zero", "1")},
            {"latency", model("zero", "1")},
            {"queue", model("none", "1")},
            {"simulator", model("truetest-event-engine", "1")},
            {"synthetic_gbm", model("disabled", "1")},
            {"synthetic_l2", model("disabled", "1")},
        })},
        {"monte_carlo", repro::CanonicalJsonValue::object({
            {"parallel", false},
            {"trial_count", 1U},
        })},
        {"run_mode", "backtest"},
        {"seed_hierarchy", repro::CanonicalJsonValue::object({
            {"derivation_version", std::uint64_t{repro::kSeedDerivationVersion}},
            {"domains", repro::CanonicalJsonValue::object({
                {"fill_model", seeds.derive(repro::SeedDomain::fill_model)},
                {"market_maker", seeds.derive(repro::SeedDomain::market_maker)},
                {"run", seeds.derive(repro::SeedDomain::run)},
            })},
            {"master_seed", master_seed},
            {"trials", repro::CanonicalJsonValue::array({
                repro::CanonicalJsonValue::object({
                    {"index", 0U},
                    {"seed", seeds.trial_seed(0)},
                }),
            })},
        })},
        {"strategy", model("sma", "1", {{"period", 20}})},
        {"target", "engine_backtest"},
        {"threading", repro::CanonicalJsonValue::object({
            {"completion_order_affects_results", false},
            {"preset", "inline"},
            {"workers", 1U},
        })},
    });
}

repro::DatasetSnapshot dataset_fixture(const temporary_directory& directory)
{
    const auto second = directory.write("b.csv", "timestamp,price\n2,101\n");
    const auto first = directory.write("a.csv", "timestamp,price\n1,100\n");
    return repro::snapshot_dataset(
        "fixture-bars", 1,
        {{"input-0001", second}, {"input-0000", first}},
        "2024-01-01T00:00:00Z", "2024-01-01T00:01:00Z",
        "timestamp-ascending", "logical-input-order-then-row-order", "0s");
}

repro::RunManifestV1 manifest_fixture(const temporary_directory& directory,
                                      std::uint64_t master_seed = 42)
{
    const auto dataset = dataset_fixture(directory);
    repro::RunManifestV1 manifest(
        valid_inputs(dataset, master_seed),
        repro::dataset_locations_json(dataset), true);
    for (const std::string_view kind : {
             "economic_result", "event_log", "lifecycle", "report"})
        manifest.set_expected_hash(
            std::string(kind), repro::sha256_hex(kind));
    return manifest;
}

} // namespace

TEST(BuildIdentity, CapturesRequiredSourceToolchainAndDependencyFields)
{
    const auto build = repro::current_build_identity();
    EXPECT_FALSE(build.git_commit_sha.empty());
    EXPECT_FALSE(build.build_type.empty());
    EXPECT_FALSE(build.cmake_profile.empty());
    EXPECT_FALSE(build.build_flags.empty());
    EXPECT_NE(build.build_flags.find("cxx_compiler_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("c_compiler_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("linker_sha256="), std::string::npos);
    EXPECT_NE(build.build_flags.find("archiver_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("ranlib_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("cc1_sha256="), std::string::npos);
    EXPECT_NE(build.build_flags.find("cc1plus_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("collect2_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("assembler_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("driver_linker_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("toolchain_file_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("cmake_sha256="), std::string::npos);
    EXPECT_NE(build.build_flags.find("build_executor_sha256="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("git_sha256="), std::string::npos);
    EXPECT_NE(build.build_flags.find("exe_linker_flags="),
              std::string::npos);
    EXPECT_NE(build.build_flags.find("exe_linker_flags_config="),
              std::string::npos);
    EXPECT_FALSE(build.compiler.empty());
    EXPECT_FALSE(build.standard_library.empty());
    EXPECT_FALSE(build.libc.empty());
    EXPECT_FALSE(build.architecture.empty());
    EXPECT_FALSE(build.march.empty());
    EXPECT_FALSE(build.floating_point_flags.empty());
    EXPECT_TRUE(repro::is_lower_hex_sha256(build.executable_sha256));
    EXPECT_TRUE(build.dependencies.contains("boost"));
    EXPECT_TRUE(build.dependencies.contains("openssl"));
    EXPECT_EQ(build.dependencies.at("cli11"),
              "v2.4.2@6c7b07a878ad834957b98d0f9ce1dbe0cb204fc9");
    EXPECT_EQ(build.dependencies.at("zstd"),
              "v1.5.6@794ea1b0afca0f020f4e57b6732332231fb23c70");
#if defined(__linux__)
    for (const std::string_view library : {
             "runtime-libstdc++", "runtime-libm", "runtime-libc",
             "runtime-libgcc-s", "runtime-loader"})
    {
        const auto& identity = build.dependencies.at(std::string(library));
        const auto separator = identity.rfind('@');
        ASSERT_NE(separator, std::string::npos) << library;
        EXPECT_TRUE(repro::is_lower_hex_sha256(
            std::string_view(identity).substr(separator + 1U))) << identity;
    }
    EXPECT_TRUE(repro::is_lower_hex_sha256(
        build.dependencies.at("runtime-dso-set")));
#endif
    if (build.git_dirty) {
        EXPECT_TRUE(repro::is_lower_hex_sha256(build.worktree_diff_sha256));
    }

    EXPECT_EQ(repro::build_identity_from_json(
                  repro::build_identity_json(build)), build);
}

TEST(DeterministicRuntimeEnvironment, RejectsLoaderInterpositionAndFpDrift)
{
    const char* previous_preload = std::getenv("LD_PRELOAD");
    const std::string saved_preload = previous_preload == nullptr
        ? std::string{} : std::string(previous_preload);
    ASSERT_EQ(::setenv("LD_PRELOAD", "/invalid/truetest-interposer.so", 1), 0);
    EXPECT_THROW(repro::validate_deterministic_runtime_environment(),
                 std::runtime_error);
    if (previous_preload == nullptr)
        ASSERT_EQ(::unsetenv("LD_PRELOAD"), 0);
    else
        ASSERT_EQ(::setenv("LD_PRELOAD", saved_preload.c_str(), 1), 0);

    const int original_rounding = std::fegetround();
    ASSERT_NE(original_rounding, -1);
    ASSERT_EQ(std::fesetround(FE_DOWNWARD), 0);
    EXPECT_THROW(repro::validate_deterministic_runtime_environment(),
                 std::runtime_error);
    ASSERT_EQ(std::fesetround(original_rounding), 0);

#if defined(__x86_64__) || defined(__i386__)
    const unsigned original_mxcsr = _mm_getcsr();
    _mm_setcsr(original_mxcsr | 0x8000U);
    EXPECT_THROW(repro::validate_deterministic_runtime_environment(),
                 std::runtime_error);
    _mm_setcsr(original_mxcsr);
#endif

    EXPECT_NO_THROW(repro::validate_deterministic_runtime_environment());
}

TEST(DatasetSnapshot, HashesFilesAndCanonicalizesLogicalInputOrder)
{
    temporary_directory directory;
    const auto snapshot = dataset_fixture(directory);
    ASSERT_EQ(snapshot.artifacts.size(), 2U);
    EXPECT_EQ(snapshot.artifacts[0].logical_name, "input-0000");
    EXPECT_EQ(snapshot.artifacts[1].logical_name, "input-0001");
    EXPECT_TRUE(repro::is_lower_hex_sha256(snapshot.artifacts[0].sha256));
    EXPECT_TRUE(repro::is_lower_hex_sha256(snapshot.aggregate_sha256));

    auto reverse = repro::snapshot_dataset(
        "fixture-bars", 1,
        {{"input-0000", directory.path() / "a.csv"},
         {"input-0001", directory.path() / "b.csv"}},
        "2024-01-01T00:00:00Z", "2024-01-01T00:01:00Z",
        "timestamp-ascending", "logical-input-order-then-row-order", "0s");
    EXPECT_EQ(reverse.aggregate_sha256, snapshot.aggregate_sha256);
}

TEST(DatasetSnapshot, DetectsChangedAndMissingInputs)
{
    temporary_directory directory;
    const auto snapshot = dataset_fixture(directory);
    EXPECT_TRUE(repro::verify_dataset(snapshot).exact);

    {
        std::ofstream changed(
            directory.path() / "a.csv",
            std::ios::binary | std::ios::trunc);
        changed << "timestamp,price\n1,900\n";
    }
    auto verification = repro::verify_dataset(snapshot);
    EXPECT_FALSE(verification.exact);
    ASSERT_FALSE(verification.mismatches.empty());
    EXPECT_NE(verification.mismatches.front().find("SHA-256 mismatch"),
              std::string::npos);

    {
        std::ofstream changed(directory.path() / "a.csv", std::ios::app);
        changed << "3,999\n";
    }
    verification = repro::verify_dataset(snapshot);
    EXPECT_FALSE(verification.exact);
    ASSERT_FALSE(verification.mismatches.empty());
    EXPECT_NE(verification.mismatches.front().find("size mismatch"),
              std::string::npos);

    std::filesystem::remove(directory.path() / "b.csv");
    verification = repro::verify_dataset(snapshot);
    EXPECT_FALSE(verification.exact);
    EXPECT_EQ(verification.mismatches.size(), 2U);
}

TEST(DatasetSnapshot, EmbeddedSyntheticDescriptorIsSelfVerifying)
{
    const std::string descriptor =
        R"({"model":"gbm","n_steps":50,"version":"1"})";
    auto snapshot = repro::snapshot_embedded_dataset(
        "synthetic-gbm", 1, "generator-config", descriptor,
        "2024-01-01T00:00:00Z", "2024-01-01T00:49:00Z",
        "generated-index-order", "price-before-l2-at-equal-index", "0s");
    EXPECT_TRUE(repro::verify_dataset(snapshot).exact);
    ASSERT_EQ(snapshot.artifacts.size(), 1U);
    EXPECT_EQ(snapshot.artifacts.front().size_bytes, descriptor.size());

    snapshot.artifacts.front().locator =
        "embedded-sha256:" + std::string(64, '0');
    const auto verification = repro::verify_dataset(snapshot);
    EXPECT_FALSE(verification.exact);
    ASSERT_EQ(verification.mismatches.size(), 1U);
    EXPECT_NE(verification.mismatches.front().find("descriptor mismatch"),
              std::string::npos);
}

TEST(RunManifest, CanonicalRoundTripAndFingerprintAreStable)
{
    temporary_directory directory;
    auto manifest = manifest_fixture(directory);
    manifest.set_expected_hash("economic_result", repro::sha256_hex("economic"));
    const std::string bytes = manifest.serialize();
    const auto parsed = repro::RunManifestV1::parse(bytes);
    EXPECT_EQ(parsed.serialize(), bytes);
    EXPECT_EQ(parsed.run_fingerprint(), manifest.run_fingerprint());
    EXPECT_TRUE(parsed.exact_lifecycle_replayable());
}

TEST(RunManifest, CheckedInExampleIsSchemaValidAndFingerprintExact)
{
    const auto source_root = std::filesystem::path(TEST_FIXTURES_DIR)
        .parent_path().parent_path();
    const auto example_path = source_root / "docs/examples/run_manifest.v1.json";
    const auto root = repro::parse_json_strict(
        repro::read_text_file(example_path));
    const std::string calculated_fingerprint = repro::sha256_hex(
        repro::serialize_canonical_json(root.at("deterministic_inputs")));
    EXPECT_EQ(root.at("run_fingerprint").as_string(),
              calculated_fingerprint);
    const auto manifest = repro::RunManifestV1::load(
        example_path);
    EXPECT_EQ(manifest.run_fingerprint(),
              "39bd6fc59807581466fed84db27af2693d208427a256aaeece05d702c152c962");
    EXPECT_FALSE(manifest.exact_lifecycle_replayable());
    const auto& inputs = manifest.deterministic_inputs();
    EXPECT_EQ(inputs.at("effective_config").at("engine")
                  .at("threading").at("preset").as_string(),
              "logging");
    EXPECT_EQ(inputs.at("models").at("queue")
                  .at("version").as_string(),
              "maker-queue-v1");
    EXPECT_EQ(inputs.at("monte_carlo")
                  .at("lifecycle_record_capacity").as_u64(),
              131'072U);
    EXPECT_EQ(inputs.at("threading").at("trial_engine_preset").as_string(),
              "logging");
    EXPECT_EQ(inputs.at("dataset").at("tie_breaking_rule").as_string(),
              "bar-only-engine-input-with-observational-l2-sidecar-v1");
    EXPECT_EQ(inputs.at("models").at("simulator").at("config")
                  .at("tie_breaking").as_string(),
              "bar-only-stable-generated-index-v1");

    // The checked-in example deliberately records the build which generated
    // it. Replace only that deployment-specific identity and prove that the
    // complete current schema reconstructs an executable effective config.
    auto reconstructable_inputs = manifest.deterministic_inputs();
    reconstructable_inputs["build"] =
        repro::build_identity_json(repro::current_build_identity());
    const repro::RunManifestV1 reconstructable(
        std::move(reconstructable_inputs), manifest.dataset_locations(), false);
    temporary_directory artifacts;
    const auto config = truetest::simulation::mc_config_from_manifest(
        reconstructable, artifacts.path());
    EXPECT_EQ(config.base_seed, 424'242U);
    EXPECT_TRUE(config.master_seed_explicitly_set);
    EXPECT_EQ(config.n_trials, 1U);
    EXPECT_EQ(config.strategy_name, "ma-crossover");
    EXPECT_TRUE(config.persist_trial_lifecycle);
}

TEST(RunManifest, ExactLifecycleManifestCannotBePublishedWithoutAllHashes)
{
    temporary_directory directory;
    const auto dataset = dataset_fixture(directory);
    repro::RunManifestV1 incomplete(
        valid_inputs(dataset), repro::dataset_locations_json(dataset), true);
    EXPECT_THROW((void)incomplete.serialize(), std::invalid_argument);

    incomplete.set_expected_hash("event_log", repro::sha256_hex("events"));
    incomplete.set_expected_hash("economic_result", repro::sha256_hex("economy"));
    incomplete.set_expected_hash("report", repro::sha256_hex("report"));
    EXPECT_THROW((void)incomplete.serialize(), std::invalid_argument);
    incomplete.set_expected_hash("lifecycle", repro::sha256_hex("lifecycle"));
    EXPECT_NO_THROW((void)incomplete.serialize());

    incomplete.set_expected_hash("unknown-artifact",
                                 repro::sha256_hex("unknown"));
    EXPECT_THROW((void)incomplete.serialize(), std::invalid_argument);
}

TEST(RunManifest, FingerprintChangesForSeedDatasetConfigAndModelVersion)
{
    temporary_directory directory;
    const auto dataset = dataset_fixture(directory);
    const auto locations = repro::dataset_locations_json(dataset);
    const auto baseline = repro::RunManifestV1(
        valid_inputs(dataset, 1), locations, true);

    const auto different_seed = repro::RunManifestV1(
        valid_inputs(dataset, 2), locations, true);
    EXPECT_NE(different_seed.run_fingerprint(), baseline.run_fingerprint());

    auto config_inputs = valid_inputs(dataset, 1);
    config_inputs["effective_config"]["balance"] = 11'000.0;
    const repro::RunManifestV1 different_config(
        std::move(config_inputs), locations, true);
    EXPECT_NE(different_config.run_fingerprint(), baseline.run_fingerprint());

    auto model_inputs = valid_inputs(dataset, 1);
    model_inputs["models"]["fill"]["version"] = "2";
    const repro::RunManifestV1 different_model(
        std::move(model_inputs), locations, true);
    EXPECT_NE(different_model.run_fingerprint(), baseline.run_fingerprint());

    directory.write("a.csv", "timestamp,price\n1,999\n");
    const auto changed_dataset = repro::snapshot_dataset(
        "fixture-bars", 1,
        {{"input-0000", directory.path() / "a.csv"},
         {"input-0001", directory.path() / "b.csv"}},
        "2024-01-01T00:00:00Z", "2024-01-01T00:01:00Z",
        "timestamp-ascending", "logical-input-order-then-row-order", "0s");
    const repro::RunManifestV1 different_dataset(
        valid_inputs(changed_dataset, 1),
        repro::dataset_locations_json(changed_dataset), true);
    EXPECT_NE(different_dataset.run_fingerprint(), baseline.run_fingerprint());
}

TEST(RunManifest, ReceiptWallClockAndExpectedHashesDoNotChangeFingerprint)
{
    temporary_directory directory;
    auto manifest = manifest_fixture(directory);
    const std::string fingerprint = manifest.run_fingerprint();
    manifest.set_expected_hash("report", repro::sha256_hex("report"));
    EXPECT_EQ(manifest.run_fingerprint(), fingerprint);

    repro::RunReceiptV1 first;
    first.executed_at_utc = "2026-09-01T00:00:00Z";
    first.duration_ms = 1;
    first.status = "complete";
    first.run_fingerprint = fingerprint;
    repro::RunReceiptV1 second = first;
    second.executed_at_utc = "2026-09-02T00:00:00Z";
    second.duration_ms = 999;
    EXPECT_NE(first.serialize(), second.serialize());
    EXPECT_EQ(manifest.run_fingerprint(), fingerprint);

    first.verification_scope = "trial";
    first.trial_index = 4U;
    const auto scoped = repro::parse_json_strict(first.serialize());
    EXPECT_EQ(scoped.at("receipt_schema_version").as_u64(), 2U);
    EXPECT_EQ(scoped.at("verification_scope").as_string(), "trial");
    EXPECT_EQ(scoped.at("trial_index").as_u64(), 4U);
}

TEST(RunManifest, RejectsCorruptionUnsupportedSchemaAndMissingModelVersion)
{
    temporary_directory directory;
    const auto valid = manifest_fixture(directory).serialize();

    auto bad_fingerprint = repro::parse_json_strict(valid);
    bad_fingerprint["run_fingerprint"] = std::string(64, '0');
    EXPECT_THROW((void)repro::RunManifestV1::parse(
                     repro::serialize_canonical_json(bad_fingerprint)),
                 std::invalid_argument);

    auto bad_schema = repro::parse_json_strict(valid);
    bad_schema["manifest_schema_version"] = 2U;
    EXPECT_THROW((void)repro::RunManifestV1::parse(
                     repro::serialize_canonical_json(bad_schema)),
                 std::invalid_argument);

    auto missing_version = repro::parse_json_strict(valid);
    missing_version["deterministic_inputs"]["models"]["fill"]
        .as_object().erase("version");
    missing_version["run_fingerprint"] = repro::sha256_hex(
        repro::serialize_canonical_json(
            missing_version.at("deterministic_inputs")));
    EXPECT_THROW((void)repro::RunManifestV1::parse(
                     repro::serialize_canonical_json(missing_version)),
                 std::out_of_range);

    EXPECT_THROW((void)repro::RunManifestV1::parse("{broken"),
                 std::invalid_argument);
}

TEST(RunManifest, RejectsMonteCarloTrialCountsOutsideSchemaV1Bound)
{
    temporary_directory directory;
    const auto valid = manifest_fixture(directory).serialize();

    for (const std::uint64_t invalid_count : {
             std::uint64_t{0},
             repro::kRunManifestV1MaxMonteCarloTrials + 1U,
             std::numeric_limits<std::uint64_t>::max()})
    {
        auto root = repro::parse_json_strict(valid);
        root["deterministic_inputs"]["run_mode"] = "monte_carlo";
        root["deterministic_inputs"]["monte_carlo"]["trial_count"] =
            invalid_count;
        root["exact_lifecycle_replayable"] = false;
        root["expected_hashes"] = repro::CanonicalJsonValue::object({});
        root["run_fingerprint"] = repro::sha256_hex(
            repro::serialize_canonical_json(
                root.at("deterministic_inputs")));

        EXPECT_THROW((void)repro::RunManifestV1::parse(
                         repro::serialize_canonical_json(root)),
                     std::invalid_argument)
            << invalid_count;
    }
}

TEST(RunManifest, AtomicWriterRefusesOverwriteAndLeavesCanonicalArtifact)
{
    temporary_directory directory;
    const auto path = directory.path() / "run_manifest.json";
    const auto manifest = manifest_fixture(directory);
    ASSERT_NO_THROW(manifest.write_atomic(path));
    EXPECT_EQ(repro::read_text_file(path), manifest.serialize());
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".partial"));
    EXPECT_THROW(manifest.write_atomic(path), std::runtime_error);
}

TEST(RunManifest, SchemaSpecificReaderIsBoundedForFullCampaignManifests)
{
    EXPECT_GT(repro::kRunManifestV1MaxBytes,
              64U * 1024U * 1024U);
    temporary_directory directory;
    const auto oversized = directory.path() / "oversized.json";
    {
        std::ofstream output(oversized, std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output.seekp(static_cast<std::streamoff>(
            repro::kRunManifestV1MaxBytes));
        output.put('x');
        ASSERT_TRUE(output.good());
    }
    EXPECT_THROW((void)repro::RunManifestV1::load(oversized),
                 std::runtime_error);
}
