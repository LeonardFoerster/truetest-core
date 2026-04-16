#pragma once

#include "transport.h"
#include "provider_event.h"
#include "data/data_source.h"
#include "execution/execution_adapter.h"

#include <memory>
#include <string>

class engine_config;

// IProvider: a venue that can deliver market data AND accept orders.
//
// Not all providers support both — a CSV file is data-only, a paper
// trading engine is execution-only. Use the capability queries to check.
//
// Providers own their connection lifecycle. open() establishes the
// connection (or opens the file), close() tears it down.
class IProvider
{
public:
	// Connection lifecycle state, queryable by the engine for logging.
	enum class lifecycle
	{
		closed,
		opening,
		open,
		error
	};

	virtual ~IProvider() = default;

	// Identity
	virtual std::string name() const = 0;

	// Capability queries
	virtual bool has_data_feed() const = 0;
	virtual bool has_execution() const = 0;

	// Lifecycle
	virtual bool open() = 0;
	virtual void close() = 0;

	// Current lifecycle state. Default implementation never reports any
	// state — providers that care should track this themselves.
	virtual lifecycle lifecycle_state() const { return lifecycle::closed; }

	// Configure the provider with engine-level settings (mode, fee/fill
	// models, backfill, execution constants). Called by the host before
	// open(). Default implementation is a no-op; providers that embed
	// execution adapters (e.g. BinanceProvider's hybrid executor) override
	// this to capture the inputs they need.
	virtual void configure(const engine_config&) {}

	// Notify the provider of a mid-price update for a symbol. Used by
	// providers that maintain internal book-seeded execution adapters;
	// the default is a no-op.
	virtual void on_mid_price(const std::string& /*symbol*/,
	                          double /*mid_price*/) {}

	// Data feed: returns a transport that the bridge can consume.
	// Returns nullptr if !has_data_feed().
	virtual std::shared_ptr<IDataTransport> get_transport() = 0;

	// Execution: returns an adapter the engine can submit orders to.
	// Returns nullptr if !has_execution().
	virtual std::shared_ptr<IExecutionAdapter> get_execution_adapter() = 0;
};
