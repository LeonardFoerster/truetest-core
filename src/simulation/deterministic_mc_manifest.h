#pragma once

#include "monte_carlo_types.h"
#include "reproducibility/run_manifest.h"
#include "strategy/strategy_interface.h"

#include <filesystem>
#include <string>
#include <vector>

namespace truetest::simulation {

// One cold-path application routine is shared by manifest construction and
// trial execution so the hashed effective strategy state cannot drift from
// the state actually used by the engine.
void configure_deterministic_mc_strategy(IStrategy& strategy,
                                         const McRunConfig& config);

[[nodiscard]] reproducibility::RunManifestV1 make_deterministic_mc_manifest(
    const McRunConfig& config);

// Reconstructs every effective model/strategy input needed by the existing
// controller. Output locations are deliberately supplied by the replaying
// invocation and therefore remain outside the run fingerprint.
[[nodiscard]] McRunConfig mc_config_from_manifest(
    const reproducibility::RunManifestV1& manifest,
    const std::filesystem::path& artifacts_directory);

struct ManifestEnvironmentVerification
{
    bool exact = true;
    bool build_exact = true;
    bool dataset_exact = true;
    std::vector<std::string> mismatches;
};

[[nodiscard]] ManifestEnvironmentVerification verify_mc_manifest_environment(
    const reproducibility::RunManifestV1& manifest,
    const std::filesystem::path& manifest_directory);

} // namespace truetest::simulation
