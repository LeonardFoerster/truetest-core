#pragma once

#include "providers/parser.h"
#include "data/data_handler.h"
#include "data/market_provenance.h"
#include "data/strict_market_csv.h"

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef HAS_DEBUG
#include "debug/copy_tracker.h"
#endif

// Performance helpers for large CSVs (1.7M+ rows common for multi-year bar data).
// Goal: reduce the dominant cost when backtesting 4+ years of bars.
// - std::from_chars for integers (volume, timestamps, qty) – zero-alloc, fast.
// - Lightweight string_view field walking instead of vector<string> + stringstream per row.
// - Caller is expected to call data_handler::reserve() before loading.
namespace tt::csv {

// Base-asset volume scale for fractional exchange quantities (matches Binance kline path).
// Integer-only volume fields (legacy equity CSVs) are stored as-is without scaling.
inline constexpr std::uint64_t kIntegerQuantityScale = tt::strict_market_csv::kIntegerQuantityScale;

inline std::int64_t fast_stoll(std::string_view sv)
{
    std::int64_t v = 0;
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{} || p != sv.data() + sv.size())
        throw std::invalid_argument("fast_stoll failed");
    return v;
}

inline bool parse_nonnegative_quantity(std::string_view sv, std::int64_t& out,
                                       std::uint64_t& quantity_scale)
{
    out = 0;
    quantity_scale = kIntegerQuantityScale;
    if (sv.empty()) return true;
    return tt::strict_market_csv::parse_decimal_atoms(sv, true, out, quantity_scale);
}

// Integer volumes pass through; fractional (e.g. "246.092") use 1e8 atoms.
inline std::int64_t parse_bar_volume(std::string_view sv, std::uint64_t* quantity_scale = nullptr)
{
    std::int64_t out = 0;
    std::uint64_t scale = kIntegerQuantityScale;
    (void)parse_nonnegative_quantity(sv, out, scale);
    if (quantity_scale) *quantity_scale = scale;
    return out;
}

}  // namespace tt::csv


struct bar_record
#ifdef HAS_DEBUG
    : public debug::CopyTracker<bar_record>
#endif
{
    std::string date;
    std::string symbol;
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    int64_t volume = 0;
    // Epoch milliseconds from open_time (Binance kline CSV). 0 = unset → use date.
    int64_t open_time_ms = 0;
    uint64_t quantity_scale = 1;
    std::optional<tt::data_provenance::accepted_row> source;
};

class CsvBarParser : public IDataParser<bar_record>
{
public:
    bool header_frame_contains_records() const override { return false; }
    bool parse_header(const std::string& line) override
    {
        rejections_.clear();
        physical_rows_seen_ = 1;
        accepted_count_ = 0;
        return static_cast<bool>(schema_.parse_header(line));
    }

    std::span<const tt::data_provenance::rejected_row> rejections() const noexcept
    {
        return rejections_;
    }

    std::size_t physical_rows_seen() const noexcept { return physical_rows_seen_; }
    std::size_t accepted_count() const noexcept { return accepted_count_; }

    std::optional<bar_record> parse_record(const std::string& line) override
    {
        const std::size_t physical_row = ++physical_rows_seen_;
        tt::strict_market_csv::parsed_bar parsed;
        const auto result = schema_.parse_row(line, parsed);
        if (!result) {
            reject(physical_row, map_reason(result.reason), map_field(result.source));
            return std::nullopt;
        }

        bar_record rec;
        rec.date = std::string(parsed.date);
        rec.symbol = std::string(parsed.symbol);
        rec.open = parsed.open;
        rec.high = parsed.high;
        rec.low = parsed.low;
        rec.close = parsed.close;
        rec.volume = parsed.volume;
        rec.open_time_ms = parsed.open_time_ms.value_or(0);
        rec.quantity_scale = parsed.quantity_scale;
        rec.source = tt::data_provenance::accepted_row{physical_row, accepted_count_++};
        return rec;
    }

private:
    static tt::data_provenance::rejection_reason
    map_reason(tt::strict_market_csv::error reason) noexcept
    {
        using source = tt::strict_market_csv::error;
        using target = tt::data_provenance::rejection_reason;
        switch (reason) {
        case source::empty_row:
            return target::empty_row;
        case source::repeated_header:
            return target::repeated_header;
        case source::missing_required_field:
            return target::missing_required_field;
        case source::invalid_quantity:
            return target::invalid_volume;
        case source::invalid_timestamp:
            return target::invalid_timestamp;
        case source::conflicting_timestamp:
            return target::invalid_timestamp;
        case source::non_finite_price:
            return target::non_finite_price;
        case source::non_positive_price:
            return target::non_positive_price;
        case source::high_below_low:
            return target::high_below_low;
        case source::open_outside_range:
            return target::open_outside_range;
        case source::close_outside_range:
            return target::close_outside_range;
        case source::invalid_symbol:
            return target::missing_required_field;
        default:
            return target::malformed_numeric;
        }
    }

    static tt::data_provenance::source_field map_field(tt::strict_market_csv::field field) noexcept
    {
        using source = tt::strict_market_csv::field;
        using target = tt::data_provenance::source_field;
        switch (field) {
        case source::date:
            return target::date;
        case source::symbol:
            return target::symbol;
        case source::open_time:
            return target::open_time;
        case source::open:
            return target::open;
        case source::high:
            return target::high;
        case source::low:
            return target::low;
        case source::close:
            return target::close;
        case source::volume:
            return target::volume;
        default:
            return target::none;
        }
    }

    void reject(std::size_t physical_row, tt::data_provenance::rejection_reason reason,
                tt::data_provenance::source_field field = tt::data_provenance::source_field::none)
    {
        rejections_.push_back({physical_row, std::nullopt,
                               tt::data_provenance::rejection_stage::parser, reason, field});
    }

    tt::strict_market_csv::bar_schema schema_;
    std::vector<tt::data_provenance::rejected_row> rejections_;
    std::size_t physical_rows_seen_ = 0;
    std::size_t accepted_count_ = 0;
};


class CsvTickParser : public IDataParser<tick_record>
{
public:
    bool parse_header(const std::string& line) override
    {
        return static_cast<bool>(schema_.parse_header_or_first_row(line));
    }

    bool header_frame_contains_records() const override { return schema_.header_contains_record(); }

    std::optional<tick_record> parse_record(const std::string& line) override
    {
        tt::strict_market_csv::parsed_tick parsed;
        if (!schema_.parse_row(line, parsed)) return std::nullopt;
        tick_record rec;
        rec.timestamp = parsed.timestamp;
        rec.symbol = std::string(parsed.symbol);
        rec.price = parsed.price;
        rec.quantity = parsed.quantity;
        rec.quantity_scale = parsed.quantity_scale;
        rec.side = parsed.side;
        return rec;
    }

private:
    tt::strict_market_csv::tick_schema schema_;
};


inline void bar_record_sink(const bar_record& rec, std::shared_ptr<data_handler> handler)
{
    handler->load_into_queue(rec.date, rec.symbol, rec.open, rec.high, rec.low, rec.close,
                             rec.volume, rec.quantity_scale);
}

inline void tick_record_sink(const tick_record& rec, std::shared_ptr<data_handler> handler)
{
    handler->add_tick(rec);
}
