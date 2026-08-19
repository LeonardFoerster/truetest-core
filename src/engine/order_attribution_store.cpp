#include "order_attribution_store.h"

// LIVE-SAFETY SURFACE: see order_attribution_store.h + scripts/check-live-safety-freeze.sh.

void OrderAttributionStore::register_order(const order_event& o)
{
    const std::uint64_t opener = (o.get_opener_order_id() != 0)
        ? o.get_opener_order_id()
        : o.get_order_id();
    map_[o.get_order_id()] = order_meta{opener, o.get_strategy_name()};
}

void OrderAttributionStore::note_bracket_leg(uint64_t engine_order_id,
                                             uint64_t opener_order_id,
                                             std::string strategy_name)
{
    map_[engine_order_id] = order_meta{opener_order_id, std::move(strategy_name)};
}

uint64_t OrderAttributionStore::opener_for(uint64_t order_id) const
{
    auto it = map_.find(order_id);
    return it != map_.end() ? it->second.opener_order_id : 0;
}

const std::string& OrderAttributionStore::strategy_for(uint64_t order_id) const
{
    static const std::string empty;
    auto it = map_.find(order_id);
    return it != map_.end() ? it->second.strategy_name : empty;
}

void OrderAttributionStore::clear()
{
    map_.clear();
}
