#pragma once

#include <cmath>
#include <string>

// Venue-specific trading rules for a single instrument.
// Fields left at 0 are treated as "unset" - the corresponding filter is skipped.
struct instrument_spec
{
    std::string symbol;
    double tick_size = 0.0;     // price increment (e.g. 0.01)
    double lot_size = 0.0;      // quantity increment (e.g. 0.00001)
    double min_qty = 0.0;       // minimum order qty
    double min_notional = 0.0;  // minimum order notional (qty * price)
    double maker_rate = 0.0;    // maker fee rate (e.g. 0.001 for 10 bps)
    double taker_rate = 0.0;    // taker fee rate
};

inline double quantize_price_to_tick(double price, double tick)
{
    if (tick <= 0.0) return price;
    return std::round(price / tick) * tick;
}

inline double floor_qty_to_lot(double qty, double lot)
{
    if (lot <= 0.0) return qty;
    return std::floor(qty / lot) * lot;
}

inline bool meets_min_qty(double qty, double min_qty)
{
    return min_qty <= 0.0 || qty >= min_qty - 1e-12;
}

inline bool meets_min_notional(double qty, double price, double min_notional)
{
    return min_notional <= 0.0 || (qty * price) >= min_notional - 1e-9;
}
