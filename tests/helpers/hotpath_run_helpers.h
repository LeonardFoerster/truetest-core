#pragma once

// Shared engine fixtures for hot-path allocation baseline tests.

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "execution/latency_model.h"
#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"
#include "strategy/sma_strategy.h"
#include "strategy/strategy_interface.h"

#include <chrono>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace truetest::test::hotpath {

struct silence_cout
{
    std::ostringstream sink;
    std::streambuf* orig;
    silence_cout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~silence_cout() { std::cout.rdbuf(orig); }
};

inline auto epoch_ms(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

inline std::shared_ptr<data_handler> make_bars(int n, const std::string& symbol = "TEST")
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < n; ++i)
        dh->load_into_queue("2024-01-01", symbol,
                            100.0 + i * 0.01, 101.0 + i * 0.01,
                            99.0 + i * 0.01, 100.5 + i * 0.01, 1000);
    return dh;
}

inline std::shared_ptr<data_handler> make_ticks(int n, const std::string& symbol = "TEST")
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < n; ++i)
    {
        tick_record t;
        t.timestamp = epoch_ms(i * 100);
        t.symbol = symbol;
        t.price = 100.0 + (i % 10) * 0.1;
        t.quantity = 10;
        t.side = data_tick_side::unknown;
        dh->tick_data.push_back(t);
    }
    return dh;
}

// Never trades — isolates market_event publish path.
class no_trade_strategy : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }
};

inline engine_config base_cfg(thread_preset preset = thread_preset::inline_mode)
{
    engine_config cfg;
    cfg.initial_balance = 10000.0;
    cfg.seed = 424242;
    cfg.threading = preset;
    cfg.disable_pinning = true;
    return cfg;
}

inline void seed_book(const std::shared_ptr<orderbook>& ob, double mid = 100.0)
{
    MarketMaker mm(424242u + 1u);
    mm.add_orders(ob, mid, 40);
}

struct pool_grow_totals
{
    std::size_t total_grow = 0;

    std::size_t total() const { return total_grow; }
};

inline pool_grow_totals read_pool_grows(const engine& eng)
{
    truetest::ui::dashboard_snapshot snap;
    if (!eng.snapshot_dashboard(snap))
        return {};
    pool_grow_totals out;
    for (const auto& p : snap.debug.pools)
        out.total_grow += p.grow_count;
    return out;
}

} // namespace truetest::test::hotpath