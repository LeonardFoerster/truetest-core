#pragma once

#include "transport.h"
#include "parser.h"
#include "provider_event.h"
#include "market_data_feed.h"
#include "data/data_source.h"
#include "execution/execution_adapter.h"
#include "execution/instrument.h"
#include "execution/live_safety.h"
#include "risk/futures_risk_check.h"
#include "exits/bracket_adapter.h"
#include "threading/ring_buffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

class engine_config;

enum class live_shutdown_disposition
{
    disarm_after_kill,
    preserve_dead_man_switch
};

// Shared, compile-time-typed shutdown sequence for futures providers.  Keeping
// this order in one place makes the concrete Binance/Bitget implementations
// directly characterizable without replacing their production resource
// types.  These operations are cold-path and must remain non-retrying.
template <typename DeadMansSwitch, typename Bridge,
          typename PrivateTransport, typename PublicTransport>
inline void quiesce_futures_live_resources(
    const std::shared_ptr<std::atomic<bool>>& mutations_cancelled,
    const std::shared_ptr<DeadMansSwitch>& dms,
    const std::shared_ptr<Bridge>& bridge,
    const std::shared_ptr<PrivateTransport>&,
    const std::shared_ptr<PublicTransport>& public_transport)
{
    mutations_cancelled->store(true, std::memory_order_release);
    std::exception_ptr first_failure;
    const auto attempt = [&first_failure](auto&& operation) {
        try { operation(); }
        catch (...) {
            if (!first_failure) first_failure = std::current_exception();
        }
    };
    if (dms) attempt([&] { dms->request_stop(); });
    if (bridge) attempt([&] { bridge->quiesce(); });
    if (public_transport)
        attempt([&] { public_transport->request_stop(); });
    if (first_failure) std::rethrow_exception(first_failure);
}

template <typename DeadMansSwitch, typename Bridge,
          typename PrivateTransport, typename PublicTransport>
inline bool finish_futures_live_resources(
    const std::shared_ptr<DeadMansSwitch>& dms,
    const std::shared_ptr<Bridge>& bridge,
    const std::shared_ptr<PrivateTransport>& private_transport,
    const std::shared_ptr<PublicTransport>& public_transport,
    live_shutdown_disposition disposition)
{
    bool finished = true;
    const auto attempt = [&finished](auto&& operation) {
        try { operation(); }
        catch (...) { finished = false; }
    };
    if (dms) attempt([&] { dms->stop(); });
    if (bridge) attempt([&] { bridge->close(); });
    if (private_transport) attempt([&] { private_transport->close(); });
    if (public_transport) attempt([&] { public_transport->close(); });

    // A local finish failure leaves ownership/lifetime uncertain.  Preserve
    // the venue countdown in that case even when kill itself succeeded.
    if (dms && finished
        && disposition == live_shutdown_disposition::disarm_after_kill)
        attempt([&] {
            if (!dms->disarm()) finished = false;
        });
    return finished;
}

// DMS callbacks run on the heartbeat thread. They must latch the engine halt
// before waking the blocking data stream; teardown and the one kill path then
// remain owned by the engine thread. Keeping this wiring shared prevents
// futures providers from drifting into a join/flatten callback on the DMS
// thread.
template <typename DeadMansSwitch>
inline void wire_dms_failure_to_engine(
    const std::shared_ptr<DeadMansSwitch>& dms,
    std::function<void(std::string_view)> halt,
    std::shared_ptr<IDataTransport> transport)
{
    if (!dms) return;
    dms->set_failure_callback(
        [halt = std::move(halt), transport = std::move(transport)](
            std::string_view reason) {
            if (halt) halt(reason);
            if (transport) transport->request_stop();
        });
}

// Fixed-value handoff from a provider's private account stream to the engine
// event loop.  It deliberately owns no heap memory: the private WS reader is
// the sole producer, and only the engine thread may turn this into a pooled
// funding_event / mutate portfolio and audit state.
struct provider_funding_update
{
    enum class reason : std::uint8_t { funding_fee };
    static constexpr std::size_t symbol_capacity = 31;

    std::int64_t event_time_ms = 0;
    double cash_delta = 0.0;
    std::array<char, symbol_capacity + 1> symbol{};
    std::uint8_t symbol_size = 0;
    reason why = reason::funding_fee;

    std::string_view symbol_view() const noexcept
    {
        return {symbol.data(), symbol_size};
    }
};

static_assert(std::is_trivially_copyable_v<provider_funding_update>);

// One producer (the provider's private WS reader), one consumer (the engine
// event loop).  Full or malformed input latches terminal failure; there is no
// retry, growth, overwrite, or drop-oldest path.
class ProviderFundingIngress
{
public:
    static constexpr std::size_t capacity = 64;

    bool try_publish(std::chrono::system_clock::time_point ts,
                     std::string_view symbol,
                     double cash_delta) noexcept
    {
        provider_funding_update update;
        update.event_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            ts.time_since_epoch()).count();
        update.cash_delta = cash_delta;
        if (failed_.load(std::memory_order_acquire)
            || update.event_time_ms <= 0
            || !std::isfinite(cash_delta)
            || cash_delta == 0.0
            || symbol.empty()
            || symbol.size() > provider_funding_update::symbol_capacity)
        {
            failed_.store(true, std::memory_order_release);
            return false;
        }
        std::copy(symbol.begin(), symbol.end(), update.symbol.begin());
        update.symbol_size = static_cast<std::uint8_t>(symbol.size());
        if (!ring_.try_push(update))
        {
            failed_.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    bool try_pop(provider_funding_update& update) noexcept
    {
        return ring_.try_pop(update);
    }

    bool empty() const noexcept
    {
        return ring_.empty();
    }

    bool failed() const noexcept
    {
        return failed_.load(std::memory_order_acquire);
    }

    void latch_failure() noexcept
    {
        failed_.store(true, std::memory_order_release);
    }

private:
    RingBuffer<provider_funding_update, capacity> ring_;
    std::atomic<bool> failed_{false};
};

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
	virtual void quiesce_for_live_shutdown() {}
	virtual void finish_live_shutdown(live_shutdown_disposition) { close(); }

	virtual lifecycle lifecycle_state() const { return lifecycle::closed; }

	virtual void configure(const engine_config&) {}

	virtual void on_mid_price(const std::string& /*symbol*/,
	                          double /*mid_price*/) {}

	virtual std::shared_ptr<IDataTransport> get_transport() = 0;

	virtual std::shared_ptr<IExecutionAdapter> get_execution_adapter() = 0;

	// nullopt -> engine falls back to user overrides or skips checks.
	virtual std::optional<instrument_spec>
	get_instrument(const std::string& /*symbol*/) const { return std::nullopt; }

	// Live reconciliation is default-refuse; venue providers must override.
	virtual std::shared_ptr<IReconciler> get_reconciler() { return nullptr; }
	virtual std::shared_ptr<IKillSwitch> get_kill_switch() { return nullptr; }

	// nullptr -> engine skips the venue-specific pre-trade check (the
	// venue-agnostic RiskManager always runs). Futures providers
	// override to enforce notional / leverage / liquidation-distance
	// caps. See risk/futures_risk_check.h.
	virtual std::shared_ptr<IRiskCheck> get_risk_check() { return nullptr; }

	// nullptr -> ExitManager runs engine-side eval only (current behavior,
	// always used by backtest/shadow). Live providers override to push
	// brackets to the venue as resting orders. See exits/bracket_adapter.h.
	virtual std::shared_ptr<truetest::exits::IBracketAdapter>
	get_bracket_adapter() { return nullptr; }

	// True -> provider emits a unified bar/tick/l2 variant stream. False
	// falls back to the specialized bar_record/tick_record bridges.
	virtual bool supports_event_stream() const { return false; }

	virtual std::shared_ptr<IDataParser<provider::event>>
	get_event_parser() { return nullptr; }

	// The transport and parser of an event stream are an inseparable public
	// market-data feed. The default preserves every existing provider: only an
	// already-advertised, complete event stream is bundled. New providers may
	// override this to publish explicit request/capability metadata.
	virtual std::optional<MarketDataFeed> get_market_data_feed()
	{
		if (!supports_event_stream())
			return std::nullopt;

		MarketDataFeed feed;
		feed.transport = get_transport();
		feed.parser = get_event_parser();
		if (!feed.ready())
			return std::nullopt;
		return feed;
	}

	// Long-lived threads owned by the provider can advertise themselves
	// here so the engine's WorkerWatchdog halts the engine if any of
	// them goes silent. Empty default -> no liveness monitoring (engine
	// won't even create a watchdog). The atomic must outlive both this
	// provider and the engine - the same lifetime constraint Worker's
	// failure_flag follows.
	struct liveness_source
	{
		std::string name;
		std::atomic<int64_t>* last_alive_ms = nullptr;
		int64_t deadline_ms = 0;
	};
	virtual std::vector<liveness_source> get_liveness_sources() { return {}; }

	// Engine wires this in live mode so a fatal transport disconnect (WS
	// idle timeout, listenKey HTTP failure beyond retry budget) routes
	// straight into engine::trigger_halt instead of the transport's
	// reconnect loop. Default no-op - backtest/shadow paths keep their
	// reconnect behavior. The reason string is short ("market-data WS
	// lost") and is published to the dashboard banner verbatim.
	virtual void set_halt_callback(
			std::function<void(std::string_view reason)> /*cb*/) {}

	// Optional fixed-capacity account-event ingress. The provider's private WS
	// thread is the sole producer; the engine event loop is the sole consumer.
	// The engine caches this pointer at construction, avoiding capability and
	// virtual poll calls on every market event. Provider/session ownership keeps
	// the ingress alive until the final post-close drain.
	virtual ProviderFundingIngress* funding_ingress() noexcept { return nullptr; }
};
