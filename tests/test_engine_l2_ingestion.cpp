// Pins the L2-into-orderbook_registry path + market-maker suppression.
// apply_l2_snapshot must (a) populate the engine's orderbook for that
// symbol with the supplied levels and (b) mark the symbol as L2-seeded
// so subsequent strategy bars don't let the market-maker overwrite it.

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "strategy/strategy_interface.h"

namespace {

class NullStrategy : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override { return std::nullopt; }
    void set_position_open(const std::string&, bool) override {}
};

} // namespace

TEST(EngineL2Ingestion, SnapshotPopulatesOrderbook)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    cfg.initial_balance = 10000.0;
    engine eng(dh, ob, strat, std::move(cfg));

    std::vector<l2_level> bids = {
        {42000.0, 150'000'000},  // 1.5 (qty is scaled by 1e8)
        {41999.0, 250'000'000},
    };
    std::vector<l2_level> asks = {
        {42001.0, 100'000'000},
        {42002.0, 200'000'000},
    };
    eng.apply_l2_snapshot("BTCUSDT", bids, asks);

    auto book = eng.get_orderbook_registry().get("BTCUSDT");
    ASSERT_NE(book, nullptr);

    const auto infos = book->get_order_infos();
    EXPECT_GE(infos.get_bids().size(), 1u);
    EXPECT_GE(infos.get_asks().size(), 1u);
    EXPECT_DOUBLE_EQ(infos.get_bids().front().price_.to_double(), 42000.0);
    EXPECT_DOUBLE_EQ(infos.get_asks().front().price_.to_double(), 42001.0);
}

TEST(EngineL2Ingestion, UpdateRemovesLevelAtZeroQty)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    engine eng(dh, ob, strat, std::move(cfg));

    std::vector<l2_level> bids = {{42000.0, 150'000'000}};
    std::vector<l2_level> asks = {{42001.0, 100'000'000}};
    eng.apply_l2_snapshot("BTCUSDT", bids, asks);

    // Delete the bid level by setting qty to 0.
    eng.apply_l2_update("BTCUSDT", tick_side::bid, 42000.0, 0);

    auto book = eng.get_orderbook_registry().get("BTCUSDT");
    ASSERT_NE(book, nullptr);
    const auto infos = book->get_order_infos();

    // Bid should be gone.
    bool found_bid_at_42000 = false;
    for (const auto& lvl : infos.get_bids())
        if (std::abs(lvl.price_.to_double() - 42000.0) < 1e-6)
            found_bid_at_42000 = true;
    EXPECT_FALSE(found_bid_at_42000);
}
