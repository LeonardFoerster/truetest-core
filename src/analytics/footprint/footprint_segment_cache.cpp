#include "analytics/footprint/footprint_segment_cache.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace truetest::footprint {

namespace {

constexpr char kMagic[8] = {'T', 'T', 'F', 'P', 'S', 'E', 'G', '1'};
constexpr std::size_t kSegmentHeaderBytes =
    sizeof(kMagic) + sizeof(std::uint32_t) + 2 * sizeof(std::uint16_t)
    + 10 * sizeof(std::uint64_t);
constexpr unsigned char kEmptyByte = 0;
constexpr std::size_t kWriterReserveChunkBytes = 64U * 1024U;

static_assert(kSegmentHeaderBytes == 96,
              "The on-disk footprint segment header must stay explicitly sized");

std::size_t effective_max_uncompressed_bytes(std::size_t requested) noexcept
{
    return std::min(requested, kHardMaxSegmentUncompressedBytes);
}

std::size_t max_trade_count(std::size_t max_uncompressed_bytes) noexcept
{
    return effective_max_uncompressed_bytes(max_uncompressed_bytes) / sizeof(PublicTrade);
}

std::uint64_t fnv1a64(const unsigned char* data, std::size_t size) noexcept
{
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < size; ++i)
    {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

void write_u16(std::ostream& out, std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }
void write_u32(std::ostream& out, std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
void write_u64(std::ostream& out, std::uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); }
void write_i64(std::ostream& out, std::int64_t v)  { out.write(reinterpret_cast<const char*>(&v), 8); }

// Bounds-checked cursor over an in-memory buffer - every read reports
// failure on truncation instead of reading past the end.
class ByteReader
{
public:
    ByteReader(const unsigned char* data, std::size_t size) noexcept : data_(data), size_(size) {}

    bool try_bytes(void* out, std::size_t n) noexcept
    {
        if (n > size_ - pos_)
            return false;
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    bool try_u16(std::uint16_t& v) noexcept { return try_bytes(&v, sizeof(v)); }
    bool try_u32(std::uint32_t& v) noexcept { return try_bytes(&v, sizeof(v)); }
    bool try_u64(std::uint64_t& v) noexcept { return try_bytes(&v, sizeof(v)); }
    bool try_i64(std::int64_t& v) noexcept { return try_bytes(&v, sizeof(v)); }

    std::size_t remaining() const noexcept { return size_ - pos_; }

private:
    const unsigned char* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

bool read_exact(std::istream& in, void* out, std::size_t size)
{
    if (size == 0)
        return true;
    in.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

SegmentMetadata metadata_for(std::uint16_t venue_id, std::uint16_t symbol_id,
                             const std::vector<PublicTrade>& trades)
{
    SegmentMetadata meta;
    meta.venue_id = venue_id;
    meta.symbol_id = symbol_id;
    meta.record_count = trades.size();

    if (!trades.empty())
    {
        meta.time_range_start_ns = trades.front().event_ns;
        meta.time_range_end_ns = trades.front().event_ns;

        // A legitimate session_id/native_trade_id of exactly 0 is valid
        // data. Track presence separately and use the header's "0 = none"
        // convention only after all trades have been inspected.
        bool has_native = false;
        bool has_session = false;
        std::uint64_t native_min = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t native_max = 0;
        std::uint64_t session_min = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t session_max = 0;

        for (const auto& t : trades)
        {
            meta.time_range_start_ns = std::min(meta.time_range_start_ns, t.event_ns);
            meta.time_range_end_ns = std::max(meta.time_range_end_ns, t.event_ns);
            if (t.flags & provenance_native_id)
            {
                has_native = true;
                native_min = std::min(native_min, t.native_trade_id);
                native_max = std::max(native_max, t.native_trade_id);
            }
            else
            {
                has_session = true;
                session_min = std::min(session_min, t.session_id);
                session_max = std::max(session_max, t.session_id);
            }
        }

        if (has_native)
        {
            meta.native_id_min = native_min;
            meta.native_id_max = native_max;
        }
        if (has_session)
        {
            meta.session_id_min = session_min;
            meta.session_id_max = session_max;
        }
    }

    const auto* raw = trades.empty()
        ? &kEmptyByte
        : reinterpret_cast<const unsigned char*>(trades.data());
    meta.checksum = fnv1a64(raw, trades.size() * sizeof(PublicTrade));
    return meta;
}

bool payload_metadata_matches(const SegmentMetadata& header,
                              const SegmentMetadata& decoded) noexcept
{
    // Non-empty segments bind venue/symbol separately by checking every
    // decoded PublicTrade before this function. Validate every remaining
    // metadata field that is derived from the raw payload, so a damaged range
    // or identity span cannot silently poison manifest queries. A fully
    // authenticated empty-header format is a schema-v2 decision.
    return header.time_range_start_ns == decoded.time_range_start_ns
        && header.time_range_end_ns == decoded.time_range_end_ns
        && header.record_count == decoded.record_count
        && header.native_id_min == decoded.native_id_min
        && header.native_id_max == decoded.native_id_max
        && header.session_id_min == decoded.session_id_min
        && header.session_id_max == decoded.session_id_max
        && header.checksum == decoded.checksum;
}

bool trades_match_segment_identity(const std::vector<PublicTrade>& trades,
                                   std::uint16_t venue_id,
                                   std::uint16_t symbol_id) noexcept
{
    return std::all_of(trades.begin(), trades.end(), [venue_id, symbol_id](const PublicTrade& trade) {
        return trade.venue_id == venue_id && trade.symbol_id == symbol_id;
    });
}

} // namespace

FootprintSegmentWriter::FootprintSegmentWriter(std::uint16_t venue_id, std::uint16_t symbol_id,
                                               std::size_t max_uncompressed_bytes)
    : venue_id_(venue_id)
    , symbol_id_(symbol_id)
    , max_buffered_trades_(max_trade_count(max_uncompressed_bytes))
{
}

segment_append_status FootprintSegmentWriter::append(const PublicTrade& trade) noexcept
{
    if (trade.venue_id != venue_id_ || trade.symbol_id != symbol_id_)
        return segment_append_status::identity_mismatch;
    if (max_buffered_trades_ == 0)
        return segment_append_status::trade_too_large;
    if (trades_.size() >= max_buffered_trades_)
        return segment_append_status::full;

    try
    {
        // Grow in small capped chunks rather than asking every newly active
        // symbol for the entire segment budget. Explicit reserve() requests
        // also avoid vector's uncontrolled geometric growth near the limit.
        // This remains cold-path allocation only.
        if (trades_.size() == trades_.capacity())
        {
            const std::size_t remaining = max_buffered_trades_ - trades_.size();
            const std::size_t chunk_trades = std::max(
                std::size_t{1}, kWriterReserveChunkBytes / sizeof(PublicTrade));
            trades_.reserve(trades_.size() + std::min(remaining, chunk_trades));
        }
        trades_.push_back(trade);
        return segment_append_status::appended;
    }
    catch (const std::bad_alloc&)
    {
        return segment_append_status::resource_exhausted;
    }
    catch (const std::length_error&)
    {
        return segment_append_status::resource_exhausted;
    }
}

std::optional<SegmentMetadata> FootprintSegmentWriter::finalize(
    const std::filesystem::path& finalized_path)
{
    try
    {
        const SegmentMetadata meta = metadata_for(venue_id_, symbol_id_, trades_);
        const auto* raw = trades_.empty()
            ? &kEmptyByte
            : reinterpret_cast<const unsigned char*>(trades_.data());
        const std::size_t raw_size = trades_.size() * sizeof(PublicTrade);
        const std::size_t bound = ZSTD_compressBound(raw_size);
        if (ZSTD_isError(bound) || bound == 0)
            return std::nullopt;

        std::vector<unsigned char> compressed(bound);
        const std::size_t compressed_size =
            ZSTD_compress(compressed.data(), bound, raw, raw_size, /*level=*/1);
        if (ZSTD_isError(compressed_size))
            return std::nullopt;

        const auto partial_path = finalized_path.string() + ".partial";
        std::ofstream out(partial_path, std::ios::binary | std::ios::trunc);
        if (!out)
            return std::nullopt;

        out.write(kMagic, sizeof(kMagic));
        write_u32(out, meta.schema_version);
        write_u16(out, meta.venue_id);
        write_u16(out, meta.symbol_id);
        write_i64(out, meta.time_range_start_ns);
        write_i64(out, meta.time_range_end_ns);
        write_u64(out, meta.record_count);
        write_u64(out, meta.native_id_min);
        write_u64(out, meta.native_id_max);
        write_u64(out, meta.session_id_min);
        write_u64(out, meta.session_id_max);
        write_u64(out, meta.checksum);
        write_u64(out, static_cast<std::uint64_t>(raw_size));
        write_u64(out, static_cast<std::uint64_t>(compressed_size));
        out.write(reinterpret_cast<const char*>(compressed.data()),
                  static_cast<std::streamsize>(compressed_size));
        if (!out)
            return std::nullopt;

        out.close();
        if (!out)
            return std::nullopt;

        std::error_code ec;
        std::filesystem::rename(partial_path, finalized_path, ec);
        if (ec)
            return std::nullopt;

        // A successful atomic rename publishes the raw batch. Release the
        // bounded backing storage rather than retaining a high-water capacity
        // for the next segment. Power-loss durability requires a separate
        // file-and-directory fsync protocol.
        std::vector<PublicTrade>{}.swap(trades_);
        return meta;
    }
    catch (const std::bad_alloc&)
    {
        return std::nullopt;
    }
    catch (const std::length_error&)
    {
        return std::nullopt;
    }
}

segment_read_status read_segment(const std::filesystem::path& path,
                                  SegmentMetadata& out_meta,
                                  std::vector<PublicTrade>& out_trades)
{
    return read_segment(path, out_meta, out_trades, SegmentReadLimits{});
}

segment_read_status read_segment(const std::filesystem::path& path,
                                  SegmentMetadata& out_meta,
                                  std::vector<PublicTrade>& out_trades,
                                  SegmentReadLimits limits)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return segment_read_status::not_found;

    // Never materialize the whole input before looking at its bounded header:
    // a corrupt/sparse segment must not be able to turn a cache read into an
    // unbounded allocation. The header declares the bounded payload buffers
    // we will make, and those declarations are checked before payload reads.
    std::array<unsigned char, kSegmentHeaderBytes> header{};
    if (!read_exact(in, header.data(), header.size()))
        return segment_read_status::truncated;

    ByteReader r(header.data(), header.size());

    char magic[sizeof(kMagic)];
    if (!r.try_bytes(magic, sizeof(magic)))
        return segment_read_status::truncated;
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        return segment_read_status::bad_magic;

    SegmentMetadata meta;
    if (!r.try_u32(meta.schema_version)) return segment_read_status::truncated;
    if (meta.schema_version != kSegmentSchemaVersion) return segment_read_status::schema_mismatch;
    if (!r.try_u16(meta.venue_id)) return segment_read_status::truncated;
    if (!r.try_u16(meta.symbol_id)) return segment_read_status::truncated;
    if (!r.try_i64(meta.time_range_start_ns)) return segment_read_status::truncated;
    if (!r.try_i64(meta.time_range_end_ns)) return segment_read_status::truncated;
    if (!r.try_u64(meta.record_count)) return segment_read_status::truncated;
    if (!r.try_u64(meta.native_id_min)) return segment_read_status::truncated;
    if (!r.try_u64(meta.native_id_max)) return segment_read_status::truncated;
    if (!r.try_u64(meta.session_id_min)) return segment_read_status::truncated;
    if (!r.try_u64(meta.session_id_max)) return segment_read_status::truncated;
    if (!r.try_u64(meta.checksum)) return segment_read_status::truncated;

    std::uint64_t raw_size = 0, compressed_size = 0;
    if (!r.try_u64(raw_size)) return segment_read_status::truncated;
    if (!r.try_u64(compressed_size)) return segment_read_status::truncated;
    if (r.remaining() != 0) return segment_read_status::truncated;

    const std::size_t max_raw_size =
        effective_max_uncompressed_bytes(limits.max_uncompressed_bytes);
    if (raw_size > max_raw_size)
        return segment_read_status::resource_limit;
    if (raw_size % sizeof(PublicTrade) != 0)
        return segment_read_status::checksum_mismatch; // structurally impossible unless corrupt
    if (meta.record_count > max_raw_size / sizeof(PublicTrade))
        return segment_read_status::resource_limit;
    if (raw_size != meta.record_count * sizeof(PublicTrade))
        return segment_read_status::checksum_mismatch;

    const std::size_t raw_bytes = static_cast<std::size_t>(raw_size);
    const std::size_t max_compressed_size = ZSTD_compressBound(raw_bytes);
    if (ZSTD_isError(max_compressed_size)
        || compressed_size > static_cast<std::uint64_t>(max_compressed_size))
        return segment_read_status::resource_limit;

    try
    {
        std::vector<unsigned char> compressed(static_cast<std::size_t>(compressed_size));
        if (!read_exact(in, compressed.data(), compressed.size()))
            return segment_read_status::truncated;

        char unexpected_trailer = 0;
        if (in.get(unexpected_trailer))
            return segment_read_status::trailing_data;
        if (!in.eof())
            return segment_read_status::truncated;

        unsigned char empty_byte = 0;
        const auto* compressed_data = compressed.empty() ? &empty_byte : compressed.data();
        const std::size_t frame_size =
            ZSTD_findFrameCompressedSize(compressed_data, compressed.size());
        if (ZSTD_isError(frame_size) || frame_size != compressed.size())
            return segment_read_status::truncated;

        std::vector<PublicTrade> decoded(static_cast<std::size_t>(meta.record_count));
        void* decoded_data = decoded.empty()
            ? static_cast<void*>(&empty_byte)
            : static_cast<void*>(decoded.data());
        const std::size_t decompressed = ZSTD_decompress(
            decoded_data, decoded.empty() ? 1U : raw_bytes,
            compressed_data, compressed.size());
        if (ZSTD_isError(decompressed) || decompressed != raw_size)
            return segment_read_status::truncated;

        if (!trades_match_segment_identity(decoded, meta.venue_id, meta.symbol_id))
            return segment_read_status::checksum_mismatch;
        const SegmentMetadata decoded_meta = metadata_for(meta.venue_id, meta.symbol_id, decoded);
        if (!payload_metadata_matches(meta, decoded_meta))
            return segment_read_status::checksum_mismatch;

        out_trades = std::move(decoded);
        out_meta = meta;
        return segment_read_status::ok;
    }
    catch (const std::bad_alloc&)
    {
        return segment_read_status::resource_limit;
    }
    catch (const std::length_error&)
    {
        return segment_read_status::resource_limit;
    }
}

std::optional<std::filesystem::path> quarantine_segment(const std::filesystem::path& path)
{
    auto quarantined = path;
    quarantined += ".quarantined";
    std::error_code ec;
    std::filesystem::rename(path, quarantined, ec);
    if (ec)
        return std::nullopt;
    return quarantined;
}

FootprintSegmentManifest::FootprintSegmentManifest(std::filesystem::path directory)
    : directory_(std::move(directory))
{
}

bool FootprintSegmentManifest::load()
{
    entries_.clear();
    std::ifstream in(directory_ / "manifest.txt");
    if (!in)
        return true; // missing file - empty manifest, not an error

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        std::istringstream iss(line);
        ManifestEntry e;
        iss >> e.meta.schema_version >> e.meta.venue_id >> e.meta.symbol_id
            >> e.meta.time_range_start_ns >> e.meta.time_range_end_ns
            >> e.meta.record_count >> e.meta.native_id_min >> e.meta.native_id_max
            >> e.meta.session_id_min >> e.meta.session_id_max >> e.meta.checksum
            >> e.last_access_ns >> e.filename;
        if (!iss)
            continue; // skip a malformed line defensively rather than fail the whole load
        entries_.push_back(std::move(e));
    }
    return true;
}

bool FootprintSegmentManifest::save() const
{
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    const auto tmp_path = directory_ / "manifest.txt.tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        for (const auto& e : entries_)
        {
            out << e.meta.schema_version << ' ' << e.meta.venue_id << ' ' << e.meta.symbol_id << ' '
                << e.meta.time_range_start_ns << ' ' << e.meta.time_range_end_ns << ' '
                << e.meta.record_count << ' ' << e.meta.native_id_min << ' ' << e.meta.native_id_max << ' '
                << e.meta.session_id_min << ' ' << e.meta.session_id_max << ' ' << e.meta.checksum << ' '
                << e.last_access_ns << ' ' << e.filename << '\n';
        }
        if (!out)
            return false;
    }
    std::filesystem::rename(tmp_path, directory_ / "manifest.txt", ec);
    return !ec;
}

bool FootprintSegmentManifest::add(const SegmentMetadata& meta, const std::string& filename,
                                   std::int64_t now_ns)
{
    ManifestEntry e;
    e.meta = meta;
    e.filename = filename;
    e.last_access_ns = now_ns;
    entries_.push_back(std::move(e));
    return save();
}

std::vector<ManifestEntry> FootprintSegmentManifest::segments_covering(
    std::int64_t query_start_ns, std::int64_t query_end_ns) const
{
    std::vector<ManifestEntry> out;
    for (const auto& e : entries_)
        if (e.meta.time_range_start_ns < query_end_ns && e.meta.time_range_end_ns > query_start_ns)
            out.push_back(e);
    return out;
}

std::vector<std::string> FootprintSegmentManifest::enforce_retention(
    std::int64_t now_ns, std::int64_t max_age_ns, std::uint64_t max_total_bytes,
    std::function<std::uint64_t(const std::string&)> segment_byte_size)
{
    std::vector<std::string> evicted;
    if (entries_.empty())
        return evicted;

    // Never evict the most recent segment(s) (by time_range_end_ns) - the
    // active/current-visible one(s) - regardless of age or size pressure.
    // A conservative tie-break: if more than one entry shares the maximum
    // time_range_end_ns, ALL of them are protected (there is no reliable
    // way to pick a single "the" active one among an exact tie, and
    // protecting too many is the safe direction - see footprint.md's
    // "never the active segment or current visible range").
    std::int64_t max_end_ns = entries_.front().meta.time_range_end_ns;
    for (const auto& e : entries_)
        max_end_ns = std::max(max_end_ns, e.meta.time_range_end_ns);
    std::unordered_set<std::string> protected_filenames;
    for (const auto& e : entries_)
        if (e.meta.time_range_end_ns == max_end_ns)
            protected_filenames.insert(e.filename);

    // A physical delete failure (permissions, still open elsewhere, ...)
    // must not silently drop the manifest entry anyway - that would leak
    // an orphaned, untracked file on disk. Only entries whose file was
    // actually removed leave the manifest; a failed delete stays tracked
    // so a later call can retry it (and it keeps counting toward
    // age/size pressure, which is the honest state).
    auto remove_file = [&](const std::string& filename) {
        std::error_code ec;
        std::filesystem::remove(directory_ / filename, ec);
        return !ec;
    };

    // Pass 1: age-based eviction.
    std::vector<ManifestEntry> kept;
    kept.reserve(entries_.size());
    for (auto& e : entries_)
    {
        const bool too_old = (now_ns - e.meta.time_range_end_ns) > max_age_ns;
        if (too_old && !protected_filenames.count(e.filename) && remove_file(e.filename))
            evicted.push_back(e.filename);
        else
            kept.push_back(std::move(e));
    }
    entries_ = std::move(kept);

    // Pass 2: size-based LRU eviction (least-recently-used first) until
    // under max_total_bytes - never a protected (most recent) entry.
    std::sort(entries_.begin(), entries_.end(), [](const ManifestEntry& a, const ManifestEntry& b) {
        return a.last_access_ns < b.last_access_ns;
    });

    auto total_bytes = [&]() {
        std::uint64_t sum = 0;
        for (const auto& e : entries_)
            sum += segment_byte_size(e.filename);
        return sum;
    };

    std::size_t i = 0;
    while (total_bytes() > max_total_bytes && i < entries_.size())
    {
        if (protected_filenames.count(entries_[i].filename))
        {
            ++i;
            continue;
        }
        if (!remove_file(entries_[i].filename))
        {
            ++i; // failed delete - leave it tracked, try the next-oldest instead
            continue;
        }
        evicted.push_back(entries_[i].filename);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        // Don't advance i - the next-oldest survivor just shifted into position i.
    }

    save();
    return evicted;
}

} // namespace truetest::footprint
