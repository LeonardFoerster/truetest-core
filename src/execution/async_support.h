#pragma once

#include "../core/event.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Forward declaration. Full definition lives in fill_parser.h (used only
// by bridge + handler registration sites that already include the parser).
struct parsed_exec;

// Top-level result / meta types extracted from ExecutionBridge so that
// engine, router and other consumers can name them without depending on
// the concrete bridge type. These are binary-compatible with the previous
// nested structs.
//
// ExecutionBridge provides using-aliases for source compatibility during
// the transition; direct test code may continue to use ExecutionBridge::
// forms until a follow-up cleanup removes the aliases.

struct submit_result
{
    enum class operation
    {
        submit,
        cancel
    };

    uint64_t    engine_id = 0;
    std::string symbol;
    std::string exchange_order_id;
    std::string error;
    operation   op = operation::submit;
    bool        ok = false;
};

struct synth_result
{
    fill_event    fill;
    std::uint64_t opener_order_id = 0;
    std::string   strategy_name;
};

struct synth_meta
{
    std::uint64_t engine_order_id  = 0;
    std::uint64_t opener_order_id  = 0;
    std::string   strategy_name;
};

using unknown_fill_handler =
    std::function<std::optional<synth_result>(const parsed_exec&,
                                              std::uint64_t fill_id)>;

// Narrow capability interface for adapters that support async submit
// acknowledgement + venue-managed bracket leg synthesis (currently only
// ExecutionBridge for live Binance providers).
//
// Engine code obtains this via IExecutionAdapter::get_async_support().
// Implementations must be thread-safe for the documented call sites
// (poll_* are called from the engine thread; set_* may be called at wiring
// time from main thread; handler is invoked on fill-transport worker).
class IAsyncSubmitSupport
{
public:
    virtual ~IAsyncSubmitSupport() = default;

    virtual void set_unknown_fill_handler(unknown_fill_handler h) = 0;

    // Clear any previously installed handler. Must be called by engine
    // on all stop/halt paths before worker threads or the provider's
    // fill transport may still be running. Prevents the handler from
    // being invoked after engine (or the objects it closes over) has
    // been destroyed.
    virtual void clear_unknown_fill_handler() = 0;

    // Engine drains this BEFORE poll_fills so order_meta_ has the
    // mapping ready when lookup_opener fires inside the fill loop.
    virtual bool poll_synth_meta(std::vector<synth_meta>& out) = 0;

    // New: drain results from async submits/cancels.
    // Engine should call this after submit_order (and periodically).
    // Mirrors the poll_fills pattern used for incoming fills.
    virtual bool poll_submit_results(std::vector<submit_result>& out) = 0;
};
