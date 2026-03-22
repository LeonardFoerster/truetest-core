#pragma once

#include "providers/provider.h"
#include "providers/local/file_transport.h"

#include <filesystem>
#include <memory>
#include <string>

// LocalProvider: local file data feed + simulated orderbook execution.
// This is the default provider for backtesting.
class LocalProvider : public IProvider
{
public:
	explicit LocalProvider(std::filesystem::path data_path)
		: data_path_(std::move(data_path)) {}

	std::string name() const override { return "local"; }

	bool has_data_feed() const override { return true; }
	bool has_execution() const override { return false; }
	// Execution for local backtesting is handled by LocalBookAdapter
	// which is created per-symbol by the engine. The provider doesn't
	// own it because the engine manages the orderbook lifecycle.

	bool open() override
	{
		transport_ = std::make_shared<FileTransport>(data_path_);
		return transport_->open();
	}

	void close() override
	{
		if (transport_) transport_->close();
	}

	std::shared_ptr<IDataTransport> get_transport() override
	{
		return transport_;
	}

	std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
	{
		return nullptr;
	}

private:
	std::filesystem::path data_path_;
	std::shared_ptr<FileTransport> transport_;
};
