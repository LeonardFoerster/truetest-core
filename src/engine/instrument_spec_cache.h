#pragma once

#include "execution/instrument.h"
#include <unordered_map>
#include <string>

// Cold extraction (PR-07). Moved from engine for net reduction.
// Thin cache, no hot path coupling.

class InstrumentSpecCache {
public:
    const instrument_spec* resolve(const std::string& symbol);
    bool apply(instrument_spec& spec, order_event& o) const;  // or the apply function

private:
    std::unordered_map<std::string, std::optional<instrument_spec>> cache_;
};