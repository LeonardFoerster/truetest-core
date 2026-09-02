#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

// Trivially copyable, fixed-size ingress record for one observed public
// (market) trade print. footprint.md §2.1.
//
// Produced by the opt-in DataBridge research tap from an already-parsed
// provider::tick (see providers/data_bridge.h); consumed off the engine hot
// path by FootprintResearchService via the bounded SPSC research ring
// (providers/footprint/footprint_ring.h). This type is never constructed on
// the engine event loop and never allocates - it is copied by value through
// the ring, so it stays POD and cache-line-friendly.
namespace truetest::footprint {

// SymbolTable's canonical invalid uint16 id, repeated at this POD boundary so
// provider/UI footprint code need not depend on the concrete symbol registry.
inline constexpr std::uint16_t kInvalidSymbolId =
    std::numeric_limits<std::uint16_t>::max();

enum class aggressor_side : std::uint8_t
{
    unknown = 0,
    buy     = 1, // aggressor bought (hit the ask / lifted the offer)
    sell    = 2, // aggressor sold (hit the bid)
};

// Bitmask - provenance / quality flags carried alongside a trade. Multiple
// flags may be set (e.g. provenance_live | provenance_native_id).
enum provenance_flags : std::uint32_t
{
    provenance_none         = 0,
    provenance_live         = 1u << 0, // observed on the live stream
    provenance_history      = 1u << 1, // backfilled from REST/cold history
    provenance_replay       = 1u << 2, // raw-frame replay, no network (§2.2)
    provenance_native_id    = 1u << 3, // native_trade_id is venue-authoritative
    provenance_session_only = 1u << 4, // identity is (session_id, obs_seq) only
                                        // - no native id available (Bitunix)
    provenance_reorder_late = 1u << 5, // arrived after the normal sequence but
                                        // within the two-second reorder window
};

struct PublicTrade
{
    // Nanosecond timestamps. event_ns is the venue-reported (or best
    // estimate) trade time; recv_ns is local receive time, used for the
    // reorder-window and reconciliation bookkeeping (§2.2).
    std::int64_t event_ns = 0;
    std::int64_t recv_ns  = 0;

    // Native venue trade id when the venue provides one (Binance, Bitget).
    // 0 when absent - see provenance_native_id / provenance_session_only.
    std::uint64_t native_trade_id = 0;

    // Connection-session id + per-session observation sequence. Always
    // populated; obs_seq is monotonic within session_id. For venues without
    // a native trade id (Bitunix), (session_id, obs_seq) is the sole
    // identity, and a new session_id is an explicit continuity boundary.
    std::uint64_t session_id = 0;
    std::uint64_t obs_seq    = 0;

    // Exact integer price ticks and base-quantity atoms - never floating
    // point. Resolved once per (venue, symbol) via
    // footprint_venue_capabilities.h::resolve_footprint_tick_size, then
    // reused for every trade so the tap stays branch-light and alloc-free.
    std::int64_t price_ticks    = 0;
    std::int64_t base_qty_atoms = 0;

    // Pre-resolved venue and symbol ids (see footprint_venue_capabilities.h
    // and SymbolTable::intern_id - both interned once at startup, never on
    // this path).
    std::uint16_t venue_id  = 0;
    std::uint16_t symbol_id = kInvalidSymbolId;

    aggressor_side side = aggressor_side::unknown;
    std::uint8_t   reserved_ = 0; // explicit pad, keeps layout stable

    std::uint32_t flags = provenance_none;
};

static_assert(std::is_trivially_copyable_v<PublicTrade>,
              "PublicTrade must stay trivially copyable - it is copied by "
              "value through the SPSC research ring");
static_assert(sizeof(PublicTrade) <= 128,
              "PublicTrade grew unexpectedly large - check for accidental "
              "padding/added fields before raising this bound");

} // namespace truetest::footprint
