#include "instrument_spec_cache.h"

#include "core/event.h"
#include "providers/provider.h"

InstrumentSpecCache::InstrumentSpecCache(
    const std::unordered_map<std::string, instrument_spec>& overrides,
    IProvider* provider)
    : overrides_(overrides)
    , provider_(provider)
{
}

const instrument_spec* InstrumentSpecCache::resolve_instrument_spec(const std::string& symbol)
{
    auto it = cache_.find(symbol);
    if (it != cache_.end())
        return it->second ? &*it->second : nullptr;

    std::optional<instrument_spec> spec;
    auto ov = overrides_.find(symbol);
    if (ov != overrides_.end())
        spec = ov->second;
    else if (provider_)
        spec = provider_->get_instrument(symbol);

    auto [cit, _] = cache_.emplace(symbol, std::move(spec));
    return cit->second ? &*cit->second : nullptr;
}

bool InstrumentSpecCache::apply_instrument_spec(order_event& o, const instrument_spec& spec) const
{
    if (spec.tick_size > 0.0)
    {
        if (o.get_order_type() == order_type::limit ||
            o.get_order_type() == order_type::stop_limit)
        {
            o.set_price(quantize_price_to_tick(o.get_price(), spec.tick_size));
        }
        if (o.get_order_type() == order_type::stop ||
            o.get_order_type() == order_type::stop_limit)
        {
            o.set_stop_price(quantize_price_to_tick(o.get_stop_price(), spec.tick_size));
        }
    }

    if (spec.lot_size > 0.0)
        o.set_quantity(floor_qty_to_lot(o.get_quantity(), spec.lot_size));

    if (!meets_min_qty(o.get_quantity(), spec.min_qty))
        return false;

    const double ref_price = (o.get_order_type() == order_type::stop)
        ? o.get_stop_price()
        : o.get_price();
    if (!meets_min_notional(o.get_quantity(), ref_price, spec.min_notional))
        return false;

    return true;
}

void InstrumentSpecCache::clear()
{
    cache_.clear();
}
