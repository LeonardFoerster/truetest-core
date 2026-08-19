#pragma once

// EngineHotPathSink — narrow interface onto engine's own centralized
// hot-path primitives (single event-log writer, single ring-dispatch
// policy, single halt entry point), per the "OrderIntentProcessor
// Preparation Report" §9 ("Callback / interface recommendation").
//
// This is the agreed narrow mechanism for a domain processor to reach
// log_event/publish_event/trigger_halt without holding a concrete engine&
// (which would be a back-reference / service-locator anti-pattern — see
// docs/architecture/05-engine-boundaries.md Check B). `engine` implements
// this interface directly (adds `public EngineHotPathSink` + `override` to
// its three existing methods); no new behavior, no new primitive.
//
// A plain vtable reference, not std::function: no per-callback heap capture,
// no type erasure beyond ordinary virtual dispatch, and — unlike binding
// three separate std::function objects per domain processor — this is meant
// to be the one reusable contract every future domain processor
// (OrderIntentProcessor now, MarketEventProcessor later) references, rather
// than each re-deriving its own three lambdas.
//
// LIVE-SAFETY SURFACE: the sole channel through which an extracted domain
// processor reaches the terminal halt entry point. See
// scripts/check-live-safety-freeze.sh.

#include "core/event.h"

#include <string_view>

class EngineHotPathSink
{
public:
    virtual ~EngineHotPathSink() = default;

    virtual void log_event(const event& ev) = 0;
    virtual void publish_event(const event_pointer& ev) = 0;

    // Single, idempotent, write-once terminal halt entry point (S3). Domain
    // processors call this instead of ever setting a halt flag themselves.
    virtual void trigger_halt(std::string_view reason) noexcept = 0;
};
