#pragma once

#include "execution/instrument.h"
#include <unordered_map>
#include <string>
#include <optional>

// Cold extraction (PR-07). Moved from engine for net reduction.
// Thin cache, no hot path coupling. Delegates from engine.

class IProvider;  // fwd to avoid include in cold cache

// forward for order_event used in apply
struct order_event;

class InstrumentSpecCache {
public:
    // cfg parts passed by ref (overrides + provider); no ownership.
    InstrumentSpecCache(const std::unordered_map<std::string, instrument_spec>& overrides,
                        IProvider* provider);

    const instrument_spec* resolve_instrument_spec(const std::string& symbol);
    bool apply_instrument_spec(order_event& o, const instrument_spec& spec) const;

    void clear();

private:
    const std::unordered_map<std::string, instrument_spec>& overrides_;
    IProvider* provider_;
    std::unordered_map<std::string, std::optional<instrument_spec>> cache_;
};