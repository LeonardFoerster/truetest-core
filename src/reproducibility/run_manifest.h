#pragma once

#include "canonical_json.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace truetest::reproducibility {

inline constexpr std::uint64_t kRunManifestSchemaVersion = 1;
// Schema-v1 manifests intentionally cap the number of materialized trial
// identities. Raising this bound changes the accepted wire contract and must
// therefore accompany a manifest schema review rather than a runtime option.
inline constexpr std::uint64_t kRunManifestV1MaxMonteCarloTrials = 100'000;
// Full 100k-trial manifests include the complete seed hierarchy and up to four
// per-trial SHA-256 entries. This schema-specific bounded reader therefore
// exceeds the generic artifact limit while still refusing unbounded input.
inline constexpr std::uint64_t kRunManifestV1MaxBytes =
    128U * 1024U * 1024U;

struct BuildIdentity
{
    std::string git_commit_sha;
    bool git_dirty = false;
    std::string worktree_diff_sha256;
    std::string build_type;
    std::string cmake_profile;
    std::string build_flags;
    std::string compiler;
    std::string standard_library;
    std::string libc;
    std::string architecture;
    std::string march;
    std::string floating_point_flags;
    std::string executable_sha256;
    std::map<std::string, std::string, std::less<>> dependencies;
    std::string container_digest;

    friend bool operator==(const BuildIdentity&, const BuildIdentity&) = default;
};

[[nodiscard]] BuildIdentity current_build_identity();
// Reproducible runs reject loader interposition and non-default floating-point
// control rather than silently widening the determinism envelope.
void validate_deterministic_runtime_environment();
[[nodiscard]] CanonicalJsonValue build_identity_json(const BuildIdentity& build);
[[nodiscard]] BuildIdentity build_identity_from_json(const CanonicalJsonValue& value);

struct DatasetInput
{
    std::string logical_name;
    std::filesystem::path locator;
};

struct DatasetArtifact
{
    std::string logical_name;
    std::string locator;
    std::uint64_t size_bytes = 0;
    std::string sha256;

    friend bool operator==(const DatasetArtifact&,
                           const DatasetArtifact&) = default;
};

struct DatasetSnapshot
{
    std::string dataset_id;
    std::uint64_t schema_version = 0;
    std::string period_start_utc;
    std::string period_end_utc;
    std::string event_ordering_rule;
    std::string tie_breaking_rule;
    std::string warmup;
    std::vector<DatasetArtifact> artifacts;
    std::string aggregate_sha256;

    friend bool operator==(const DatasetSnapshot&,
                           const DatasetSnapshot&) = default;
};

struct DatasetVerification
{
    bool exact = true;
    std::vector<std::string> mismatches;
};

[[nodiscard]] DatasetSnapshot snapshot_dataset(
    std::string dataset_id,
    std::uint64_t schema_version,
    std::vector<DatasetInput> inputs,
    std::string period_start_utc,
    std::string period_end_utc,
    std::string event_ordering_rule,
    std::string tie_breaking_rule,
    std::string warmup);

// Synthetic/generated inputs have no mutable file locator. Their complete
// effective generator descriptor is hashed as an embedded dataset artifact;
// replay independently validates the same descriptor from the manifest.
[[nodiscard]] DatasetSnapshot snapshot_embedded_dataset(
    std::string dataset_id,
    std::uint64_t schema_version,
    std::string logical_name,
    std::string_view canonical_descriptor,
    std::string period_start_utc,
    std::string period_end_utc,
    std::string event_ordering_rule,
    std::string tie_breaking_rule,
    std::string warmup);

[[nodiscard]] CanonicalJsonValue dataset_identity_json(
    const DatasetSnapshot& snapshot);
[[nodiscard]] CanonicalJsonValue dataset_locations_json(
    const DatasetSnapshot& snapshot);
[[nodiscard]] DatasetSnapshot dataset_snapshot_from_json(
    const CanonicalJsonValue& identity,
    const CanonicalJsonValue& locations);
[[nodiscard]] DatasetVerification verify_dataset(
    const DatasetSnapshot& snapshot,
    const std::filesystem::path& manifest_directory = {});

class RunManifestV1 final
{
public:
    RunManifestV1(CanonicalJsonValue deterministic_inputs,
                  CanonicalJsonValue dataset_locations,
                  bool exact_lifecycle_replayable,
                  std::map<std::string, std::string, std::less<>> expected_hashes = {});

    [[nodiscard]] static RunManifestV1 parse(std::string_view bytes);
    [[nodiscard]] static RunManifestV1 load(const std::filesystem::path& path);

    [[nodiscard]] const CanonicalJsonValue& deterministic_inputs() const noexcept
    {
        return deterministic_inputs_;
    }
    [[nodiscard]] const CanonicalJsonValue& dataset_locations() const noexcept
    {
        return dataset_locations_;
    }
    [[nodiscard]] bool exact_lifecycle_replayable() const noexcept
    {
        return exact_lifecycle_replayable_;
    }
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>&
    expected_hashes() const noexcept
    {
        return expected_hashes_;
    }

    [[nodiscard]] std::string run_fingerprint() const;
    [[nodiscard]] std::string serialize() const;
    void write_atomic(const std::filesystem::path& path,
                      bool replace_existing = false) const;

    void set_expected_hash(std::string kind, std::string sha256);

private:
    void validate() const;
    void validate_publishable() const;

    CanonicalJsonValue deterministic_inputs_;
    CanonicalJsonValue dataset_locations_;
    bool exact_lifecycle_replayable_ = false;
    std::map<std::string, std::string, std::less<>> expected_hashes_;
};

struct RunReceiptV1
{
    std::string executed_at_utc;
    std::uint64_t duration_ms = 0;
    std::string status;
    bool dataset_mismatch_override_used = false;
    bool exact_reproduction = false;
    std::string run_fingerprint;
    std::string verification_scope = "run";
    std::optional<std::uint64_t> trial_index;
    std::map<std::string, std::string, std::less<>> actual_hashes;
    std::vector<std::string> mismatches;

    [[nodiscard]] std::string serialize() const;
    void write_atomic(const std::filesystem::path& path,
                      bool replace_existing = false) const;
};

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path,
                                         std::uint64_t max_bytes = 64U * 1024U * 1024U);
void write_text_file_atomic(const std::filesystem::path& path,
                            std::string_view bytes,
                            bool replace_existing = false);
[[nodiscard]] bool is_lower_hex_sha256(std::string_view value) noexcept;

} // namespace truetest::reproducibility
