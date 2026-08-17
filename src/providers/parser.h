#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class empty_parse_status
{
	ignored,
	malformed
};

template <typename T>
class IDataParser
{
public:
	virtual ~IDataParser() = default;

	virtual bool parse_header(const std::string& /*line*/) { return true; }
	virtual bool header_frame_contains_records() const { return true; }

	virtual std::optional<T> parse_record(const std::string& line) = 0;

	virtual std::optional<T> parse_record(std::string_view line)
	{
		return parse_record(std::string(line));
	}

	// Multi-record frames (e.g. Bitget publicTrade data[] with N trades).
	// Default: single parse_record. Override to emit every element of a batch.
	// DataBridge prefers this over parse_record so venues with batched
	// trade frames do not drop all but the first element.
	virtual std::vector<T> parse_records(std::string_view line)
	{
		std::vector<T> out;
		if (auto r = parse_record(line))
			out.push_back(std::move(*r));
		return out;
	}

	virtual std::vector<T> parse_records(const std::string& line)
	{
		return parse_records(std::string_view{line});
	}

	// Empty output is ambiguous for streaming parsers. Control acknowledgements
	// are valid no-data frames; all other empty results fail closed by default.
	virtual empty_parse_status classify_empty_frame(std::string_view) const
	{
		// Fail closed. Venue parsers must positively recognize their own
		// subscription/control frames instead of relying on text heuristics.
		return empty_parse_status::malformed;
	}
};
