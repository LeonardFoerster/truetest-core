#pragma once

// Compact, format-boundary provenance vocabulary for cold-path data audits.
// Keep this out of Bar/Tick: those records also cross streaming and hot paths.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace tt::data_provenance {

enum class rejection_stage : std::uint8_t
{
	parser,
	domain_validation
};

enum class rejection_reason : std::uint8_t
{
	none,
	empty_row,
	repeated_header,
	missing_required_field,
	malformed_numeric,
	invalid_volume,
	invalid_timestamp,
	invalid_symbol,
	non_finite_price,
	non_positive_price,
	high_below_low,
	open_outside_range,
	close_outside_range,
	negative_volume,
	non_positive_volume,
	zero_quantity_scale
};

enum class source_field : std::uint8_t
{
	none,
	date,
	symbol,
	open_time,
	open,
	high,
	low,
	close,
	volume
};

struct accepted_row
{
	std::size_t physical_row = 0;   // one-based; the header is row 1
	std::size_t accepted_index = 0; // zero-based parser-success ordinal
};

struct rejected_row
{
	std::size_t physical_row = 0;
	std::optional<std::size_t> accepted_index;
	rejection_stage stage = rejection_stage::parser;
	rejection_reason reason = rejection_reason::none;
	source_field field = source_field::none;
};

} // namespace tt::data_provenance
