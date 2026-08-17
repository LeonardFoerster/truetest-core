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
    std::chrono::milliseconds candle_interval{}; // candles only
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
