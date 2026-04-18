#pragma once

#include <string>
#include <string_view>
#include <optional>

template <typename T>
class IDataParser
{
public:
	virtual ~IDataParser() = default;

	virtual bool parse_header(const std::string& /*line*/) { return true; }

	virtual std::optional<T> parse_record(const std::string& line) = 0;

	virtual std::optional<T> parse_record(std::string_view line)
	{
		return parse_record(std::string(line));
	}
};
