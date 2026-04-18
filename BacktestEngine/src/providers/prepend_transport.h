#pragma once

#include "transport.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class PrependTransport : public IDataTransport
{
public:
	PrependTransport(std::shared_ptr<IDataTransport> inner,
	                 std::vector<std::string> prepend_lines)
		: inner_(std::move(inner))
		, prepend_(std::move(prepend_lines))
	{ }

	bool open() override
	{
		return inner_ ? inner_->open() : false;
	}

	void close() override
	{
		if (inner_) inner_->close();
	}

	bool is_open() const override
	{
		return inner_ && inner_->is_open();
	}

	bool is_streaming() const override
	{
		return inner_ && inner_->is_streaming();
	}

	std::optional<std::string> read_line() override
	{
		if (idx_ < prepend_.size())
			return prepend_[idx_++];
		return inner_ ? inner_->read_line() : std::nullopt;
	}

	std::optional<std::string> read_line_blocking() override
	{
		if (idx_ < prepend_.size())
			return prepend_[idx_++];
		return inner_ ? inner_->read_line_blocking() : std::nullopt;
	}

	bool read_frame(std::string_view& out) override
	{
		if (idx_ < prepend_.size())
		{
			out = prepend_[idx_++];
			return true;
		}
		return inner_ ? inner_->read_frame(out) : false;
	}

	bool read_frame_blocking(std::string_view& out) override
	{
		if (idx_ < prepend_.size())
		{
			out = prepend_[idx_++];
			return true;
		}
		return inner_ ? inner_->read_frame_blocking(out) : false;
	}

	void request_stop() override
	{
		if (inner_) inner_->request_stop();
	}

private:
	std::shared_ptr<IDataTransport> inner_;
	std::vector<std::string> prepend_;
	std::size_t idx_ = 0;
};
