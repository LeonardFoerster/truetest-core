#pragma once

#include "execution/instrument.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


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

    // F-07a — every configured --instrument symbol that does not appear in
    // `present_symbols`. A spec that binds to no symbol is completely inert
    // today: tick size, lot size, min quantity and min notional all do
    // nothing and the run is byte-identical to one without the flag. This
    // turns that silent no-op into something the caller can fail on.
    //
    // Only explicit overrides are checked. Provider-supplied specs are
    // resolved lazily per traded symbol and cannot be "unmatched".
    std::vector<std::string> unmatched_overrides(
        const std::unordered_set<std::string>& present_symbols) const;


    void clear();

private:
    const std::unordered_map<std::string, instrument_spec>& overrides_;
    IProvider* provider_;
    std::unordered_map<std::string, std::optional<instrument_spec>> cache_;
};