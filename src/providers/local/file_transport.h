#pragma once

#include "providers/transport.h"

#include <fstream>
#include <filesystem>

class FileTransport : public IDataTransport
{
public:
	explicit FileTransport(std::filesystem::path path)
		: path_(std::move(path))
	{ }

	bool open() override
	{
		terminal_ = transport_terminal_status::unknown;
		if (file_.is_open())
		{
			file_.clear();
			file_.seekg(0);
			return file_.good();
		}
		file_.open(path_);
		if (!file_.good()) terminal_ = transport_terminal_status::failed;
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
		terminal_ = file_.eof() ? transport_terminal_status::clean_eof
		                       : transport_terminal_status::failed;
		return std::nullopt;
	}

	transport_terminal_status terminal_status() const override
	{
		return terminal_;
	}

private:
	std::filesystem::path path_;
	std::ifstream file_;
	transport_terminal_status terminal_ = transport_terminal_status::unknown;
};
