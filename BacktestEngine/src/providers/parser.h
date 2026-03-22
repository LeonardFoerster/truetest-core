#pragma once

#include <string>
#include <optional>

// IDataParser<T>: handles *how* raw data is structured.
// Converts raw text lines into typed records of type T.
//
// Usage:
//   parser.parse_header(first_line);        // configure columns, etc.
//   while (auto record = parser.parse_record(next_line)) { ... }
//
template <typename T>
class IDataParser
{
public:
	virtual ~IDataParser() = default;

	// Parse a header line to discover column layout. Returns false on error.
	// Not all formats have headers — default implementation is a no-op.
	virtual bool parse_header(const std::string& /*line*/) { return true; }

	// Parse a single data line into a record of type T.
	// Returns nullopt if the line should be skipped (empty, malformed).
	virtual std::optional<T> parse_record(const std::string& line) = 0;
};
