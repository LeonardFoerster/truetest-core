#pragma once

#include "providers/transport.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

// MockStreamingTransport: a streaming transport backed by a thread-safe queue.
// Useful for testing DataBridge streaming mode and transport contracts.
class MockStreamingTransport : public IDataTransport
{
public:
	bool open() override
	{
		std::lock_guard<std::mutex> lk(mu_);
		open_ = true;
		stopped_ = false;
		return true;
	}

	void close() override
	{
		std::lock_guard<std::mutex> lk(mu_);
		open_ = false;
	}

	bool is_open() const override
	{
		std::lock_guard<std::mutex> lk(mu_);
		// Stay open for drain after request_stop while queued lines remain.
		// DataBridge loops on is_open(); closing early drops pending frames
		// when the consumer is mid-callback (TSan/slow paths).
		return open_ || !lines_.empty();
	}

	std::optional<std::string> read_line() override
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (lines_.empty()) return std::nullopt;
		auto line = std::move(lines_.front());
		lines_.pop();
		return line;
	}

	bool is_streaming() const override { return true; }

	std::optional<std::string> read_line_blocking() override
	{
		std::unique_lock<std::mutex> lk(mu_);
		cv_.wait(lk, [this] { return !lines_.empty() || stopped_; });
		if (lines_.empty())
		{
			open_ = false;
			return std::nullopt;
		}
		auto line = std::move(lines_.front());
		lines_.pop();
		if (stopped_ && lines_.empty())
			open_ = false;
		return line;
	}

	void request_stop() override
	{
		std::lock_guard<std::mutex> lk(mu_);
		stopped_ = true;
		// Close only when queue is already empty; otherwise drain via reads.
		if (lines_.empty())
			open_ = false;
		cv_.notify_all();
	}

	// Enqueue a line for the consumer to read.
	void enqueue(const std::string& line)
	{
		std::lock_guard<std::mutex> lk(mu_);
		lines_.push(line);
		cv_.notify_one();
	}

private:
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::queue<std::string> lines_;
	bool open_ = false;
	bool stopped_ = false;
};

// MockBatchTransport: a non-streaming transport backed by a vector of lines.
class MockBatchTransport : public IDataTransport
{
public:
	explicit MockBatchTransport(std::vector<std::string> lines)
		: lines_(std::move(lines)) {}

	bool open() override { open_ = true; idx_ = 0; return true; }
	void close() override { open_ = false; }
	bool is_open() const override { return open_; }

	std::optional<std::string> read_line() override
	{
		if (idx_ < lines_.size())
			return lines_[idx_++];
		return std::nullopt;
	}

private:
	std::vector<std::string> lines_;
	std::size_t idx_ = 0;
	bool open_ = false;
};
