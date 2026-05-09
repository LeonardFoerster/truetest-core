#pragma once

#include "transport.h"
#include "parser.h"
#include "provider_event.h"
#include "data/data_source.h"
#include "execution/execution_adapter.h"
#include "execution/instrument.h"
#include "execution/live_safety.h"
#include "risk/futures_risk_check.h"
#include "exits/bracket_adapter.h"

#include <memory>
#include <optional>
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

	// nullopt → engine falls back to user overrides or skips checks.
	virtual std::optional<instrument_spec>
	get_instrument(const std::string& /*symbol*/) const { return std::nullopt; }

	// nullptr → engine installs Noop* safety shims; venue providers override.
	virtual std::shared_ptr<IReconciler> get_reconciler() { return nullptr; }
	virtual std::shared_ptr<IKillSwitch> get_kill_switch() { return nullptr; }

	// nullptr → engine skips the venue-specific pre-trade check (the
	// venue-agnostic RiskManager always runs). Futures providers
	// override to enforce notional / leverage / liquidation-distance
	// caps. See risk/futures_risk_check.h.
	virtual std::shared_ptr<IRiskCheck> get_risk_check() { return nullptr; }

	// nullptr → ExitManager runs engine-side eval only (current behavior,
	// always used by backtest/shadow). Live providers override to push
	// brackets to the venue as resting orders. See exits/bracket_adapter.h.
	virtual std::shared_ptr<truetest::exits::IBracketAdapter>
	get_bracket_adapter() { return nullptr; }

	// True → provider emits a unified bar/tick/l2 variant stream. False
	// falls back to the specialized bar_record/tick_record bridges.
	virtual bool supports_event_stream() const { return false; }

	virtual std::shared_ptr<IDataParser<provider::event>>
	get_event_parser() { return nullptr; }
};
