#pragma once

#include "transport.h"
#include "parser.h"
#include "provider_event.h"
#include "data/data_source.h"
#include "execution/execution_adapter.h"
#include "execution/instrument.h"
#include "execution/live_safety.h"

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

	// Per-symbol trading rules (tick/lot/min-notional/fees).
	// Return std::nullopt if the provider has no opinion — the engine will
	// fall back to user-supplied overrides or skip the checks entirely.
	virtual std::optional<instrument_spec>
	get_instrument(const std::string& /*symbol*/) const { return std::nullopt; }

	// Live-mode safety surfaces. Default to nullptr — engine installs
	// Noop* implementations when no provider-specific one exists, so live
	// runs against a half-wired provider fail-safe rather than no-op
	// silently. Venue providers override these.
	virtual std::shared_ptr<IReconciler> get_reconciler() { return nullptr; }
	virtual std::shared_ptr<IKillSwitch> get_kill_switch() { return nullptr; }

	// Unified event-stream hook. When a provider can emit a mixed stream
	// of bar / tick / l2_snapshot / l2_update events (typically required
	// for realistic shadow: real market data AND real exchange depth
	// driving the engine's orderbook_registry_), it sets this to true
	// and returns a parser that produces provider::event variants.
	//
	// The engine then uses a single DataBridge<provider::event> →
	// run_streaming path regardless of venue. Default false/null means
	// main.inc falls back to the existing specialized
	// DataBridge<bar_record>/DataBridge<tick_record> bridges.
	//
	// Each provider owns its own wire format, subscription topology
	// (single vs combined WebSocket), and reconnect logic — the engine
	// only sees the variant stream.
	virtual bool supports_event_stream() const { return false; }

	virtual std::shared_ptr<IDataParser<provider::event>>
	get_event_parser() { return nullptr; }
};
