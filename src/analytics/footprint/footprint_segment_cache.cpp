#include "analytics/footprint/footprint_segment_cache.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace truetest::footprint {

namespace {

constexpr char kMagic[8] = {'T', 'T', 'F', 'P', 'S', 'E', 'G', '1'};

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
        if (pos_ + n > size_)
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
    const unsigned char* cursor() const noexcept { return data_ + pos_; }

private:
    const unsigned char* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

} // namespace

FootprintSegmentWriter::FootprintSegmentWriter(std::uint16_t venue_id, std::uint16_t symbol_id)
    : venue_id_(venue_id)
    , symbol_id_(symbol_id)
{
}

void FootprintSegmentWriter::append(const PublicTrade& trade)
{
    trades_.push_back(trade);
}

std::optional<SegmentMetadata> FootprintSegmentWriter::finalize(
    const std::filesystem::path& finalized_path)
{
    SegmentMetadata meta;
    meta.venue_id = venue_id_;
    meta.symbol_id = symbol_id_;
    meta.record_count = trades_.size();

    if (!trades_.empty())
    {
        meta.time_range_start_ns = trades_.front().event_ns;
        meta.time_range_end_ns = trades_.front().event_ns;

        // A legitimate session_id/native_trade_id of exactly 0 is valid
        // data (e.g. a venue's first session), so "0 == unset" can't be
        // used as the running-min sentinel while accumulating - it would
        // wrongly latch onto 0 forever the moment a real 0 is seen even if
        // a later trade's id is smaller. Track "has any" separately instead
        // and only apply the header's documented "0 = none of this kind"
        // convention once, at the end.
        bool has_native = false, has_session = false;
        std::uint64_t native_min = std::numeric_limits<std::uint64_t>::max(), native_max = 0;
        std::uint64_t session_min = std::numeric_limits<std::uint64_t>::max(), session_max = 0;

        for (const auto& t : trades_)
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

        if (has_native) { meta.native_id_min = native_min; meta.native_id_max = native_max; }
        if (has_session) { meta.session_id_min = session_min; meta.session_id_max = session_max; }
    }

    const auto* raw = reinterpret_cast<const unsigned char*>(trades_.data());
    const std::size_t raw_size = trades_.size() * sizeof(PublicTrade);
    meta.checksum = fnv1a64(raw, raw_size);

    const auto partial_path = finalized_path.string() + ".partial";
    {
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

        const std::size_t bound = ZSTD_compressBound(raw_size);
        std::vector<unsigned char> compressed(bound);
        const std::size_t compressed_size =
            ZSTD_compress(compressed.data(), bound, raw, raw_size, /*level=*/1);
        if (ZSTD_isError(compressed_size))
            return std::nullopt;

        write_u64(out, static_cast<std::uint64_t>(raw_size));
        write_u64(out, static_cast<std::uint64_t>(compressed_size));
        out.write(reinterpret_cast<const char*>(compressed.data()),
                  static_cast<std::streamsize>(compressed_size));
        if (!out)
            return std::nullopt;
    }

    std::error_code ec;
    std::filesystem::rename(partial_path, finalized_path, ec);
    if (ec)
        return std::nullopt;

    return meta;
}

segment_read_status read_segment(const std::filesystem::path& path,
                                  SegmentMetadata& out_meta,
                                  std::vector<PublicTrade>& out_trades)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return segment_read_status::not_found;

    std::vector<unsigned char> all(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    ByteReader r(all.data(), all.size());

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
    if (r.remaining() < compressed_size) return segment_read_status::truncated;

    // Sanity-check the claimed decompressed size BEFORE allocating for it -
    // an on-disk header is untrusted input (corruption or a hostile file),
    // and ZSTD_decompress's declared output size is not otherwise bounded
    // by the (small) compressed input on disk. Cross-checking against
    // record_count also catches corruption that changed raw_size alone.
    // Reject rather than risk an unbounded/failing allocation - the
    // documented behavior is "quarantine and report", never crash.
    constexpr std::uint64_t kMaxReasonableRawSize = 512ULL * 1024 * 1024; // far beyond any real 5-minute segment
    if (raw_size > kMaxReasonableRawSize)
        return segment_read_status::truncated;
    if (raw_size % sizeof(PublicTrade) != 0)
        return segment_read_status::checksum_mismatch; // structurally impossible unless corrupt
    // Guard the multiplication itself before comparing - record_count is
    // equally untrusted input, and could otherwise overflow into a value
    // that coincidentally matches the (already-bounded) raw_size.
    if (meta.record_count > kMaxReasonableRawSize / sizeof(PublicTrade))
        return segment_read_status::checksum_mismatch;
    if (raw_size != meta.record_count * sizeof(PublicTrade))
        return segment_read_status::checksum_mismatch;

    std::vector<unsigned char> raw(raw_size);
    if (raw_size > 0)
    {
        const std::size_t decompressed =
            ZSTD_decompress(raw.data(), raw_size, r.cursor(), compressed_size);
        if (ZSTD_isError(decompressed) || decompressed != raw_size)
            return segment_read_status::truncated;
    }

    if (fnv1a64(raw.data(), raw.size()) != meta.checksum)
        return segment_read_status::checksum_mismatch;

    const std::size_t n = raw_size / sizeof(PublicTrade);
    out_trades.resize(n);
    if (n > 0)
        std::memcpy(out_trades.data(), raw.data(), raw_size);
    out_meta = meta;
    return segment_read_status::ok;
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
