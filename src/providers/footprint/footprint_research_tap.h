#pragma once

#include "providers/footprint/footprint_ring.h"
#include "providers/footprint/footprint_venue_capabilities.h"
#include "providers/provider_event.h"
#include "types/public_trade.h"

#include <chrono>
#include <cmath>
#include <cstdint>

// The opt-in DataBridge research tap: converts an already-parsed
// provider::tick into a PublicTrade and try_push()es it into the bounded
// SPSC research ring. footprint.md §2.1: "It may only convert the
// already-parsed tick into PublicTrade and try_push it into a prewarmed
// bounded SPSC ring. It must not allocate, lock, format, log, retry,
// aggregate, or block."
//
// Every function here is safe to call from the hot handoff point right
// after a tick is parsed - no heap, no I/O, no throwing paths taken on the
// steady-state route.
namespace truetest::footprint {

// One instance per (venue, symbol) stream, owned by whatever wires the tap
// to a provider connection (cold-path setup, outside this header). Not
// thread-safe by design - the tap has exactly one producer (§2.1).
struct FootprintTapContext
{
    venue_id venue = venue_id::unknown;
    std::uint16_t symbol_id = 0;

    // Bumped by the owner on every new connection/reconnect - an explicit
    // continuity boundary, especially for venues without a native trade id
    // (Bitunix, §2.1). obs_seq resets to 0 whenever session_id changes.
    std::uint64_t session_id = 0;
    std::uint64_t next_obs_seq = 0;

    // Resolved once at startup via resolve_footprint_tick_size(); 0 means
    // "unresolved" and the tap will no-op rather than guess (§2.1 "make the
    // footprint unavailable rather than guessing").
    double tick_size = 0.0;

    // Base-quantity atoms per whole unit (e.g. 1e8 for a satoshi-like base
    // asset). Only consulted for ticks that did not arrive with an exact
    // base_qty_atoms already populated (has_exact_decimal == false).
    double qty_atom_scale = 1.0;
};

// True when ctx is ready to tap trades (tick size resolved). Callers should
// gate on this once at wiring time; the tap itself also re-checks so a
// misuse never silently fabricates ticks against a zero tick size.
inline bool tap_context_ready(const FootprintTapContext& ctx) noexcept
{
    return ctx.tick_size > 0.0;
}

// Converts a parsed tick into a PublicTrade using ctx. Does not push -
// separated from try_tap_push() so tests can inspect the conversion result
// directly without a ring. Pure/deterministic: recv_ns is left equal to
// event_ns here (no clock read), since a unit test calling this directly
// has no real "local receive time" to report either. try_tap_push() below
// is the actual production entry point and overwrites recv_ns with a real
// clock sample - see its comment.
//
// Precision: when the tick already carries exact integer fields
// (has_exact_decimal - populated by a venue parser that decoded the raw
// decimal string directly, §2.1), those are used verbatim - no floating
// point touches the price/quantity. Otherwise price_ticks/base_qty_atoms
// are derived from the double price/quantity via ctx.tick_size /
// ctx.qty_atom_scale; this fallback exists for venues/tests that have not
// yet been wired for exact-decimal parsing and must not be relied on for
// production tick-grouping precision (see footprint.md §2.1 tick-size note).
inline PublicTrade tick_to_public_trade(const FootprintTapContext& ctx,
                                         const provider::tick& t) noexcept
{
    PublicTrade out;
    out.event_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t.timestamp.time_since_epoch()).count();
    out.recv_ns = out.event_ns; // placeholder here - try_tap_push() overwrites
    out.session_id = ctx.session_id;
    out.venue_id = static_cast<std::uint16_t>(ctx.venue);
    out.symbol_id = ctx.symbol_id;
    out.side = t.side == 0 ? aggressor_side::buy
             : t.side == 1 ? aggressor_side::sell
                            : aggressor_side::unknown;

    if (t.has_exact_decimal)
    {
        out.price_ticks = t.price_ticks;
        out.base_qty_atoms = t.base_qty_atoms;
    }
    else if (ctx.tick_size > 0.0)
    {
        out.price_ticks = static_cast<std::int64_t>(
            std::llround(t.price / ctx.tick_size));
        out.base_qty_atoms = static_cast<std::int64_t>(
            std::llround(static_cast<double>(t.quantity) * ctx.qty_atom_scale));
    }
    // else: tick_size unresolved and no exact decimal on the tick - leave
    // price_ticks/base_qty_atoms at 0. try_tap_push() below refuses to push
    // this case (ctx not ready) rather than emit a zero-price trade.

    out.flags = provenance_live;
    if (t.native_trade_id != 0)
    {
        out.native_trade_id = t.native_trade_id;
        out.flags |= provenance_native_id;
    }
    else
    {
        out.flags |= provenance_session_only;
    }

    return out;
}

// Full tap: convert + try_push. Mutates ctx.next_obs_seq (single-writer,
// producer-owned - safe because the tap has exactly one caller). Returns
// false without pushing when the context is not ready (tick_size
// unresolved) or the ring was full (ring.discontinuous() will be set).
//
// recv_ns is stamped here (not in tick_to_public_trade) with the local wall
// clock at the moment this tap actually observes the parsed record - the
// point the reorder window (§2.2) needs, distinct from the venue-reported
// event_ns. A clock read is not an allocation/lock/retry and stays within
// the tap's "must not allocate, lock, format, log, retry, aggregate, or
// block" contract.
template <std::size_t N>
inline bool try_tap_push(FootprintTapContext& ctx,
                          const provider::tick& t,
                          FootprintResearchRing<N>& ring) noexcept
{
    if (!t.has_exact_decimal && !tap_context_ready(ctx))
        return false;

    PublicTrade trade = tick_to_public_trade(ctx, t);
    trade.recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    trade.obs_seq = ctx.next_obs_seq++;
    return ring.try_push(trade);
}

} // namespace truetest::footprint
