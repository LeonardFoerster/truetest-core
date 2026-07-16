#include "instrument_spec_cache.h"

const instrument_spec* InstrumentSpecCache::resolve(const std::string& symbol) {
    // stub, logic moved from engine
    auto it = cache_.find(symbol);
    if (it != cache_.end() && it->second.has_value()) {
        return &*it->second;
    }
    return nullptr;
}

bool InstrumentSpecCache::apply(instrument_spec& spec, order_event& o) const {
    // stub
    return true;
}