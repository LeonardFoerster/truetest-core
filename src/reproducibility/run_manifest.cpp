#include "run_manifest.h"

#include "deterministic_rng.h"
#include "deterministic_seed.h"
#include "sha256.h"

#include <tt/truetest_version.h>

#include <algorithm>
#include <array>
#include <cfenv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#if defined(__linux__)
#include <link.h>
#endif

namespace truetest::reproducibility {

namespace {

[[nodiscard]] std::string standard_library_identity()
{
#if defined(_LIBCPP_VERSION)
    return "libc++ " + std::to_string(_LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
    return "libstdc++ " + std::to_string(__GLIBCXX__);
#elif defined(_MSVC_STL_VERSION)
    return "msvc-stl " + std::to_string(_MSVC_STL_VERSION);
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string libc_identity()
{
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__)
    return "glibc " + std::to_string(__GLIBC__) + "."
        + std::to_string(__GLIBC_MINOR__);
#elif defined(_WIN32)
    return "msvcrt";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string architecture_identity()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

[[nodiscard]] const std::map<std::string, std::string, std::less<>>&
runtime_library_identities()
{
    static const auto identities = [] {
    std::vector<std::filesystem::path> paths;
#if defined(__linux__)
    const auto visit = [](dl_phdr_info* info, std::size_t,
                          void* opaque) noexcept {
        auto& loaded = *static_cast<decltype(paths)*>(opaque);
        if (info == nullptr || info->dlpi_name == nullptr
            || info->dlpi_name[0] == '\0')
            return 0;
        const std::filesystem::path path(info->dlpi_name);
        if (path.filename() != "linux-vdso.so.1")
            loaded.push_back(path);
        return 0;
    };
    (void)::dl_iterate_phdr(visit, &paths);
#endif

    std::vector<std::string> loaded_identities;
    loaded_identities.reserve(paths.size());
    for (const auto& path : paths)
    {
        std::error_code error;
        const auto resolved = std::filesystem::canonical(path, error);
        if (error)
            throw std::runtime_error(
                "cannot resolve loaded deterministic runtime object '"
                + path.string() + "': " + error.message());
        loaded_identities.push_back(
            resolved.filename().string() + "@" + sha256_file_hex(resolved));
    }
    std::sort(loaded_identities.begin(), loaded_identities.end());
    loaded_identities.erase(
        std::unique(loaded_identities.begin(), loaded_identities.end()),
        loaded_identities.end());

    const bool strict_profile = std::string_view(TRUETEST_BUILD_PROFILE)
        == "linux-deterministic-v1";
    const std::array expected_prefixes{
        std::string_view{"libstdc++.so"}, std::string_view{"libm.so"},
        std::string_view{"libc.so"}, std::string_view{"libgcc_s.so"},
        std::string_view{"ld-linux-x86-64.so"},
    };
    std::array<unsigned, expected_prefixes.size()> expected_counts{};
    for (const auto& identity : loaded_identities)
    {
        const std::string_view filename(identity.data(), identity.find('@'));
        bool expected = false;
        for (std::size_t index = 0; index < expected_prefixes.size(); ++index)
        {
            if (filename.starts_with(expected_prefixes[index]))
            {
                ++expected_counts[index];
                expected = true;
                break;
            }
        }
        if (strict_profile && !expected)
            throw std::runtime_error(
                "deterministic runtime rejects unexpected loaded ELF object: "
                + std::string(filename));
    }
    if (strict_profile
        && std::any_of(expected_counts.begin(), expected_counts.end(),
                       [](unsigned count) { return count != 1U; }))
        throw std::runtime_error(
            "deterministic runtime requires exactly one audited C/C++/math/"
            "compiler-support/loader ELF object");

    std::map<std::string, std::string, std::less<>> result;
    std::string set_preimage;
    for (const auto& identity : loaded_identities)
    {
        set_preimage += std::to_string(identity.size());
        set_preimage.push_back(':');
        set_preimage += identity;
        set_preimage.push_back('\n');
        result.emplace("runtime-dso-" + sha256_hex(identity).substr(0, 16),
                       identity);
    }
    result.emplace("runtime-dso-set", sha256_hex(set_preimage));
    const auto capture_named = [&](std::string key, std::string_view prefix) {
        const auto found = std::find_if(
            loaded_identities.begin(), loaded_identities.end(),
            [prefix](const std::string& identity) {
                return std::string_view(identity).starts_with(prefix);
            });
        result.emplace(std::move(key), found == loaded_identities.end()
            ? std::string("unavailable") : *found);
    };
    capture_named("runtime-libstdc++", "libstdc++.so");
    capture_named("runtime-libm", "libm.so");
    capture_named("runtime-libc", "libc.so");
    capture_named("runtime-libgcc-s", "libgcc_s.so");
    capture_named("runtime-loader", "ld-linux-x86-64.so");
    return result;
    }();
    return identities;
}

[[nodiscard]] const std::string& current_executable_sha256()
{
    static const std::string identity = [] {
#if defined(__linux__)
        std::error_code error;
        const auto executable = std::filesystem::canonical(
            "/proc/self/exe", error);
        if (error)
            throw std::runtime_error(
                "cannot resolve deterministic runtime executable: "
                + error.message());
        return sha256_file_hex(executable);
#else
        return std::string("unavailable");
#endif
    }();
    return identity;
}

[[nodiscard]] const CanonicalJsonValue::Object& require_object(
    const CanonicalJsonValue& value, std::string_view field)
{
    if (!value.is_object())
        throw std::invalid_argument("manifest field '" + std::string(field)
            + "' must be an object");
    return value.as_object();
}

[[nodiscard]] const CanonicalJsonValue::Array& require_array(
    const CanonicalJsonValue& value, std::string_view field)
{
    if (!value.is_array())
        throw std::invalid_argument("manifest field '" + std::string(field)
            + "' must be an array");
    return value.as_array();
}

[[nodiscard]] std::string require_string(
    const CanonicalJsonValue& object, std::string_view field,
    bool permit_empty = false)
{
    const auto& value = object.at(field);
    if (!value.is_string() || (!permit_empty && value.as_string().empty()))
        throw std::invalid_argument("manifest field '" + std::string(field)
            + "' must be a nonempty string");
    return value.as_string();
}

void require_exact_object_keys(const CanonicalJsonValue& value,
                               std::initializer_list<std::string_view> allowed,
                               std::string_view context)
{
    const auto& object = require_object(value, context);
    std::set<std::string_view, std::less<>> allowed_set(allowed);
    for (const auto& [key, child] : object)
    {
        (void)child;
        if (!allowed_set.contains(key))
            throw std::invalid_argument("unknown " + std::string(context)
                + " field: " + key);
    }
}

[[nodiscard]] CanonicalJsonValue dataset_descriptor_array(
    const DatasetSnapshot& snapshot)
{
    CanonicalJsonValue::Array artifacts;
    artifacts.reserve(snapshot.artifacts.size());
    for (const auto& artifact : snapshot.artifacts)
    {
        artifacts.push_back(CanonicalJsonValue::object({
            {"logical_name", artifact.logical_name},
            {"sha256", artifact.sha256},
            {"size_bytes", artifact.size_bytes},
        }));
    }
    return artifacts;
}

[[nodiscard]] std::string dataset_aggregate_sha256(
    const DatasetSnapshot& snapshot)
{
    return sha256_hex(serialize_canonical_json(
        dataset_descriptor_array(snapshot)));
}

void validate_dataset_metadata(const DatasetSnapshot& snapshot)
{
    if (snapshot.dataset_id.empty())
        throw std::invalid_argument("dataset_id must not be empty");
    if (snapshot.schema_version == 0)
        throw std::invalid_argument("dataset schema_version must be positive");
    if (snapshot.period_start_utc.empty() || snapshot.period_end_utc.empty())
        throw std::invalid_argument("dataset UTC period must be explicit");
    if (snapshot.event_ordering_rule.empty()
        || snapshot.tie_breaking_rule.empty() || snapshot.warmup.empty())
        throw std::invalid_argument(
            "dataset ordering, tie-breaking, and warmup metadata are required");
    if (snapshot.artifacts.empty())
        throw std::invalid_argument("dataset must contain at least one artifact");

    std::string previous;
    for (const auto& artifact : snapshot.artifacts)
    {
        if (artifact.logical_name.empty() || artifact.locator.empty())
            throw std::invalid_argument(
                "dataset artifact logical_name and locator are required");
        if (!is_lower_hex_sha256(artifact.sha256))
            throw std::invalid_argument("dataset artifact has invalid SHA-256");
        if (!previous.empty() && artifact.logical_name <= previous)
            throw std::invalid_argument(
                "dataset artifacts must have unique sorted logical names");
        previous = artifact.logical_name;
    }
    if (!is_lower_hex_sha256(snapshot.aggregate_sha256)
        || snapshot.aggregate_sha256 != dataset_aggregate_sha256(snapshot))
        throw std::invalid_argument("dataset aggregate SHA-256 mismatch");
}

[[nodiscard]] CanonicalJsonValue string_map_json(
    const std::map<std::string, std::string, std::less<>>& values)
{
    CanonicalJsonValue::Object object;
    for (const auto& [key, value] : values)
        object.emplace(key, value);
    return object;
}

[[nodiscard]] std::map<std::string, std::string, std::less<>>
string_map_from_json(const CanonicalJsonValue& value, std::string_view context)
{
    std::map<std::string, std::string, std::less<>> result;
    for (const auto& [key, child] : require_object(value, context))
    {
        if (key.empty() || !child.is_string())
            throw std::invalid_argument(std::string(context)
                + " must map nonempty names to strings");
        result.emplace(key, child.as_string());
    }
    return result;
}

void validate_versioned_model(const CanonicalJsonValue& value,
                              std::string_view field)
{
    (void)require_object(value, field);
    (void)require_string(value, "id");
    (void)require_string(value, "version");
    (void)require_object(value.at("config"), "config");
}

} // namespace

void validate_deterministic_runtime_environment()
{
    for (const std::string_view variable : {
             "LD_PRELOAD", "LD_AUDIT", "LD_LIBRARY_PATH",
             "LD_HWCAP_MASK", "GLIBC_TUNABLES"})
    {
        const char* value = std::getenv(std::string(variable).c_str());
        if (value != nullptr && value[0] != '\0')
            throw std::runtime_error(
                "deterministic runtime forbids loader/tunable environment '"
                + std::string(variable) + "'");
    }

    if (std::fegetround() != FE_TONEAREST)
        throw std::runtime_error(
            "deterministic runtime requires FE_TONEAREST rounding");

#if defined(__x86_64__) || defined(__i386__)
    constexpr unsigned kMxcsrResultControlMask =
        0x0040U | 0x6000U | 0x8000U; // DAZ, rounding, FTZ
    if ((_mm_getcsr() & kMxcsrResultControlMask) != 0U)
        throw std::runtime_error(
            "deterministic runtime requires default x86 MXCSR rounding, "
            "denormal, and flush-to-zero control");

    unsigned short x87_control_word = 0;
    __asm__ __volatile__("fnstcw %0" : "=m"(x87_control_word));
    constexpr unsigned short kX87ResultControlMask = 0x0f00U;
    constexpr unsigned short kX87ExpectedControl = 0x0300U;
    if ((x87_control_word & kX87ResultControlMask) != kX87ExpectedControl)
        throw std::runtime_error(
            "deterministic runtime requires default x87 precision and "
            "rounding control");
#endif
    (void)runtime_library_identities();
}

BuildIdentity current_build_identity()
{
    validate_deterministic_runtime_environment();
    BuildIdentity result;
    result.git_commit_sha = TRUETEST_GIT_SHA;
    result.git_dirty = std::string_view(TRUETEST_GIT_DIRTY) == "dirty";
    result.worktree_diff_sha256 = TRUETEST_GIT_DIFF_SHA256;
    result.build_type = TRUETEST_BUILD_TYPE;
    result.cmake_profile = TRUETEST_BUILD_PROFILE;
    result.build_flags = TRUETEST_BUILD_FLAGS;
    result.compiler = TRUETEST_CXX_COMPILER;
    result.standard_library = standard_library_identity();
    result.libc = libc_identity();
    result.architecture = architecture_identity();
    result.march = TRUETEST_MARCH;
    result.floating_point_flags = TRUETEST_FP_FLAGS;
    result.executable_sha256 = current_executable_sha256();
    result.dependencies = {
        {"abseil", TRUETEST_DEP_ABSEIL},
        {"boost", TRUETEST_DEP_BOOST},
        {"cli11", TRUETEST_DEP_CLI11},
        {"gtest", TRUETEST_DEP_GTEST},
        {"nlohmann-json", TRUETEST_DEP_NLOHMANN},
        {"openssl", TRUETEST_DEP_OPENSSL},
        {"zstd", TRUETEST_DEP_ZSTD},
    };
    const auto& runtime = runtime_library_identities();
    result.dependencies.insert(runtime.begin(), runtime.end());
    result.container_digest = TRUETEST_CONTAINER_DIGEST;
    return result;
}

CanonicalJsonValue build_identity_json(const BuildIdentity& build)
{
    return CanonicalJsonValue::object({
        {"architecture", build.architecture},
        {"build_flags", build.build_flags},
        {"build_type", build.build_type},
        {"cmake_profile", build.cmake_profile},
        {"compiler", build.compiler},
        {"container_digest", build.container_digest},
        {"dependencies", string_map_json(build.dependencies)},
        {"floating_point_flags", build.floating_point_flags},
        {"executable_sha256", build.executable_sha256},
        {"git_commit_sha", build.git_commit_sha},
        {"git_dirty", build.git_dirty},
        {"libc", build.libc},
        {"march", build.march},
        {"standard_library", build.standard_library},
        {"worktree_diff_sha256", build.worktree_diff_sha256},
    });
}

BuildIdentity build_identity_from_json(const CanonicalJsonValue& value)
{
    (void)require_object(value, "build");
    BuildIdentity result;
    result.git_commit_sha = require_string(value, "git_commit_sha");
    const auto& dirty = value.at("git_dirty");
    if (!dirty.is_bool())
        throw std::invalid_argument("build.git_dirty must be boolean");
    result.git_dirty = dirty.as_bool();
    result.worktree_diff_sha256 = require_string(
        value, "worktree_diff_sha256");
    if (result.git_dirty
        && !is_lower_hex_sha256(result.worktree_diff_sha256))
        throw std::invalid_argument(
            "dirty build requires a valid worktree diff SHA-256");
    if (!result.git_dirty && result.worktree_diff_sha256 != "none")
        throw std::invalid_argument(
            "clean build must use worktree_diff_sha256='none'");
    result.build_type = require_string(value, "build_type");
    result.cmake_profile = require_string(value, "cmake_profile");
    result.build_flags = require_string(value, "build_flags");
    result.compiler = require_string(value, "compiler");
    result.standard_library = require_string(value, "standard_library");
    result.libc = require_string(value, "libc");
    result.architecture = require_string(value, "architecture");
    result.march = require_string(value, "march");
    result.floating_point_flags = require_string(
        value, "floating_point_flags");
    result.executable_sha256 = require_string(value, "executable_sha256");
    if (!is_lower_hex_sha256(result.executable_sha256))
        throw std::invalid_argument(
            "build.executable_sha256 must be a lower-case SHA-256");
    result.dependencies = string_map_from_json(
        value.at("dependencies"), "build dependencies");
    result.container_digest = require_string(value, "container_digest");
    return result;
}

DatasetSnapshot snapshot_dataset(
    std::string dataset_id,
    std::uint64_t schema_version,
    std::vector<DatasetInput> inputs,
    std::string period_start_utc,
    std::string period_end_utc,
    std::string event_ordering_rule,
    std::string tie_breaking_rule,
    std::string warmup)
{
    if (inputs.empty())
        throw std::invalid_argument("dataset input list must not be empty");
    std::sort(inputs.begin(), inputs.end(),
              [](const DatasetInput& lhs, const DatasetInput& rhs) {
                  return lhs.logical_name < rhs.logical_name;
              });

    DatasetSnapshot result;
    result.dataset_id = std::move(dataset_id);
    result.schema_version = schema_version;
    result.period_start_utc = std::move(period_start_utc);
    result.period_end_utc = std::move(period_end_utc);
    result.event_ordering_rule = std::move(event_ordering_rule);
    result.tie_breaking_rule = std::move(tie_breaking_rule);
    result.warmup = std::move(warmup);
    result.artifacts.reserve(inputs.size());

    for (const auto& input : inputs)
    {
        if (input.logical_name.empty())
            throw std::invalid_argument("dataset logical name must not be empty");
        std::error_code error;
        if (!std::filesystem::is_regular_file(input.locator, error) || error)
            throw std::invalid_argument("dataset input is missing or not a file: "
                + input.locator.string());
        const auto size = std::filesystem::file_size(input.locator, error);
        if (error)
            throw std::runtime_error("cannot stat dataset input: "
                + input.locator.string());
        result.artifacts.push_back({
            input.logical_name,
            input.locator.generic_string(),
            static_cast<std::uint64_t>(size),
            sha256_file_hex(input.locator),
        });
    }
    result.aggregate_sha256 = dataset_aggregate_sha256(result);
    validate_dataset_metadata(result);
    return result;
}

DatasetSnapshot snapshot_embedded_dataset(
    std::string dataset_id,
    std::uint64_t schema_version,
    std::string logical_name,
    std::string_view canonical_descriptor,
    std::string period_start_utc,
    std::string period_end_utc,
    std::string event_ordering_rule,
    std::string tie_breaking_rule,
    std::string warmup)
{
    if (logical_name.empty() || canonical_descriptor.empty())
        throw std::invalid_argument(
            "embedded dataset requires a logical name and descriptor");
    DatasetSnapshot result;
    result.dataset_id = std::move(dataset_id);
    result.schema_version = schema_version;
    result.period_start_utc = std::move(period_start_utc);
    result.period_end_utc = std::move(period_end_utc);
    result.event_ordering_rule = std::move(event_ordering_rule);
    result.tie_breaking_rule = std::move(tie_breaking_rule);
    result.warmup = std::move(warmup);
    const std::string descriptor_hash = sha256_hex(canonical_descriptor);
    result.artifacts.push_back({
        std::move(logical_name), "embedded-sha256:" + descriptor_hash,
        static_cast<std::uint64_t>(canonical_descriptor.size()),
        descriptor_hash,
    });
    result.aggregate_sha256 = dataset_aggregate_sha256(result);
    validate_dataset_metadata(result);
    return result;
}

CanonicalJsonValue dataset_identity_json(const DatasetSnapshot& snapshot)
{
    validate_dataset_metadata(snapshot);
    return CanonicalJsonValue::object({
        {"aggregate_sha256", snapshot.aggregate_sha256},
        {"artifacts", dataset_descriptor_array(snapshot)},
        {"dataset_id", snapshot.dataset_id},
        {"event_ordering_rule", snapshot.event_ordering_rule},
        {"period_end_utc", snapshot.period_end_utc},
        {"period_start_utc", snapshot.period_start_utc},
        {"schema_version", snapshot.schema_version},
        {"tie_breaking_rule", snapshot.tie_breaking_rule},
        {"warmup", snapshot.warmup},
    });
}

CanonicalJsonValue dataset_locations_json(const DatasetSnapshot& snapshot)
{
    CanonicalJsonValue::Object locations;
    for (const auto& artifact : snapshot.artifacts)
        locations.emplace(artifact.logical_name, artifact.locator);
    return locations;
}

DatasetSnapshot dataset_snapshot_from_json(
    const CanonicalJsonValue& identity,
    const CanonicalJsonValue& locations)
{
    (void)require_object(identity, "dataset");
    const auto& locator_object = require_object(locations, "dataset_locations");
    DatasetSnapshot result;
    result.dataset_id = require_string(identity, "dataset_id");
    result.schema_version = identity.at("schema_version").as_u64();
    result.period_start_utc = require_string(identity, "period_start_utc");
    result.period_end_utc = require_string(identity, "period_end_utc");
    result.event_ordering_rule = require_string(identity, "event_ordering_rule");
    result.tie_breaking_rule = require_string(identity, "tie_breaking_rule");
    result.warmup = require_string(identity, "warmup");
    result.aggregate_sha256 = require_string(identity, "aggregate_sha256");

    const auto& artifacts = require_array(identity.at("artifacts"),
                                          "dataset artifacts");
    result.artifacts.reserve(artifacts.size());
    for (const auto& artifact_value : artifacts)
    {
        (void)require_object(artifact_value, "dataset artifact");
        DatasetArtifact artifact;
        artifact.logical_name = require_string(artifact_value, "logical_name");
        artifact.size_bytes = artifact_value.at("size_bytes").as_u64();
        artifact.sha256 = require_string(artifact_value, "sha256");
        const auto locator = locator_object.find(artifact.logical_name);
        if (locator == locator_object.end() || !locator->second.is_string()
            || locator->second.as_string().empty())
            throw std::invalid_argument(
                "dataset artifact lacks a nonempty locator: "
                + artifact.logical_name);
        artifact.locator = locator->second.as_string();
        result.artifacts.push_back(std::move(artifact));
    }
    if (locator_object.size() != result.artifacts.size())
        throw std::invalid_argument(
            "dataset_locations contains an unknown or duplicate logical artifact");
    validate_dataset_metadata(result);
    return result;
}

DatasetVerification verify_dataset(
    const DatasetSnapshot& snapshot,
    const std::filesystem::path& manifest_directory)
{
    validate_dataset_metadata(snapshot);
    DatasetVerification result;
    for (const auto& artifact : snapshot.artifacts)
    {
        constexpr std::string_view embedded_prefix = "embedded-sha256:";
        if (artifact.locator.starts_with(embedded_prefix))
        {
            if (std::string_view(artifact.locator).substr(
                    embedded_prefix.size()) != artifact.sha256)
                result.mismatches.push_back(
                    artifact.logical_name + ": embedded descriptor mismatch");
            continue;
        }
        std::filesystem::path path(artifact.locator);
        if (path.is_relative() && !manifest_directory.empty())
            path = manifest_directory / path;

        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
        {
            result.mismatches.push_back(
                artifact.logical_name + ": file missing");
            continue;
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error || static_cast<std::uint64_t>(size) != artifact.size_bytes)
        {
            result.mismatches.push_back(
                artifact.logical_name + ": size mismatch");
            continue;
        }
        try
        {
            const std::string actual = sha256_file_hex(path);
            if (actual != artifact.sha256)
                result.mismatches.push_back(
                    artifact.logical_name + ": SHA-256 mismatch");
        }
        catch (const std::exception& error_message)
        {
            result.mismatches.push_back(
                artifact.logical_name + ": " + error_message.what());
        }
    }
    result.exact = result.mismatches.empty();
    return result;
}

RunManifestV1::RunManifestV1(
    CanonicalJsonValue deterministic_inputs,
    CanonicalJsonValue dataset_locations,
    bool exact_lifecycle_replayable,
    std::map<std::string, std::string, std::less<>> expected_hashes)
    : deterministic_inputs_(std::move(deterministic_inputs)),
      dataset_locations_(std::move(dataset_locations)),
      exact_lifecycle_replayable_(exact_lifecycle_replayable),
      expected_hashes_(std::move(expected_hashes))
{
    validate();
}

void RunManifestV1::validate() const
{
    (void)require_object(deterministic_inputs_, "deterministic_inputs");
    (void)build_identity_from_json(deterministic_inputs_.at("build"));
    (void)dataset_snapshot_from_json(
        deterministic_inputs_.at("dataset"), dataset_locations_);
    (void)require_object(deterministic_inputs_.at("instrument"), "instrument");
    (void)require_object(deterministic_inputs_.at("fee_schedule"), "fee_schedule");
    validate_versioned_model(deterministic_inputs_.at("strategy"), "strategy");
    (void)require_object(deterministic_inputs_.at("effective_config"),
                         "effective_config");
    (void)require_string(deterministic_inputs_, "target");
    const std::string run_mode = require_string(
        deterministic_inputs_, "run_mode");
    (void)require_object(deterministic_inputs_.at("monte_carlo"),
                         "monte_carlo");
    if (run_mode != "backtest" && run_mode != "monte_carlo")
        throw std::invalid_argument(
            "run manifest has an unsupported run mode");
    if (run_mode == "monte_carlo")
    {
        const std::uint64_t trial_count = deterministic_inputs_
            .at("monte_carlo").at("trial_count").as_u64();
        if (trial_count == 0
            || trial_count > kRunManifestV1MaxMonteCarloTrials)
            throw std::invalid_argument(
                "run manifest Monte Carlo trial count is outside the schema-v1 supported range");
    }
    (void)require_object(deterministic_inputs_.at("threading"), "threading");
    (void)require_object(deterministic_inputs_.at("determinism_envelope"),
                         "determinism_envelope");

    const auto& seeds = deterministic_inputs_.at("seed_hierarchy");
    (void)require_object(seeds, "seed_hierarchy");
    (void)seeds.at("master_seed").as_u64();
    if (seeds.at("derivation_version").as_u64() != kSeedDerivationVersion)
        throw std::invalid_argument("unsupported seed derivation version");
    (void)require_object(seeds.at("domains"), "seed domains");
    (void)require_array(seeds.at("trials"), "trial seeds");

    const auto& models = deterministic_inputs_.at("models");
    (void)require_object(models, "models");
    for (const std::string_view name : {
             "simulator", "queue", "fill", "latency", "impact", "fee",
             "synthetic_gbm", "synthetic_l2"})
        validate_versioned_model(models.at(name), name);

    for (const auto& [kind, hash] : expected_hashes_)
    {
        if (kind.empty() || !is_lower_hex_sha256(hash))
            throw std::invalid_argument(
                "expected output hashes require nonempty names and lowercase SHA-256 values");
    }
}

void RunManifestV1::validate_publishable() const
{
    const auto require_hash = [&](std::string_view kind) {
        const auto found = expected_hashes_.find(kind);
        if (found == expected_hashes_.end()
            || !is_lower_hex_sha256(found->second))
            throw std::invalid_argument(
                "exact lifecycle manifest lacks expected output hash: "
                + std::string(kind));
    };
    const std::string run_mode = require_string(
        deterministic_inputs_, "run_mode");
    std::set<std::string, std::less<>> allowed_hashes{
        "event_log", "economic_result", "report"};
    if (run_mode == "backtest")
    {
        allowed_hashes.emplace("lifecycle");
    }
    else if (run_mode == "monte_carlo")
    {
        const std::uint64_t trial_count = deterministic_inputs_
            .at("monte_carlo").at("trial_count").as_u64();
        const auto& synthetic_l2_enabled = deterministic_inputs_
            .at("models").at("synthetic_l2").at("config").at("enabled");
        if (!synthetic_l2_enabled.is_bool())
            throw std::invalid_argument(
                "Monte Carlo synthetic-L2 enabled flag must be boolean");
        for (std::uint64_t trial = 0; trial < trial_count; ++trial)
        {
            const std::string prefix = "trial_" + std::to_string(trial) + ".";
            allowed_hashes.emplace(prefix + "event_log");
            allowed_hashes.emplace(prefix + "lifecycle");
            allowed_hashes.emplace(prefix + "result");
            if (synthetic_l2_enabled.as_bool())
                allowed_hashes.emplace(prefix + "synthetic_l2");
        }
    }

    for (const auto& [kind, hash] : expected_hashes_)
    {
        (void)hash;
        if (!allowed_hashes.contains(kind))
            throw std::invalid_argument(
                "run manifest contains an unsupported expected output hash: "
                + kind);
    }
    if (!exact_lifecycle_replayable_)
        return;
    for (const auto& kind : allowed_hashes)
        require_hash(kind);
    if (expected_hashes_.size() != allowed_hashes.size())
        throw std::invalid_argument(
            "exact lifecycle manifest expected hash set is not exact");
}

std::string RunManifestV1::run_fingerprint() const
{
    return sha256_hex(serialize_canonical_json(deterministic_inputs_));
}

std::string RunManifestV1::serialize() const
{
    validate();
    validate_publishable();
    return serialize_canonical_json(CanonicalJsonValue::object({
        {"dataset_locations", dataset_locations_},
        {"deterministic_inputs", deterministic_inputs_},
        {"exact_lifecycle_replayable", exact_lifecycle_replayable_},
        {"expected_hashes", string_map_json(expected_hashes_)},
        {"manifest_schema_version", kRunManifestSchemaVersion},
        {"run_fingerprint", run_fingerprint()},
    }));
}

RunManifestV1 RunManifestV1::parse(std::string_view bytes)
{
    const CanonicalJsonValue root = parse_json_strict(bytes);
    require_exact_object_keys(root,
        {"dataset_locations", "deterministic_inputs",
         "exact_lifecycle_replayable", "expected_hashes",
         "manifest_schema_version", "run_fingerprint"},
        "run manifest");
    if (root.at("manifest_schema_version").as_u64()
        != kRunManifestSchemaVersion)
        throw std::invalid_argument("unsupported run manifest schema version");
    if (!root.at("exact_lifecycle_replayable").is_bool())
        throw std::invalid_argument(
            "exact_lifecycle_replayable must be boolean");

    RunManifestV1 result(
        root.at("deterministic_inputs"), root.at("dataset_locations"),
        root.at("exact_lifecycle_replayable").as_bool(),
        string_map_from_json(root.at("expected_hashes"), "expected_hashes"));
    const std::string stored_fingerprint = require_string(
        root, "run_fingerprint");
    if (!is_lower_hex_sha256(stored_fingerprint)
        || stored_fingerprint != result.run_fingerprint())
        throw std::invalid_argument("run manifest fingerprint mismatch");
    result.validate_publishable();
    return result;
}

RunManifestV1 RunManifestV1::load(const std::filesystem::path& path)
{
    return parse(read_text_file(path, kRunManifestV1MaxBytes));
}

void RunManifestV1::write_atomic(const std::filesystem::path& path,
                                 bool replace_existing) const
{
    write_text_file_atomic(path, serialize(), replace_existing);
}

void RunManifestV1::set_expected_hash(std::string kind, std::string hash)
{
    if (kind.empty() || !is_lower_hex_sha256(hash))
        throw std::invalid_argument("invalid expected output hash");
    expected_hashes_.insert_or_assign(std::move(kind), std::move(hash));
}

std::string RunReceiptV1::serialize() const
{
    CanonicalJsonValue::Array mismatch_values;
    mismatch_values.reserve(mismatches.size());
    for (const auto& mismatch : mismatches)
        mismatch_values.emplace_back(mismatch);
    const CanonicalJsonValue serialized_trial_index = trial_index
        ? CanonicalJsonValue(*trial_index) : CanonicalJsonValue(nullptr);
    return serialize_canonical_json(CanonicalJsonValue::object({
        {"actual_hashes", string_map_json(actual_hashes)},
        {"dataset_mismatch_override_used", dataset_mismatch_override_used},
        {"duration_ms", duration_ms},
        {"exact_reproduction", exact_reproduction},
        {"executed_at_utc", executed_at_utc},
        {"mismatches", std::move(mismatch_values)},
        {"receipt_schema_version", std::uint64_t{2}},
        {"run_fingerprint", run_fingerprint},
        {"status", status},
        {"trial_index", serialized_trial_index},
        {"verification_scope", verification_scope},
    }));
}

void RunReceiptV1::write_atomic(const std::filesystem::path& path,
                                bool replace_existing) const
{
    write_text_file_atomic(path, serialize(), replace_existing);
}

std::string read_text_file(const std::filesystem::path& path,
                           std::uint64_t max_bytes)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        throw std::runtime_error("cannot stat file: " + path.string());
    if (static_cast<std::uint64_t>(size) > max_bytes)
        throw std::runtime_error("file exceeds configured read limit: "
            + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open file: " + path.string());
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty())
    {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
            throw std::runtime_error("short read from file: " + path.string());
    }
    return bytes;
}

void write_text_file_atomic(const std::filesystem::path& path,
                            std::string_view bytes,
                            bool replace_existing)
{
    if (path.empty() || path.filename().empty())
        throw std::invalid_argument("artifact path must name a file");
    std::error_code error;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            throw std::runtime_error("cannot create artifact directory: "
                + path.parent_path().string());
    }
    if (!replace_existing && std::filesystem::exists(path, error))
        throw std::runtime_error("refusing to overwrite artifact: "
            + path.string());
    if (error)
        throw std::runtime_error("cannot inspect artifact destination: "
            + path.string());

    std::filesystem::path partial = path;
    partial += ".partial";
    if (std::filesystem::exists(partial, error))
        throw std::runtime_error("incomplete artifact already exists: "
            + partial.string());
    if (error)
        throw std::runtime_error("cannot inspect artifact staging path: "
            + partial.string());

    {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot create artifact staging file: "
                + partial.string());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
            throw std::runtime_error("cannot finalize artifact staging file: "
                + partial.string());
    }
    std::filesystem::rename(partial, path, error);
    if (error)
        throw std::runtime_error("cannot atomically publish artifact: "
            + path.string() + ": " + error.message());
}

bool is_lower_hex_sha256(std::string_view value) noexcept
{
    if (value.size() != 64)
        return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace truetest::reproducibility
