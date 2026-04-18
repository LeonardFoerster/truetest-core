#pragma once

#include "transport.h"
#include "provider_event.h"
#include "data/data_source.h"
#include "execution/execution_adapter.h"

#include <memory>
#include <string>

class engine_config;

class IProvider
{
public:
	enum class lifecycle
	{
		closed,
		opening,
		open,
		error
	};

	virtual ~IProvider() = default;

	virtual std::string name() const = 0;

	virtual bool has_data_feed() const = 0;
	virtual bool has_execution() const = 0;

	virtual bool open() = 0;
	virtual void close() = 0;

	virtual lifecycle lifecycle_state() const { return lifecycle::closed; }

	virtual void configure(const engine_config&) {}

	virtual void on_mid_price(const std::string& /*symbol*/,
	                          double /*mid_price*/) {}

	virtual std::shared_ptr<IDataTransport> get_transport() = 0;

	virtual std::shared_ptr<IExecutionAdapter> get_execution_adapter() = 0;
};
