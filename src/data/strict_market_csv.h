#pragma once

#include "data/date_parse.h"
#include "data/market_types.h"
#include "data/symbol_validation.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace tt::strict_market_csv {

inline constexpr std::size_t kMaximumColumns = 32;
inline constexpr std::uint64_t kIntegerQuantityScale = 1;
inline constexpr std::uint64_t kFractionalQuantityScale = 100'000'000ULL;
inline constexpr std::size_t kMissingColumn = std::numeric_limits<std::size_t>::max();

enum class field : std::uint8_t
{
    none,
    date,
    symbol,
    open_time,
    open,
    high,
    low,
    close,
    volume,
    timestamp,
    price,
    quantity,
    side
};

enum class error : std::uint8_t
{
    none,
    empty_row,
    malformed_csv,
    duplicate_column,
    missing_required_column,
    missing_required_field,
    malformed_numeric,
    invalid_timestamp,
    conflicting_timestamp,
    non_finite_price,
    non_positive_price,
    invalid_quantity,
    high_below_low,
    open_outside_range,
    close_outside_range,
    invalid_symbol,
    invalid_side,
    repeated_header
};

struct status
{
    error reason = error::none;
    field source = field::none;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return reason == error::none;
    }
};

struct split_fields
{
    std::array<std::string_view, kMaximumColumns> values{};
    std::size_t count = 0;
};

inline std::string_view trim(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                              value.back() == '\n'))
        value.remove_suffix(1);
    return value;
}

inline status split(std::string_view line, split_fields& output) noexcept
{
    output = {};
    if (trim(line).empty()) return {error::empty_row, field::none};
    // Quoted CSV is intentionally unsupported. Silently treating an embedded
    // comma as a field separator would change the instrument/economic record.
    if (line.find('"') != std::string_view::npos) return {error::malformed_csv, field::none};

    std::size_t cursor = 0;
    while (cursor <= line.size()) {
        if (output.count == output.values.size()) return {error::malformed_csv, field::none};
        const std::size_t comma = line.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos ? line.size() : comma;
        output.values[output.count++] = trim(line.substr(cursor, end - cursor));
        if (comma == std::string_view::npos) break;
        cursor = comma + 1;
    }
    return {};
}

inline bool parse_integer(std::string_view token, std::int64_t& output) noexcept
{
    token = trim(token);
    if (token.empty()) return false;
    for (const char character : token) {
        if (character < '0' || character > '9') return false;
    }
    const auto [end, conversion_error] =
        std::from_chars(token.data(), token.data() + token.size(), output);
    return conversion_error == std::errc{} && end == token.data() + token.size();
}

inline status parse_price(std::string_view token, double& output, field source) noexcept
{
    token = trim(token);
    if (token.empty()) return {error::missing_required_field, source};
    const auto [end, conversion_error] = std::from_chars(token.data(), token.data() + token.size(),
                                                         output, std::chars_format::general);
    if (conversion_error != std::errc{} || end != token.data() + token.size())
        return {error::malformed_numeric, source};
    if (!std::isfinite(output)) return {error::non_finite_price, source};
    if (output <= 0.0) return {error::non_positive_price, source};
    return {};
}

// Quantities are represented exactly as either whole units (scale 1) or
// decimal atoms (scale 1e8). No binary floating-point rounding is involved.
inline bool parse_decimal_atoms(std::string_view token, bool allow_zero, std::int64_t& output,
                                std::uint64_t& quantity_scale) noexcept
{
    token = trim(token);
    output = 0;
    quantity_scale = kIntegerQuantityScale;
    if (token.empty()) return false;

    const std::size_t dot = token.find('.');
    if (dot == 0 || dot == token.size() - 1 ||
        (dot != std::string_view::npos && token.find('.', dot + 1) != std::string_view::npos))
        return false;
    const std::string_view whole_token =
        dot == std::string_view::npos ? token : token.substr(0, dot);
    std::int64_t whole = 0;
    if (!parse_integer(whole_token, whole)) return false;

    if (dot == std::string_view::npos) {
        if (!allow_zero && whole == 0) return false;
        output = whole;
        return true;
    }

    const std::string_view fractional = token.substr(dot + 1);
    if (fractional.size() > 8) return false;
    std::int64_t fraction = 0;
    if (!parse_integer(fractional, fraction)) return false;
    for (std::size_t digits = fractional.size(); digits < 8; ++digits)
        fraction *= 10;
    constexpr std::int64_t scale = static_cast<std::int64_t>(kFractionalQuantityScale);
    if (whole > (std::numeric_limits<std::int64_t>::max() - fraction) / scale) return false;
    output = whole * scale + fraction;
    quantity_scale = kFractionalQuantityScale;
    return allow_zero || output > 0;
}

inline std::string_view header_name(std::string_view value, bool first) noexcept
{
    value = trim(value);
    if (first && value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF)
        value.remove_prefix(3);
    return value;
}

struct parsed_bar
{
    std::string_view date;
    std::string_view symbol;
    std::optional<std::int64_t> open_time_ms;
    std::chrono::system_clock::time_point timestamp{};
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    std::int64_t volume = 0;
    std::uint64_t quantity_scale = kIntegerQuantityScale;
};

class bar_schema
{
public:
    status parse_header(std::string_view line) noexcept
    {
        *this = {};
        split_fields columns;
        if (const status parsed = split(line, columns); !parsed) return parsed;
        field_count_ = columns.count;

        for (std::size_t index = 0; index < columns.count; ++index) {
            const std::string_view name = header_name(columns.values[index], index == 0);
            if (name.empty()) return {error::malformed_csv, field::none};
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (name == header_name(columns.values[earlier], earlier == 0))
                    return {error::duplicate_column, field::none};
            }
            assign_column(name, index);
        }

        if (open_ == kMissingColumn || high_ == kMissingColumn || low_ == kMissingColumn ||
            close_ == kMissingColumn || volume_ == kMissingColumn)
            return {error::missing_required_column, field::none};
        if (date_ == kMissingColumn && open_time_ == kMissingColumn)
            return {error::missing_required_column, field::open_time};
        valid_ = true;
        return {};
    }

    status parse_row(std::string_view line, parsed_bar& output) const noexcept
    {
        output = {};
        if (!valid_) return {error::missing_required_column, field::none};
        split_fields columns;
        if (const status parsed = split(line, columns); !parsed) return parsed;
        if (columns.count != field_count_) return {error::malformed_csv, field::none};
        if (is_repeated_header(columns)) return {error::repeated_header, field::none};

        output.date = get(columns, date_);
        output.symbol = get(columns, symbol_);
        if (symbol_ != kMissingColumn &&
            !tt::symbol_validation::valid(output.symbol))
            return {error::invalid_symbol, field::symbol};

        std::optional<std::chrono::system_clock::time_point> date_timestamp;
        if (!output.date.empty()) {
            date_timestamp = tt::date_parse::parse(output.date);
            if (!date_timestamp) return {error::invalid_timestamp, field::date};
        }

        const std::string_view open_time_token = get(columns, open_time_);
        if (open_time_ != kMissingColumn) {
            if (open_time_token.empty()) return {error::missing_required_field, field::open_time};
            std::int64_t milliseconds = 0;
            if (!parse_integer(open_time_token, milliseconds))
                return {error::invalid_timestamp, field::open_time};
            const auto timestamp = tt::date_parse::from_epoch_milliseconds(milliseconds);
            if (!timestamp) return {error::invalid_timestamp, field::open_time};
            if (date_timestamp) {
                const bool date_only =
                    output.date.size() == 10 && output.date[4] == '-' && output.date[7] == '-';
                const bool same_time =
                    date_only ? std::chrono::floor<std::chrono::days>(*timestamp) ==
                                    std::chrono::floor<std::chrono::days>(*date_timestamp)
                              : *timestamp == *date_timestamp;
                if (!same_time) return {error::conflicting_timestamp, field::open_time};
            }
            output.open_time_ms = milliseconds;
            output.timestamp = *timestamp;
        } else if (date_timestamp) {
            output.timestamp = *date_timestamp;
        } else {
            return {error::missing_required_field, field::open_time};
        }

        if (const status parsed = parse_price(get(columns, open_), output.open, field::open);
            !parsed)
            return parsed;
        if (const status parsed = parse_price(get(columns, high_), output.high, field::high);
            !parsed)
            return parsed;
        if (const status parsed = parse_price(get(columns, low_), output.low, field::low); !parsed)
            return parsed;
        if (const status parsed = parse_price(get(columns, close_), output.close, field::close);
            !parsed)
            return parsed;
        if (output.high < output.low) return {error::high_below_low, field::high};
        if (output.open < output.low || output.open > output.high)
            return {error::open_outside_range, field::open};
        if (output.close < output.low || output.close > output.high)
            return {error::close_outside_range, field::close};

        if (!parse_decimal_atoms(get(columns, volume_), false, output.volume,
                                 output.quantity_scale))
            return {error::invalid_quantity, field::volume};
        return {};
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    static std::string_view get(const split_fields& columns, std::size_t index) noexcept
    {
        return index == kMissingColumn ? std::string_view{} : columns.values[index];
    }

    void assign_column(std::string_view name, std::size_t index) noexcept
    {
        if (name == "date")
            date_ = index;
        else if (name == "symbol")
            symbol_ = index;
        else if (name == "open_time")
            open_time_ = index;
        else if (name == "open")
            open_ = index;
        else if (name == "high")
            high_ = index;
        else if (name == "low")
            low_ = index;
        else if (name == "close")
            close_ = index;
        else if (name == "volume")
            volume_ = index;
    }

    bool is_repeated_header(const split_fields& columns) const noexcept
    {
        return get(columns, open_) == "open" && get(columns, high_) == "high" &&
               get(columns, low_) == "low" && get(columns, close_) == "close";
    }

    std::size_t field_count_ = 0;
    std::size_t date_ = kMissingColumn;
    std::size_t symbol_ = kMissingColumn;
    std::size_t open_time_ = kMissingColumn;
    std::size_t open_ = kMissingColumn;
    std::size_t high_ = kMissingColumn;
    std::size_t low_ = kMissingColumn;
    std::size_t close_ = kMissingColumn;
    std::size_t volume_ = kMissingColumn;
    bool valid_ = false;
};

struct parsed_tick
{
    std::chrono::system_clock::time_point timestamp{};
    std::string_view symbol;
    double price = 0.0;
    std::int64_t quantity = 0;
    std::uint64_t quantity_scale = kIntegerQuantityScale;
    data_tick_side side = data_tick_side::unknown;
};

class tick_schema
{
public:
    status parse_header_or_first_row(std::string_view line) noexcept
    {
        *this = {};
        split_fields columns;
        if (const status parsed = split(line, columns); !parsed) return parsed;

        bool has_timestamp_name = false;
        bool has_symbol_name = false;
        bool has_price_name = false;
        bool has_quantity_name = false;
        for (std::size_t index = 0; index < columns.count; ++index) {
            const std::string_view name = header_name(columns.values[index], index == 0);
            has_timestamp_name = has_timestamp_name || name == "timestamp_ms";
            has_symbol_name = has_symbol_name || name == "symbol";
            has_price_name = has_price_name || name == "price";
            has_quantity_name = has_quantity_name || name == "quantity";
        }
        if (has_timestamp_name && has_symbol_name && has_price_name && has_quantity_name) {
            field_count_ = columns.count;
            for (std::size_t index = 0; index < columns.count; ++index) {
                const std::string_view name = header_name(columns.values[index], index == 0);
                if (name.empty()) return {error::malformed_csv, field::none};
                for (std::size_t earlier = 0; earlier < index; ++earlier) {
                    if (name == header_name(columns.values[earlier], earlier == 0))
                        return {error::duplicate_column, field::none};
                }
                assign_column(name, index);
            }
            if (timestamp_ == kMissingColumn || symbol_ == kMissingColumn ||
                price_ == kMissingColumn || quantity_ == kMissingColumn)
                return {error::missing_required_column, field::none};
            valid_ = true;
            header_contains_record_ = false;
            return {};
        }

        if (columns.count != 4 && columns.count != 5) return {error::malformed_csv, field::none};
        set_canonical(columns.count);
        parsed_tick candidate;
        if (const status parsed = parse_row(line, candidate); !parsed) return parsed;
        header_contains_record_ = true;
        return {};
    }

    status parse_row(std::string_view line, parsed_tick& output) const noexcept
    {
        output = {};
        split_fields columns;
        if (const status parsed = split(line, columns); !parsed) return parsed;

        tick_schema canonical;
        const tick_schema* schema = this;
        if (!valid_) {
            if (columns.count != 4 && columns.count != 5)
                return {error::malformed_csv, field::none};
            canonical.set_canonical(columns.count);
            schema = &canonical;
        }
        if (columns.count != schema->field_count_) return {error::malformed_csv, field::none};

        std::int64_t milliseconds = 0;
        if (!parse_integer(get(columns, schema->timestamp_), milliseconds))
            return {error::invalid_timestamp, field::timestamp};
        const auto timestamp = tt::date_parse::from_epoch_milliseconds(milliseconds);
        if (!timestamp) return {error::invalid_timestamp, field::timestamp};
        output.timestamp = *timestamp;

        output.symbol = get(columns, schema->symbol_);
        if (!tt::symbol_validation::valid(output.symbol))
            return {error::invalid_symbol, field::symbol};
        if (const status parsed =
                parse_price(get(columns, schema->price_), output.price, field::price);
            !parsed)
            return parsed;
        if (!parse_decimal_atoms(get(columns, schema->quantity_), false, output.quantity,
                                 output.quantity_scale))
            return {error::invalid_quantity, field::quantity};

        if (schema->side_ != kMissingColumn) {
            const std::string_view side = get(columns, schema->side_);
            if (side == "B" || side == "b")
                output.side = data_tick_side::bid;
            else if (side == "A" || side == "a")
                output.side = data_tick_side::ask;
            else if (!side.empty())
                return {error::invalid_side, field::side};
        }
        return {};
    }

    [[nodiscard]] bool header_contains_record() const noexcept { return header_contains_record_; }

private:
    static std::string_view get(const split_fields& columns, std::size_t index) noexcept
    {
        return index == kMissingColumn ? std::string_view{} : columns.values[index];
    }

    void assign_column(std::string_view name, std::size_t index) noexcept
    {
        if (name == "timestamp_ms")
            timestamp_ = index;
        else if (name == "symbol")
            symbol_ = index;
        else if (name == "price")
            price_ = index;
        else if (name == "quantity")
            quantity_ = index;
        else if (name == "side")
            side_ = index;
    }

    void set_canonical(std::size_t count) noexcept
    {
        field_count_ = count;
        timestamp_ = 0;
        symbol_ = 1;
        price_ = 2;
        quantity_ = 3;
        side_ = count == 5 ? 4 : kMissingColumn;
        valid_ = true;
    }

    std::size_t field_count_ = 0;
    std::size_t timestamp_ = kMissingColumn;
    std::size_t symbol_ = kMissingColumn;
    std::size_t price_ = kMissingColumn;
    std::size_t quantity_ = kMissingColumn;
    std::size_t side_ = kMissingColumn;
    bool valid_ = false;
    bool header_contains_record_ = false;
};

}  // namespace tt::strict_market_csv
