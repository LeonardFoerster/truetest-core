#pragma once

#include "types/public_trade.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// footprint.md §2.2: versioned zstd-compressed raw-trade segments + a
// manifest, both updated through atomic replacement, with corrupt segments
// quarantined rather than partially trusted. Pure cold-path file I/O - no
// hot-path interaction, no engine dependency. FootprintReconciler
// (footprint_reconciler.h) is this module's consumer: a cold worker reads
// segments here and hands the decoded trades to
// FootprintReconciler::load_cache().
namespace truetest::footprint {

inline constexpr std::uint32_t kSegmentSchemaVersion = 1;

// Segment persistence is cold-path work, but it reads data that may have
// been damaged or supplied by an operator. Keep a deliberately small default
// memory budget: the reader's newly allocated staging is one compressed buffer
// plus one decoded PublicTrade vector. Existing caller-owned output and Zstd
// internals add to the process peak. Callers can explicitly raise the limit
// for a known workload, but never beyond the compiled hard ceiling.
inline constexpr std::size_t kDefaultMaxSegmentUncompressedBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kHardMaxSegmentUncompressedBytes = 32U * 1024U * 1024U;

struct SegmentReadLimits
{
    std::size_t max_uncompressed_bytes = kDefaultMaxSegmentUncompressedBytes;
};

struct SegmentMetadata
{
    std::uint32_t schema_version = kSegmentSchemaVersion;
    std::uint16_t venue_id = 0;
    std::uint16_t symbol_id = 0;
    std::int64_t time_range_start_ns = 0;
    std::int64_t time_range_end_ns = 0;
    std::uint64_t record_count = 0;
    // 0 when the segment has no trades of that identity kind at all.
    std::uint64_t native_id_min = 0;
    std::uint64_t native_id_max = 0;
    std::uint64_t session_id_min = 0;
    std::uint64_t session_id_max = 0;
    std::uint64_t checksum = 0; // FNV-1a 64 over the uncompressed PublicTrade bytes
};

enum class segment_read_status : std::uint8_t
{
    ok,
    not_found,
    truncated,
    bad_magic,
    schema_mismatch,
    checksum_mismatch,
    resource_limit,
    trailing_data,
};

enum class segment_append_status : std::uint8_t
{
    appended,
    full,
    trade_too_large,
    identity_mismatch,
    resource_exhausted,
};

// Accumulates trades for one rolling segment (footprint.md "rolling every
// five minutes or at a fixed maximum uncompressed size". Only `full` means
// the caller should finalize and retry the same whole trade. The other
// non-appended results are fail-closed: they leave state unchanged and must be
// surfaced rather than causing an empty-segment retry loop or silent loss.
class FootprintSegmentWriter
{
public:
    FootprintSegmentWriter(std::uint16_t venue_id, std::uint16_t symbol_id,
                           std::size_t max_uncompressed_bytes = kDefaultMaxSegmentUncompressedBytes);

    // A venue trade is never split between segments, matching the
    // aggregation rule elsewhere - append() only ever adds a whole trade.
    [[nodiscard]] segment_append_status append(const PublicTrade& trade) noexcept;

    std::size_t buffered_count() const noexcept { return trades_.size(); }
    std::size_t max_buffered_count() const noexcept { return max_buffered_trades_; }
    bool empty() const noexcept { return trades_.empty(); }

    // Writes the compressed payload + metadata to `<finalized_path>.partial`
    // (a "recoverable partial segment" - if the process dies mid-write, the
    // ".partial" file is inert and never mistaken for a finalized one),
    // then atomically renames it to `finalized_path` on success. Returns
    // nullopt (and leaves no finalized file, though a `.partial` may
    // remain for forensics) on any I/O failure.
    std::optional<SegmentMetadata> finalize(const std::filesystem::path& finalized_path);

private:
    std::uint16_t venue_id_;
    std::uint16_t symbol_id_;
    std::size_t max_buffered_trades_ = 0;
    std::vector<PublicTrade> trades_;
};

// Reads and verifies a finalized segment file (magic, schema version,
// checksum). Never returns partial contents on any mismatch - the caller
// is expected to call quarantine_segment() and treat it as a fault
// (footprint.md's "corrupt segment" reconciliation trigger).
segment_read_status read_segment(const std::filesystem::path& path,
                                  SegmentMetadata& out_meta,
                                  std::vector<PublicTrade>& out_trades);

// Limit-aware overload for low-memory callers and deterministic tests. The
// effective limit is clamped to kHardMaxSegmentUncompressedBytes, so a caller
// cannot accidentally reintroduce an unbounded reader by supplying a huge
// value. Outputs are changed only after the entire segment has verified.
segment_read_status read_segment(const std::filesystem::path& path,
                                  SegmentMetadata& out_meta,
                                  std::vector<PublicTrade>& out_trades,
                                  SegmentReadLimits limits);

// Moves a corrupt/unreadable segment aside to `<path>.quarantined` (never
// deleted outright - "report them instead of accepting partial contents").
// Returns the quarantine path, or nullopt if the move itself failed.
std::optional<std::filesystem::path> quarantine_segment(const std::filesystem::path& path);

// One finalized segment's manifest entry - metadata plus where it lives
// and when it was last read (for LRU retention).
struct ManifestEntry
{
    SegmentMetadata meta;
    std::string filename; // relative to the manifest's own directory
    std::int64_t last_access_ns = 0;
};

// footprint.md §2.2: "finalize and update the manifest through atomic
// replacement... Enforce seven-day retention, 2 GiB per venue/symbol...
// Evict least-recently-used finalized segments, never the active segment
// or current visible range." One manifest instance covers a single
// (venue, symbol) key's directory - the 8 GiB *global* cap is a
// cross-key orchestration concern layered on top (a future cold-worker
// sums per-key manifests' on-disk bytes and asks the least-recently-used
// ones across ALL keys to evict first); not implemented in this class.
class FootprintSegmentManifest
{
public:
    explicit FootprintSegmentManifest(std::filesystem::path directory);

    // Loads `<directory>/manifest.txt` if present; a missing file means an
    // empty (fresh) manifest, not an error.
    bool load();

    // Adds a newly finalized segment and atomically rewrites the manifest
    // file (write to a temp file, then rename over the old one).
    bool add(const SegmentMetadata& meta, const std::string& filename, std::int64_t now_ns);

    const std::vector<ManifestEntry>& entries() const noexcept { return entries_; }

    // Segments whose [time_range_start_ns, time_range_end_ns) overlaps
    // [query_start_ns, query_end_ns).
    std::vector<ManifestEntry> segments_covering(std::int64_t query_start_ns,
                                                 std::int64_t query_end_ns) const;

    // footprint.md retention: removes (from the manifest AND the
    // filesystem) any finalized segment older than max_age_ns, then evicts
    // least-recently-used finalized segments beyond max_total_bytes -
    // never the single most recent segment (the "active"/current-visible
    // one), regardless of age or size pressure. Returns the filenames
    // actually evicted. `now_ns`/`segment_byte_size` let tests avoid real
    // wall-clock/filesystem-size dependencies.
    std::vector<std::string> enforce_retention(
        std::int64_t now_ns, std::int64_t max_age_ns, std::uint64_t max_total_bytes,
        std::function<std::uint64_t(const std::string&)> segment_byte_size);

private:
    bool save() const;

    std::filesystem::path directory_;
    std::vector<ManifestEntry> entries_;
};

} // namespace truetest::footprint
