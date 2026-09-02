#pragma once

#include "data/market_series.h"
#include "data/symbol_validation.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tt::api {

enum class series_symbol_error
{
    none,
    invalid_expected_symbol,
    unbound_without_expected_symbol,
    expected_symbol_mismatch,
};

struct series_symbol_result
{
    series_symbol_error error = series_symbol_error::none;
    std::string_view observed_symbol;
    MarketSeries::symbol_binding bound;

    [[nodiscard]] bool success() const noexcept { return error == series_symbol_error::none; }
};

// Venue-agnostic identity validation deliberately preserves case and
// punctuation. Empty identities and embedded ASCII control/whitespace bytes
// are outside the C API's supported instrument domain.
inline bool valid_expected_symbol(std::string_view symbol) noexcept
{
    return tt::symbol_validation::valid(symbol);
}

// Apply the C API's fail-closed dataset identity policy. All named records are
// checked before any blank record is bound, so a mismatch causes zero mutation.
// A missing expected symbol is valid only when the dataset already names every
// accepted record. This identity policy preserves named multi-symbol input; it
// makes no claim about downstream multi-symbol analytics correctness.
inline series_symbol_result
enforce_series_symbol_policy(MarketSeries& series, std::optional<std::string_view> expected_symbol)
{
    if (!expected_symbol) {
        if (series.has_unbound_symbols()) {
            series_symbol_result result;
            result.error = series_symbol_error::unbound_without_expected_symbol;
            return result;
        }
        return {};
    }
    if (!valid_expected_symbol(*expected_symbol)) {
        series_symbol_result result;
        result.error = series_symbol_error::invalid_expected_symbol;
        return result;
    }

    for (std::size_t i = 0; i < series.bar_count(); ++i) {
        const std::string_view observed = series.bar_symbol_at(i);
        if (!observed.empty() && observed != *expected_symbol) {
            series_symbol_result result;
            result.error = series_symbol_error::expected_symbol_mismatch;
            result.observed_symbol = observed;
            return result;
        }
    }
    for (std::size_t i = 0; i < series.tick_count(); ++i) {
        const std::string_view observed = series.tick_at(i).symbol;
        if (!observed.empty() && observed != *expected_symbol) {
            series_symbol_result result;
            result.error = series_symbol_error::expected_symbol_mismatch;
            result.observed_symbol = observed;
            return result;
        }
    }

    series_symbol_result result;
    result.bound = series.bind_unset_symbols(std::string{*expected_symbol});
    return result;
}

}  // namespace tt::api
