#pragma once

#include "monte_carlo_types.h"
#include "reproducibility/canonical_json.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace truetest::simulation {

inline constexpr std::uint64_t kTrialResultSchemaVersion = 1;
inline constexpr std::uint64_t kEconomicResultSchemaVersion = 1;

struct TrialArtifactPaths
{
    std::filesystem::path directory;
    std::filesystem::path event_log;
    std::filesystem::path event_log_partial;
    std::filesystem::path lifecycle;
    std::filesystem::path synthetic_l2;
    std::filesystem::path synthetic_l2_partial;
    std::filesystem::path result;
    std::filesystem::path manifest;
};

[[nodiscard]] TrialArtifactPaths trial_artifact_paths(
    const McRunConfig& config, std::size_t trial_index);

void prepare_trial_artifacts(const TrialArtifactPaths& paths);
void finalize_trial_event_log(const TrialArtifactPaths& paths);
void write_trial_synthetic_l2_partial(
    const TrialArtifactPaths& paths,
    const std::vector<provider::l2_snapshot>& snapshots);
void finalize_trial_synthetic_l2(const TrialArtifactPaths& paths);

[[nodiscard]] reproducibility::CanonicalJsonValue trial_result_payload(
    const TrialResult& result);
[[nodiscard]] std::string trial_result_sha256(const TrialResult& result);
[[nodiscard]] reproducibility::CanonicalJsonValue effective_trial_config_json(
    const McRunConfig& config);
[[nodiscard]] reproducibility::CanonicalJsonValue component_seed_json(
    std::uint64_t master_seed, std::size_t trial_index);

void write_completed_trial_artifacts(const McRunConfig& config,
                                     const TrialArtifactPaths& paths,
                                     const TrialResult& result);
void write_failed_trial_artifact(const McRunConfig& config,
                                 std::size_t trial_index,
                                 std::string_view error);

[[nodiscard]] std::string economic_result_sha256(const McAggregate& aggregate);
[[nodiscard]] reproducibility::CanonicalJsonValue economic_result_payload(
    const McAggregate& aggregate);
[[nodiscard]] std::string aggregate_event_log_sha256(
    const McAggregate& aggregate);
[[nodiscard]] std::string deterministic_report_sha256(
    const McAggregate& aggregate, const McRunConfig& config);
[[nodiscard]] std::string deterministic_report_json(
    const McAggregate& aggregate, const McRunConfig& config);

} // namespace truetest::simulation
