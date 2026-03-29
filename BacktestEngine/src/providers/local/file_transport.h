#pragma once

#include "providers/transport.h"

#include <fstream>
#include <filesystem>

// FileTransport: reads lines from a local file.
class FileTransport : public IDataTransport
{
public:
	explicit FileTransport(std::filesystem::path path)
		: path_(std::move(path))
	{ }

	bool open() override
	{
		if (file_.is_open())
		{
			// Already open — rewind to beginning so it can be re-read
			file_.clear();
			file_.seekg(0);
			return file_.good();
		}
		file_.open(path_);
		return file_.good();
	}

	void close() override
	{
		if (file_.is_open())
			file_.close();
	}

	bool is_open() const override
	{
		return file_.is_open();
	}

	std::optional<std::string> read_line() override
	{
		std::string line;
		if (std::getline(file_, line))
			return line;
		return std::nullopt;
	}

private:
	std::filesystem::path path_;
	std::ifstream file_;
};
