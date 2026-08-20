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

}

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
    eng.apply_l2_snapshot("BTCUSDT", bids, asks, {}, 100'000'000ULL);

    auto book = eng.get_orderbook_registry().get("BTCUSDT");
    ASSERT_NE(book, nullptr);

    const auto infos = book->get_order_infos();
    EXPECT_GE(infos.get_bids().size(), 1u);
    EXPECT_GE(infos.get_asks().size(), 1u);
    EXPECT_DOUBLE_EQ(infos.get_bids().front().price_.to_double(), 42000.0);
    EXPECT_DOUBLE_EQ(infos.get_asks().front().price_.to_double(), 42001.0);
    EXPECT_EQ(infos.get_bids().front().quantity_, 150'000'000u)
        << "1.5 base units must map to the configured 1e8 book scale";
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
    eng.apply_l2_snapshot("BTCUSDT", bids, asks, {}, 100'000'000ULL);

    // Delete the bid level by setting qty to 0.
    eng.apply_l2_update("BTCUSDT", tick_side::bid, 42000.0, 0, {},
                        100'000'000ULL);

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

TEST(EngineL2Ingestion, SequencedDeltaBatchCommitsAllMutationsAtomically)
{
    auto dh = std::make_shared<data_handler>();
    auto strat = std::make_shared<NullStrategy>();
    engine eng(dh, std::make_shared<orderbook>(), strat, engine_config{});
    const auto t0 = std::chrono::system_clock::time_point(std::chrono::milliseconds(1));
    eng.apply_l2_snapshot("BTCUSDT", {{100.0, 10}}, {{101.0, 10}}, t0, 1, 100);

    provider::l2_delta_batch batch;
    batch.timestamp = t0 + std::chrono::milliseconds(1);
    batch.symbol = "BTCUSDT";
    batch.first_update_id = 101;
    batch.final_update_id = 101;
    batch.quantity_scale = 1;
    batch.updates = {
        {batch.timestamp, batch.symbol, 0, 100.0, 0, 1},
        {batch.timestamp, batch.symbol, 1, 101.0, 0, 1},
        {batch.timestamp, batch.symbol, 1, 102.0, 20, 1},
    };
    eng.apply_l2_delta_batch(batch);

    EXPECT_FALSE(eng.is_halted());
    const auto book = eng.get_orderbook_registry().get("BTCUSDT");
    ASSERT_NE(book, nullptr);
    EXPECT_DOUBLE_EQ(book->best_external_bid_price(), 0.0);
    EXPECT_DOUBLE_EQ(book->best_external_ask_price(), 102.0);
}

TEST(EngineL2Ingestion, SequencedDeltaGapHaltsBeforeBookMutation)
{
    auto dh = std::make_shared<data_handler>();
    auto strat = std::make_shared<NullStrategy>();
    engine eng(dh, std::make_shared<orderbook>(), strat, engine_config{});
    const auto t0 = std::chrono::system_clock::time_point(std::chrono::milliseconds(1));
    eng.apply_l2_snapshot("BTCUSDT", {{100.0, 10}}, {{101.0, 10}}, t0, 1, 100);

    provider::l2_delta_batch gap;
    gap.timestamp = t0 + std::chrono::milliseconds(1);
    gap.symbol = "BTCUSDT";
    gap.first_update_id = 102;
    gap.final_update_id = 102;
    gap.quantity_scale = 1;
    gap.updates = {{gap.timestamp, gap.symbol, 0, 99.0, 50, 1}};
    eng.apply_l2_delta_batch(gap);

    EXPECT_TRUE(eng.is_halted());
    const auto book = eng.get_orderbook_registry().get("BTCUSDT");
    ASSERT_NE(book, nullptr);
    EXPECT_DOUBLE_EQ(book->best_external_bid_price(), 100.0);
}

TEST(EngineL2Ingestion, InvalidSnapshotHaltsWithoutMutatingExistingBook)
{
    auto dh = std::make_shared<data_handler>();
    auto strat = std::make_shared<NullStrategy>();
    engine eng(dh, std::make_shared<orderbook>(), strat, engine_config{});

    eng.apply_l2_snapshot("BTCUSDT", {{100.0, 100'000'000}},
                          {{101.0, 100'000'000}}, {}, 100'000'000ULL);
    auto book = eng.get_orderbook_registry().get("BTCUSDT");
    ASSERT_NE(book, nullptr);

    eng.apply_l2_snapshot("BTCUSDT", {{99.0, -1}},
                          {{102.0, 100'000'000}}, {}, 100'000'000ULL);

    EXPECT_TRUE(eng.is_halted());
    const auto infos = book->get_order_infos();
    ASSERT_FALSE(infos.get_bids().empty());
    ASSERT_FALSE(infos.get_asks().empty());
    EXPECT_DOUBLE_EQ(infos.get_bids().front().price_.to_double(), 100.0);
    EXPECT_DOUBLE_EQ(infos.get_asks().front().price_.to_double(), 101.0);
}

TEST(EngineL2Ingestion, InvalidUpdateHaltsWithoutDeletingExistingLevel)
{
    auto dh = std::make_shared<data_handler>();
    auto strat = std::make_shared<NullStrategy>();
    engine eng(dh, std::make_shared<orderbook>(), strat, engine_config{});

    eng.apply_l2_snapshot("BTCUSDT", {{100.0, 100'000'000}},
                          {{101.0, 100'000'000}}, {}, 100'000'000ULL);
    auto book = eng.get_orderbook_registry().get("BTCUSDT");
    ASSERT_NE(book, nullptr);

    eng.apply_l2_update("BTCUSDT", tick_side::bid, 100.0,
                        100'000'000, {}, /*quantity_scale=*/0);

    EXPECT_TRUE(eng.is_halted());
    const auto infos = book->get_order_infos();
    ASSERT_FALSE(infos.get_bids().empty());
    EXPECT_DOUBLE_EQ(infos.get_bids().front().price_.to_double(), 100.0);
    EXPECT_EQ(infos.get_bids().front().quantity_, 100'000'000u);
}

TEST(EngineL2Ingestion, InvalidPriceAndSideHaltBeforeBookMutation)
{
    for (const auto invalid_call : {0, 1, 2, 3, 4})
    {
        auto dh = std::make_shared<data_handler>();
        auto strat = std::make_shared<NullStrategy>();
        engine eng(dh, std::make_shared<orderbook>(), strat, engine_config{});
        eng.apply_l2_snapshot("BTCUSDT", {{100.0, 100'000'000}},
                              {{101.0, 100'000'000}}, {}, 100'000'000ULL);
        auto book = eng.get_orderbook_registry().get("BTCUSDT");
        ASSERT_NE(book, nullptr);

        if (invalid_call == 0)
            eng.apply_l2_snapshot("BTCUSDT", {{NAN, 100'000'000}},
                                  {{102.0, 100'000'000}}, {},
                                  100'000'000ULL);
        else if (invalid_call == 1)
            eng.apply_l2_update("BTCUSDT", tick_side::unknown, 100.0,
                                0, {}, 100'000'000ULL);
        else if (invalid_call == 2)
            eng.apply_l2_update("BTCUSDT", tick_side::bid, INFINITY,
                                0, {}, 100'000'000ULL);
        else if (invalid_call == 3)
            eng.apply_l2_snapshot("BTCUSDT", {{1e100, 100'000'000}},
                                  {{102.0, 100'000'000}}, {},
                                  100'000'000ULL);
        else
            eng.apply_l2_update("BTCUSDT", tick_side::bid, 1e100,
                                0, {}, 100'000'000ULL);

        EXPECT_TRUE(eng.is_halted());
        const auto infos = book->get_order_infos();
        ASSERT_FALSE(infos.get_bids().empty());
        EXPECT_DOUBLE_EQ(infos.get_bids().front().price_.to_double(), 100.0);
        EXPECT_EQ(infos.get_bids().front().quantity_, 100'000'000u);
    }
}
