#pragma once

#include "../core/event.h"
#include "private_execution_record.h"

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
    bool        uncertain = false;
    bool        fatal = false;
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

// Result of engine-thread resolution of one record taken from the provider's
// private FIFO.  The private reader only parses and publishes; all bridge map
// mutation, terminal retirement, and venue-bracket attribution happen on the
// engine thread after source order has been fixed by the ingress.
enum class private_execution_resolution : std::uint8_t
{
    tracked,
    untracked,
    duplicate,
    fatal,
};

// Opaque engine-thread proof returned implicitly by a successful private
// record resolution.  The source sequence is globally monotonic for one
// provider ingress and the engine id binds that sequence to exactly one
// tracked order.  A caller may commit or roll back only this exact pair;
// guessing a later sequence must fail closed.
struct private_execution_reservation
{
    std::uint64_t source_sequence = 0;
    std::uint64_t engine_order_id = 0;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return source_sequence != 0 && engine_order_id != 0;
    }
};

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

    // Resolve a provider-private record after the engine has popped it from
    // the SPSC ingress.  On `tracked`, `record.engine_order_id` and
    // `record.remaining_qty` are populated.  `duplicate` is an exact no-op;
    // `untracked` is reserved for a typed venue-managed leg that ExitManager
    // must resolve; `fatal` means identity/cumulative proof failed and order
    // admission has already been closed.
    virtual private_execution_resolution
    resolve_private_execution(private_execution_record&)
    {
        return private_execution_resolution::fatal;
    }

    // Resolution is deliberately a prepare step.  A concrete bridge must
    // not advance cumulative quantities, replay history, or terminal map
    // retirement until the engine has completed canonical accounting.  Test
    // doubles must explicitly override these methods when used with a unified
    // ingress.  The fail-closed defaults prevent a future adapter from
    // accidentally claiming source-of-truth lifecycle proof by inheritance.
    virtual bool commit_private_execution(
        const private_execution_reservation& /*reservation*/)
    {
        return false;
    }

    virtual bool rollback_private_execution(
        const private_execution_reservation& /*reservation*/)
    {
        return false;
    }

    // Commits retirement of a terminal record only after the engine has
    // accounted/audited it.  This is the bridge half of the proof that a
    // terminal mapping cannot disappear before accounting succeeds.
    virtual bool acknowledge_private_terminal(std::uint64_t /*sequence*/)
    {
        return false;
    }

    // Returns false once a REST-cancel acknowledgement has outlived its
    // bounded private-terminal confirmation window.  Engine polls it from
    // normal and final drain maintenance; failure is terminal/reconcile.
    virtual bool check_private_lifecycle_deadline()
    {
        return true;
    }

    // Used only after the private producer has joined at shutdown.  A clean
    // final drain is impossible while a REST cancel acknowledgement or a
    // terminal record still awaits its engine-side commit.
    virtual bool has_unresolved_private_lifecycle() const
    {
        return true;
    }

    // Engine-side validation can discover a broken sequence or an
    // unresolvable record after the reader successfully published it.  Close
    // the bridge's venue-admission gate synchronously before the engine
    // begins its accounting-only drain; this is distinct from merely setting
    // the provider ring failure flag.
    virtual void fail_private_execution_admission() {}
};
