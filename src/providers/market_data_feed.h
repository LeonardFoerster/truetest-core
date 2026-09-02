#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/transport.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Public-market-data configuration is deliberately semantic. Each venue
// adapter translates it to its own URL/topic/subscription protocol; no venue
// stream spelling is allowed to escape this boundary.
enum class market_data_channel_kind : std::uint8_t
{
    trades,
    candles,
    l2_snapshot,
    l2_delta,
};

struct market_data_channel_request
{
    market_data_channel_kind kind = market_data_channel_kind::trades;
    std::uint32_t depth = 0; // L2 only; 0 means the venue default.
    // Candles only; zero means the provider's already-validated configured
    // interval. It never means that a timestamp/interval may be inferred.
    std::chrono::milliseconds candle_interval{};
};

struct market_data_request
{
    std::string symbol;
    std::vector<market_data_channel_request> channels;
};

// Describes a venue adapter's published event semantics. In particular,
// callers must not mistake a sequenced L2 delta feed for self-contained
// snapshots that can safely replace a local book.
struct market_data_capabilities
{
    bool trades = false;
    bool candles = false;
    bool l2_snapshots = false;
    bool l2_deltas = false;
    std::uint32_t max_l2_depth = 0;
    bool event_order_is_receive_order = true;
};

// A public market-data transport and its matching parser form one ownership
// and lifetime unit. This is a cold-path construction object: per-event work
// remains in the existing DataBridge<provider::event> path.
struct MarketDataFeed
{
    std::shared_ptr<IDataTransport> transport;
    std::shared_ptr<IDataParser<provider::event>> parser;

    // A legacy adapter can safely bundle its existing transport/parser before
    // it has translated opaque venue stream names into this semantic model.
    // Missing metadata means "not declared", never "unsupported".
    std::optional<market_data_request> request;
    std::optional<market_data_capabilities> capabilities;

    bool ready() const noexcept
    {
        return transport != nullptr && parser != nullptr;
    }
};

enum class market_data_route : std::uint8_t
{
    unified_event_stream,
    legacy_non_streaming,
    invalid,
};

enum class market_data_contract_status : std::uint8_t
{
    valid,
    incomplete_feed,
    missing_declaration,
    empty_symbol,
    empty_channels,
    duplicate_channel,
    unsupported_channel,
    invalid_channel_options,
    invalid_depth_capability,
};

// Cold-path validation for the transport/parser contract. A non-null parser
// is not itself a capability declaration: request and emitted-event metadata
// must agree before any external frame is allowed to reach DataBridge.
[[nodiscard]] inline market_data_contract_status validate_market_data_feed(
    const MarketDataFeed& feed) noexcept
{
    if (!feed.ready())
        return market_data_contract_status::incomplete_feed;
    if (!feed.request || !feed.capabilities)
        return market_data_contract_status::missing_declaration;
    if (feed.request->symbol.empty())
        return market_data_contract_status::empty_symbol;
    if (feed.request->channels.empty())
        return market_data_contract_status::empty_channels;

    const auto& capabilities = *feed.capabilities;
    if (capabilities.max_l2_depth > 0
        && !capabilities.l2_snapshots && !capabilities.l2_deltas)
        return market_data_contract_status::invalid_depth_capability;
    if (capabilities.l2_snapshots && capabilities.max_l2_depth == 0)
        return market_data_contract_status::invalid_depth_capability;

    std::uint8_t seen_channels = 0;
    for (const auto& channel : feed.request->channels)
    {
        const auto channel_ordinal = static_cast<std::uint8_t>(channel.kind);
        if (channel_ordinal
            > static_cast<std::uint8_t>(market_data_channel_kind::l2_delta))
            return market_data_contract_status::invalid_channel_options;
        const auto bit = static_cast<std::uint8_t>(1U << channel_ordinal);
        if ((seen_channels & bit) != 0)
            return market_data_contract_status::duplicate_channel;
        seen_channels = static_cast<std::uint8_t>(seen_channels | bit);

        if (channel.kind != market_data_channel_kind::candles
            && channel.candle_interval.count() != 0)
            return market_data_contract_status::invalid_channel_options;
        if (channel.kind != market_data_channel_kind::l2_snapshot
            && channel.kind != market_data_channel_kind::l2_delta
            && channel.depth != 0)
            return market_data_contract_status::invalid_channel_options;

        bool supported = false;
        switch (channel.kind)
        {
        case market_data_channel_kind::trades:
            supported = capabilities.trades;
            break;
        case market_data_channel_kind::candles:
            supported = capabilities.candles;
            break;
        case market_data_channel_kind::l2_snapshot:
            supported = capabilities.l2_snapshots;
            break;
        case market_data_channel_kind::l2_delta:
            supported = capabilities.l2_deltas;
            break;
        }
        if (!supported)
            return market_data_contract_status::unsupported_channel;
        if (channel.depth > 0
            && (capabilities.max_l2_depth == 0
                || channel.depth > capabilities.max_l2_depth))
            return market_data_contract_status::invalid_depth_capability;
    }
    return market_data_contract_status::valid;
}

// Parser selection is capability-owned. A streaming transport without its
// matching parser/feed declaration is never routed by provider-name guesses.
[[nodiscard]] inline market_data_route select_market_data_route(
    bool provider_advertises_event_stream,
    const std::optional<MarketDataFeed>& feed,
    bool transport_is_streaming) noexcept
{
    if (feed
        && validate_market_data_feed(*feed)
            == market_data_contract_status::valid)
        return market_data_route::unified_event_stream;
    if (provider_advertises_event_stream || feed || transport_is_streaming)
        return market_data_route::invalid;
    return market_data_route::legacy_non_streaming;
}
