#pragma once

#include "transport.h"
#include "provider_event.h"
#include "data/data_source.h"
#include "execution/execution_adapter.h"

#include <memory>
#include <string>

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
	virtual ~IProvider() = default;

	// Identity
	virtual std::string name() const = 0;

	// Capability queries
	virtual bool has_data_feed() const = 0;
	virtual bool has_execution() const = 0;

	// Lifecycle
	virtual bool open() = 0;
	virtual void close() = 0;

	// Data feed: returns a transport that the bridge can consume.
	// Returns nullptr if !has_data_feed().
	virtual std::shared_ptr<IDataTransport> get_transport() = 0;

	// Execution: returns an adapter the engine can submit orders to.
	// Returns nullptr if !has_execution().
	virtual std::shared_ptr<IExecutionAdapter> get_execution_adapter() = 0;
};
