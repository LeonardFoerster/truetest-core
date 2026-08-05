#pragma once

#include "types/public_trade.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Optional venue-capability surface for the footprint subsystem. footprint.md
// §2.1: "Extend venue registration with optional FootprintVenueCapabilities,
// instrument-metadata, and IPublicTradeHistorySource factories. Do not add
// research methods to frozen IProvider or modify engine.cpp."
//
// Providers that want to participate opt in by *additionally* implementing
// IFootprintCapableProvider (multiple inheritance alongside IProvider) and
// registering the object under the existing ProviderRegistry factory as
// usual. The footprint service discovers the extra interface via
// dynamic_pointer_cast<IFootprintCapableProvider>(provider) - IProvider
// itself never learns this interface exists, and engine.cpp is untouched.
namespace truetest::footprint {

// Stable small id per supported venue. Values are append-only; never reuse
// or renumber - ids are persisted in cache segment manifests (§2.2).
enum class venue_id : std::uint16_t
{
    unknown         = 0,
    binance_usdm    = 1,
    bitget_usdtm    = 2,
    bitunix_futures = 3,
};

// What a venue can offer the footprint subsystem for a given symbol. All
// fields default to "cannot" - a capability struct with every field false
// degrades to "history/live unavailable" rather than silently guessing.
struct FootprintVenueCapabilities
{
    venue_id venue = venue_id::unknown;

    // Live stream carries a native, venue-assigned trade id we can dedupe on.
    bool has_native_trade_id_live = false;

    // A REST/history endpoint exists that returns exact raw trades (not
    // aggregated) for backfill, subject to the venue's own range/rate limits.
    bool has_public_trade_history = false;

    // History endpoint requires authenticated (read-only) credentials.
    bool history_requires_credentials = false;

    // History is bounded to a short recent window (e.g. a "recent fills"
    // endpoint) rather than the full requested lookback.
    bool history_is_recent_only = false;

    // Live identity is only a reconnect-session boundary, not a stitchable
    // trade id (Bitunix) - reconnects must surface as explicit gaps, never
    // be silently bridged.
    bool identity_is_session_only = false;
};

// Public-trade history fetch surface a venue may optionally provide. Cold
// path only (REST calls); never invoked from the engine event loop.
class IPublicTradeHistorySource
{
public:
    virtual ~IPublicTradeHistorySource() = default;

    // [start_ns, end_ns) half-open. Returns false (and leaves out untouched)
    // on failure so callers can distinguish "no data in range" from "call
    // failed" - reconciliation (§2.2) treats a failed fetch as RECOVERING,
    // a true-but-empty result as a confirmed empty interval.
    virtual bool fetch_public_trades(
        std::uint16_t /*symbol_id*/,
        std::int64_t /*start_ns*/,
        std::int64_t /*end_ns*/,
        std::vector<PublicTrade>& /*out*/)
    {
        return false;
    }
};

// Opt-in provider extension. A venue provider class MAY additionally
// implement this (alongside IProvider) to participate in the footprint
// subsystem. Never added to IProvider itself - see file header.
class IFootprintCapableProvider
{
public:
    virtual ~IFootprintCapableProvider() = default;

    virtual FootprintVenueCapabilities
    footprint_capabilities(const std::string& /*symbol*/) const
    {
        return FootprintVenueCapabilities{};
    }

    // nullptr -> no history source; footprint stays BACKFILLING/PARTIAL from
    // live-only data until one is available.
    virtual std::shared_ptr<IPublicTradeHistorySource>
    get_public_trade_history_source()
    {
        return nullptr;
    }
};

// Reasons resolve_footprint_tick_size can decline to produce a tick size.
// footprint.md §2.1: "Conflicting metadata and override will make the
// footprint unavailable rather than guessing."
enum class tick_size_status : std::uint8_t
{
    resolved,             // exactly one positive source, or both agree
    unavailable,          // neither official metadata nor override present
    conflicting_override, // both present and disagree
    invalid_override,     // override present but not a positive exact value
};

struct tick_size_resolution
{
    tick_size_status status = tick_size_status::unavailable;
    double tick_size = 0.0; // valid only when status == resolved
};

// Official venue metadata wins; a positive override is only consulted when
// metadata is absent, and must exactly match metadata when both are
// present. Never silently prefers one over a mismatched other - see
// footprint.md §2.1.
inline tick_size_resolution resolve_footprint_tick_size(
    std::optional<double> official_metadata_tick_size,
    std::optional<double> configured_override_tick_size)
{
    const bool override_present = configured_override_tick_size.has_value();
    const bool override_valid =
        override_present && *configured_override_tick_size > 0.0;

    if (override_present && !override_valid)
        return {tick_size_status::invalid_override, 0.0};

    if (official_metadata_tick_size.has_value() &&
        *official_metadata_tick_size > 0.0)
    {
        if (override_valid &&
            *configured_override_tick_size != *official_metadata_tick_size)
        {
            return {tick_size_status::conflicting_override, 0.0};
        }
        return {tick_size_status::resolved, *official_metadata_tick_size};
    }

    if (override_valid)
        return {tick_size_status::resolved, *configured_override_tick_size};

    return {tick_size_status::unavailable, 0.0};
}

} // namespace truetest::footprint
