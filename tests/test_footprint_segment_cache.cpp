// footprint.md §2.2: versioned zstd-compressed segments + a manifest, both
// updated through atomic replacement, corrupt segments quarantined rather
// than partially trusted, and retention/LRU eviction that never touches
// the active/most-recent segment.

#include <gtest/gtest.h>

#include "analytics/footprint/footprint_segment_cache.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <unistd.h>

using namespace truetest::footprint;

namespace {

constexpr std::int64_t kSecond = 1'000'000'000LL;

// RAII scratch directory - unique per test process invocation via a
// monotonic counter (Date.now()/random device use is fine here, this is a
// normal gtest binary, not a Workflow script).
class ScratchDir
{
public:
    ScratchDir()
    {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path()
              / ("footprint_segment_cache_test_" + std::to_string(::getpid())
                 + "_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

PublicTrade make_native(std::int64_t event_ns, std::uint64_t native_id)
{
    PublicTrade t;
    t.event_ns = event_ns;
    t.recv_ns = event_ns;
    t.native_trade_id = native_id;
    t.flags = provenance_native_id;
    t.price_ticks = 100;
    t.base_qty_atoms = 1;
    return t;
}

PublicTrade make_session(std::int64_t event_ns, std::uint64_t session_id, std::uint64_t obs_seq)
{
    PublicTrade t;
    t.event_ns = event_ns;
    t.recv_ns = event_ns;
    t.session_id = session_id;
    t.obs_seq = obs_seq;
    t.flags = provenance_session_only;
    t.price_ticks = 100;
    t.base_qty_atoms = 1;
    return t;
}

} // namespace

TEST(FootprintSegmentCache, WriteFinalizeReadRoundTrip)
{
    ScratchDir dir;
    const auto path = dir.path() / "seg1.bin";

    FootprintSegmentWriter writer(/*venue=*/1, /*symbol=*/2);
    writer.append(make_native(0, 10));
    writer.append(make_native(kSecond, 11));
    writer.append(make_native(2 * kSecond, 12));

    auto meta = writer.finalize(path);
    ASSERT_TRUE(meta.has_value());
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".partial")); // renamed away

    SegmentMetadata read_meta;
    std::vector<PublicTrade> read_trades;
    ASSERT_EQ(read_segment(path, read_meta, read_trades), segment_read_status::ok);

    EXPECT_EQ(read_trades.size(), 3u);
    EXPECT_EQ(read_trades[0].native_trade_id, 10u);
    EXPECT_EQ(read_trades[2].native_trade_id, 12u);
    EXPECT_EQ(read_meta.venue_id, 1u);
    EXPECT_EQ(read_meta.symbol_id, 2u);
    EXPECT_EQ(read_meta.record_count, 3u);
    EXPECT_EQ(read_meta.time_range_start_ns, 0);
    EXPECT_EQ(read_meta.time_range_end_ns, 2 * kSecond);
    EXPECT_EQ(read_meta.native_id_min, 10u);
    EXPECT_EQ(read_meta.native_id_max, 12u);
}

TEST(FootprintSegmentCache, FinalizeOnEmptyWriterProducesAnEmptyValidSegment)
{
    ScratchDir dir;
    const auto path = dir.path() / "empty.bin";
    FootprintSegmentWriter writer(1, 2);
    auto meta = writer.finalize(path);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->record_count, 0u);

    SegmentMetadata read_meta;
    std::vector<PublicTrade> read_trades;
    EXPECT_EQ(read_segment(path, read_meta, read_trades), segment_read_status::ok);
    EXPECT_TRUE(read_trades.empty());
}

TEST(FootprintSegmentCache, ReadNonexistentPathReturnsNotFound)
{
    ScratchDir dir;
    SegmentMetadata meta;
    std::vector<PublicTrade> trades;
    EXPECT_EQ(read_segment(dir.path() / "missing.bin", meta, trades),
              segment_read_status::not_found);
}

TEST(FootprintSegmentCache, CorruptedPayloadFailsChecksumNotPartiallyAccepted)
{
    ScratchDir dir;
    const auto path = dir.path() / "seg.bin";
    FootprintSegmentWriter writer(1, 1);
    writer.append(make_native(0, 1));
    writer.append(make_native(kSecond, 2));
    ASSERT_TRUE(writer.finalize(path).has_value());

    // Flip a byte near the end of the file (inside the compressed payload).
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekg(-1, std::ios::end);
        char c;
        f.read(&c, 1);
        c = static_cast<char>(~c);
        f.seekp(-1, std::ios::end);
        f.write(&c, 1);
    }

    SegmentMetadata meta;
    std::vector<PublicTrade> trades;
    const auto status = read_segment(path, meta, trades);
    // Either the zstd frame itself fails to decode, or it decodes to data
    // that fails the checksum - either way, never "ok" with wrong content.
    EXPECT_NE(status, segment_read_status::ok);
    EXPECT_TRUE(trades.empty());
}

TEST(FootprintSegmentCache, TruncatedFileIsDetected)
{
    ScratchDir dir;
    const auto path = dir.path() / "seg.bin";
    FootprintSegmentWriter writer(1, 1);
    writer.append(make_native(0, 1));
    ASSERT_TRUE(writer.finalize(path).has_value());

    const auto size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size / 2);

    SegmentMetadata meta;
    std::vector<PublicTrade> trades;
    EXPECT_EQ(read_segment(path, meta, trades), segment_read_status::truncated);
}

TEST(FootprintSegmentCache, BadMagicIsDetected)
{
    ScratchDir dir;
    const auto path = dir.path() / "seg.bin";
    FootprintSegmentWriter writer(1, 1);
    writer.append(make_native(0, 1));
    ASSERT_TRUE(writer.finalize(path).has_value());

    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.write("XXXXXXXX", 8);
    }

    SegmentMetadata meta;
    std::vector<PublicTrade> trades;
    EXPECT_EQ(read_segment(path, meta, trades), segment_read_status::bad_magic);
}

TEST(FootprintSegmentCache, SchemaVersionMismatchIsDetected)
{
    ScratchDir dir;
    const auto path = dir.path() / "seg.bin";
    FootprintSegmentWriter writer(1, 1);
    writer.append(make_native(0, 1));
    ASSERT_TRUE(writer.finalize(path).has_value());

    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(8, std::ios::beg); // schema_version field, right after the 8-byte magic
        std::uint32_t bogus_version = 999;
        f.write(reinterpret_cast<const char*>(&bogus_version), sizeof(bogus_version));
    }

    SegmentMetadata meta;
    std::vector<PublicTrade> trades;
    EXPECT_EQ(read_segment(path, meta, trades), segment_read_status::schema_mismatch);
}

TEST(FootprintSegmentCache, QuarantineMovesFileAsideAndReportsThePath)
{
    ScratchDir dir;
    const auto path = dir.path() / "seg.bin";
    FootprintSegmentWriter writer(1, 1);
    writer.append(make_native(0, 1));
    ASSERT_TRUE(writer.finalize(path).has_value());

    auto quarantined = quarantine_segment(path);
    ASSERT_TRUE(quarantined.has_value());
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(*quarantined));
}

// --- Regression: verifier-found bugs ---

TEST(FootprintSegmentCache, SessionIdMinReportsTrueZeroNotSentinelCollision)
{
    // A session-only venue's (Bitunix, §2.1) first session is legitimately
    // session_id=0. A naive "0 means unset" running-min would wrongly
    // latch onto 0 the moment it's seen and never update again even if a
    // LATER trade has a smaller session_id than the true minimum implies
    // here - the real regression was the reverse: once a real 0 appears,
    // it must stay reported as 0, not get overwritten by a subsequent
    // larger session_id.
    ScratchDir dir;
    const auto path = dir.path() / "seg.bin";
    FootprintSegmentWriter writer(1, 1);
    writer.append(make_session(0, /*session_id=*/0, /*obs_seq=*/0));
    writer.append(make_session(kSecond, /*session_id=*/1, /*obs_seq=*/0));
    ASSERT_TRUE(writer.finalize(path).has_value());

    SegmentMetadata meta;
    std::vector<PublicTrade> trades;
    ASSERT_EQ(read_segment(path, meta, trades), segment_read_status::ok);
    EXPECT_EQ(meta.session_id_min, 0u);
    EXPECT_EQ(meta.session_id_max, 1u);
}

TEST(FootprintSegmentCache, NativeIdMinReportsTrueZeroNotSentinelCollision)
{
    ScratchDir dir;
    const auto path = dir.path() / "seg.bin";
    FootprintSegmentWriter writer(1, 1);
    writer.append(make_native(0, /*native_id=*/0));
    writer.append(make_native(kSecond, /*native_id=*/5));
    ASSERT_TRUE(writer.finalize(path).has_value());

    SegmentMetadata meta;
    std::vector<PublicTrade> trades;
    ASSERT_EQ(read_segment(path, meta, trades), segment_read_status::ok);
    EXPECT_EQ(meta.native_id_min, 0u);
    EXPECT_EQ(meta.native_id_max, 5u);
}

TEST(FootprintSegmentCache, CorruptedRawSizeIsRejectedNotAllocatedBlindly)
{
    ScratchDir dir;
    const auto path = dir.path() / "seg.bin";
    FootprintSegmentWriter writer(1, 1);
    writer.append(make_native(0, 1));
    ASSERT_TRUE(writer.finalize(path).has_value());

    // raw_size is the first uint64 after the 11-field metadata block:
    // magic(8) + schema(4) + venue(2) + symbol(2) + start(8) + end(8) +
    // count(8) + native_min(8) + native_max(8) + session_min(8) +
    // session_max(8) + checksum(8) = 80 bytes in.
    constexpr std::streamoff kRawSizeOffset = 80;
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(kRawSizeOffset, std::ios::beg);
        std::uint64_t absurd_raw_size = std::numeric_limits<std::uint64_t>::max() / 2;
        f.write(reinterpret_cast<const char*>(&absurd_raw_size), sizeof(absurd_raw_size));
    }

    SegmentMetadata meta;
    std::vector<PublicTrade> trades;
    // Must fail gracefully (quarantine-worthy status), never crash/throw
    // trying to allocate for the corrupted size.
    EXPECT_NE(read_segment(path, meta, trades), segment_read_status::ok);
    EXPECT_TRUE(trades.empty());
}

// --- Manifest ---

TEST(FootprintSegmentManifest, AddPersistsAndLoadRoundTripsAcrossInstances)
{
    ScratchDir dir;
    {
        FootprintSegmentManifest m(dir.path());
        ASSERT_TRUE(m.load()); // no file yet - empty, not an error
        SegmentMetadata meta;
        meta.venue_id = 1;
        meta.symbol_id = 2;
        meta.time_range_start_ns = 0;
        meta.time_range_end_ns = kSecond;
        meta.record_count = 5;
        ASSERT_TRUE(m.add(meta, "seg_0.bin", /*now_ns=*/kSecond));
    }

    FootprintSegmentManifest reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.entries().size(), 1u);
    EXPECT_EQ(reloaded.entries()[0].filename, "seg_0.bin");
    EXPECT_EQ(reloaded.entries()[0].meta.record_count, 5u);
}

TEST(FootprintSegmentManifest, SegmentsCoveringFindsOverlappingRangesOnly)
{
    ScratchDir dir;
    FootprintSegmentManifest m(dir.path());
    m.load();

    SegmentMetadata a; a.time_range_start_ns = 0; a.time_range_end_ns = 10 * kSecond;
    SegmentMetadata b; b.time_range_start_ns = 10 * kSecond; b.time_range_end_ns = 20 * kSecond;
    SegmentMetadata c; c.time_range_start_ns = 100 * kSecond; c.time_range_end_ns = 110 * kSecond;
    m.add(a, "a.bin", 0);
    m.add(b, "b.bin", 0);
    m.add(c, "c.bin", 0);

    const auto hits = m.segments_covering(5 * kSecond, 15 * kSecond);
    std::vector<std::string> names;
    for (const auto& e : hits) names.push_back(e.filename);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"a.bin", "b.bin"}));
}

TEST(FootprintSegmentManifest, EnforceRetentionEvictsOldSegmentsButNeverTheMostRecent)
{
    ScratchDir dir;
    FootprintSegmentManifest m(dir.path());
    m.load();

    // Two old segments and one recent one - real files so remove() has
    // something to act on.
    for (const auto& name : {"old1.bin", "old2.bin", "recent.bin"})
        std::ofstream(dir.path() / name) << "x";

    SegmentMetadata old1; old1.time_range_end_ns = 0;
    SegmentMetadata old2; old2.time_range_end_ns = kSecond;
    SegmentMetadata recent; recent.time_range_end_ns = 8 * 86400 * kSecond; // "now-ish"
    m.add(old1, "old1.bin", 0);
    m.add(old2, "old2.bin", 0);
    m.add(recent, "recent.bin", 0);

    const std::int64_t now_ns = 8 * 86400 * kSecond;
    const std::int64_t seven_days_ns = 7 * 86400 * kSecond;
    auto evicted = m.enforce_retention(now_ns, seven_days_ns,
        /*max_total_bytes=*/std::numeric_limits<std::uint64_t>::max(),
        [](const std::string&) { return std::uint64_t{1}; });

    std::sort(evicted.begin(), evicted.end());
    EXPECT_EQ(evicted, (std::vector<std::string>{"old1.bin", "old2.bin"}));
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "old1.bin"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "recent.bin"));

    ASSERT_EQ(m.entries().size(), 1u);
    EXPECT_EQ(m.entries()[0].filename, "recent.bin");
}

TEST(FootprintSegmentManifest, EnforceRetentionNeverEvictsSoleMostRecentEvenIfOverBudget)
{
    ScratchDir dir;
    FootprintSegmentManifest m(dir.path());
    m.load();
    std::ofstream(dir.path() / "only.bin") << "x";

    SegmentMetadata meta; meta.time_range_end_ns = 100 * kSecond;
    m.add(meta, "only.bin", /*now_ns=*/0);

    auto evicted = m.enforce_retention(
        /*now_ns=*/100 * kSecond, /*max_age_ns=*/1'000'000 * kSecond,
        /*max_total_bytes=*/0, // budget of zero - would evict everything if allowed
        [](const std::string&) { return std::uint64_t{999}; });

    EXPECT_TRUE(evicted.empty());
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "only.bin"));
    ASSERT_EQ(m.entries().size(), 1u);
}

TEST(FootprintSegmentManifest, EnforceRetentionEvictsLeastRecentlyUsedFirstUnderSizePressure)
{
    ScratchDir dir;
    FootprintSegmentManifest m(dir.path());
    m.load();
    for (const auto& name : {"lru1.bin", "lru2.bin", "mru.bin"})
        std::ofstream(dir.path() / name) << "x";

    SegmentMetadata s1; s1.time_range_end_ns = 10 * kSecond;
    SegmentMetadata s2; s2.time_range_end_ns = 20 * kSecond;
    SegmentMetadata s3; s3.time_range_end_ns = 30 * kSecond; // most recent by time -> protected
    m.add(s1, "lru1.bin", /*now_ns=*/1);  // accessed longest ago
    m.add(s2, "lru2.bin", /*now_ns=*/2);
    m.add(s3, "mru.bin", /*now_ns=*/3);

    // Each segment "costs" 1 byte; budget of 1 forces evicting 2 of the 3,
    // and it must pick the least-recently-used ones, never the protected
    // most-recent-by-time segment.
    auto evicted = m.enforce_retention(
        /*now_ns=*/1000, /*max_age_ns=*/1'000'000, /*max_total_bytes=*/1,
        [](const std::string&) { return std::uint64_t{1}; });

    std::sort(evicted.begin(), evicted.end());
    EXPECT_EQ(evicted, (std::vector<std::string>{"lru1.bin", "lru2.bin"}));
    ASSERT_EQ(m.entries().size(), 1u);
    EXPECT_EQ(m.entries()[0].filename, "mru.bin");
}

// --- Regression: verifier-found bugs ---

TEST(FootprintSegmentManifest, TiedMostRecentSegmentsAreAllProtected)
{
    ScratchDir dir;
    FootprintSegmentManifest m(dir.path());
    m.load();
    for (const auto& name : {"tie_a.bin", "tie_b.bin"})
        std::ofstream(dir.path() / name) << "x";

    // Both share the exact same time_range_end_ns - an exact tie for
    // "most recent". Neither is arbitrarily distinguishable as THE active
    // one, so both must be protected rather than picking index 0 only.
    SegmentMetadata a; a.time_range_end_ns = 50 * kSecond;
    SegmentMetadata b; b.time_range_end_ns = 50 * kSecond;
    m.add(a, "tie_a.bin", 0);
    m.add(b, "tie_b.bin", 0);

    const auto evicted = m.enforce_retention(
        /*now_ns=*/1'000'000 * kSecond, /*max_age_ns=*/0, // everything looks "too old"
        /*max_total_bytes=*/std::numeric_limits<std::uint64_t>::max(),
        [](const std::string&) { return std::uint64_t{1}; });

    EXPECT_TRUE(evicted.empty());
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "tie_a.bin"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "tie_b.bin"));
    EXPECT_EQ(m.entries().size(), 2u);
}

TEST(FootprintSegmentManifest, FailedDeleteKeepsTheEntryTrackedRatherThanLeakingTheFile)
{
    ScratchDir dir;
    FootprintSegmentManifest m(dir.path());
    m.load();

    // std::filesystem::remove() on a non-empty directory fails with an
    // error code (it only removes empty directories or single files) -
    // use that to force a real delete failure deterministically.
    const auto undeletable = dir.path() / "undeletable.bin";
    std::filesystem::create_directory(undeletable);
    std::ofstream(undeletable / "inner.txt") << "x";

    std::ofstream(dir.path() / "recent.bin") << "x";

    SegmentMetadata old_meta; old_meta.time_range_end_ns = 0;
    SegmentMetadata recent_meta; recent_meta.time_range_end_ns = 100 * kSecond;
    m.add(old_meta, "undeletable.bin", 0);
    m.add(recent_meta, "recent.bin", 0);

    const auto evicted = m.enforce_retention(
        /*now_ns=*/100 * kSecond, /*max_age_ns=*/0,
        /*max_total_bytes=*/std::numeric_limits<std::uint64_t>::max(),
        [](const std::string&) { return std::uint64_t{1}; });

    // The undeletable entry must NOT be reported as evicted, and must
    // still be tracked in the manifest - never silently dropped while its
    // file lingers on disk untracked.
    EXPECT_TRUE(evicted.empty());
    bool still_tracked = false;
    for (const auto& e : m.entries())
        if (e.filename == "undeletable.bin") still_tracked = true;
    EXPECT_TRUE(still_tracked);
    EXPECT_TRUE(std::filesystem::exists(undeletable));
}
