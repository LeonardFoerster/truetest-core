#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef HAS_BINANCE

#include "providers/binance/hybrid_executor.h"
#include "execution/latency_model.h"
#include "orderbook/orderbook.h"

using tp = std::chrono::system_clock::time_point;
using us = std::chrono::microseconds;

namespace {

order_event make_market(uint64_t id, order_side side, double qty, tp ts,
                        const std::string& sym = "BTCUSDT")
{
    order_event o(ts, sym, order_type::market, side, qty, 0.0,
                  time_in_force::ioc);
    o.set_order_id(id);
    return o;
}

std::shared_ptr<HybridExecutor> make_hybrid(
    std::shared_ptr<ILatencyModel> latency = nullptr)
{
    auto paper = std::make_shared<BinanceExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<HybridExecutor>(
        paper, book, /*fee_model=*/nullptr, /*fill_model=*/nullptr,
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001,
        std::move(latency));
    hx->on_mid_price(100.0);  // seed the paper book
    return hx;
}

}

TEST(HybridExecutor, NoLatency_FillsVisibleImmediately)
{
    // Regression: with no latency model, fills come back on the same
    // poll that submitted the order.
    auto hx = make_hybrid();
    auto o = make_market(1, order_side::buy, 1.0, tp{us(0)});
    hx->submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(hx->poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
}

TEST(HybridExecutor, WireLatency_FillHeldUntilWindowElapses)
{
    auto latency = std::make_shared<FixedLatencyModel>(us(500));
    auto hx = make_hybrid(latency);

    // Submit order at t=0, eligible_ts defaults to 0. Fill's release_ts
    // becomes fill_ts + 500us.
    auto o = make_market(1, order_side::buy, 1.0, tp{us(0)});
    hx->submit_order(o);

    std::vector<fill_event> fills;
    // Poll immediately after submit: now_proxy_ = 0, release_ts >= 500us,
    // fill is held.
    EXPECT_FALSE(hx->poll_fills(fills));
    EXPECT_TRUE(fills.empty());

    // Time progression must release fills without requiring another order.
    hx->advance_time(tp{us(300)});
    fills.clear();
    EXPECT_FALSE(hx->poll_fills(fills));

    hx->advance_time(tp{us(600)});
    fills.clear();
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 1u);
    EXPECT_EQ(hx->pending_latency_order_count(), 0u)
        << "final fill release must retire submit-time latency state";
}

TEST(HybridExecutor, WireLatency_CancelCannotEraseMatchedFill)
{
    // Matching is final even while its report is behind simulated wire
    // latency. A later cancel must neither claim success nor erase the fill.
    auto latency = std::make_shared<FixedLatencyModel>(us(1000));
    auto hx = make_hybrid(latency);

    auto o = make_market(42, order_side::buy, 1.0, tp{us(0)});
    hx->submit_order(o);

    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills));  // still in latency window

    EXPECT_FALSE(hx->cancel_order(42));
    EXPECT_EQ(hx->pending_latency_order_count(), 0u)
        << "cancel-after-match must not retain stale latency metadata";

    hx->advance_time(tp{us(5000)});
    fills.clear();
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 42u);
}

TEST(HybridExecutor, ZeroLatencyModel_ObservablyEquivalentToNoModel)
{
    // A ZeroLatencyModel shouldn't introduce any visibility delay.
    auto hx = make_hybrid(std::make_shared<ZeroLatencyModel>());
    auto o = make_market(1, order_side::buy, 1.0, tp{us(0)});
    hx->submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(hx->poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
}

TEST(HybridExecutor, QueueModelDemuxesMarketableAndPassiveLimits)
{
    auto paper = std::make_shared<BinanceExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto fees = std::make_shared<TieredFeeModel>(0.001, 0.002);
    auto hx = std::make_shared<HybridExecutor>(
        paper, book, fees,
        std::make_shared<RealisticFillModel>(
            /*fade_rate=*/1.0, /*base_fill_prob=*/0.0,
            /*distance_decay=*/0.0),
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001,
        /*latency_model=*/nullptr,
        std::make_shared<BackCancelModel>());
    hx->on_mid_price(100.0);

    order_event crossing(tp{us(0)}, "BTCUSDT", order_type::limit,
                         order_side::buy, 1.0, 101.0);
    crossing.set_order_id(1001);
    crossing.set_earliest_eligible_ts(tp{us(0)});
    hx->submit_order(crossing);

    std::vector<fill_event> fills;
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 1001u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-12)
        << "configured stochastic/fade model must not downsize a taker";
    EXPECT_NEAR(fills[0].get_commission(),
                fills[0].get_filled_quantity()
                    * fills[0].get_fill_price() * 0.002,
                1e-12)
        << "crossing limit must execute as taker";

    hx->on_l2_snapshot("BTCUSDT", {{99.0, 0.0}}, {});
    order_event passive(tp{us(1)}, "BTCUSDT", order_type::limit,
                        order_side::buy, 1.0, 99.0);
    passive.set_order_id(1002);
    passive.set_earliest_eligible_ts(tp{us(1)});
    hx->submit_order(passive);
    fills.clear();
    EXPECT_FALSE(hx->poll_fills(fills))
        << "passive limit must remain QueueAware";

    hx->on_trade("BTCUSDT", 99.0, 2.0, tp{us(2)});
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 1002u);
    EXPECT_NEAR(fills[0].get_commission(), 99.0 * 0.001, 1e-12)
        << "passive queue fill must retain maker fees";
}

TEST(HybridExecutor, PartialFillRetainsLatencyUntilResidualCancel)
{
    auto latency = std::make_shared<FixedLatencyModel>(us(500));
    auto paper = std::make_shared<BinanceExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<HybridExecutor>(
        paper, book, /*fee_model=*/nullptr,
        std::make_shared<PerfectFillModel>(),
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001, latency);
    hx->on_mid_price(100.0);

    // The best synthetic ask contains one unit. A 1.5-unit limit priced only
    // through that level fills one and leaves 0.5 resting.
    order_event partial(tp{us(0)}, "BTCUSDT", order_type::limit,
                        order_side::buy, 1.5, 100.01);
    partial.set_order_id(2001);
    partial.set_earliest_eligible_ts(tp{us(0)});
    hx->submit_order(partial);

    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills));
    EXPECT_EQ(hx->pending_latency_order_count(), 1u);
    hx->advance_time(tp{us(600)});
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 0.5, 1e-9);
    EXPECT_EQ(hx->pending_latency_order_count(), 1u)
        << "partial release must retain latency for the live residual";

    EXPECT_TRUE(hx->cancel_order(2001));
    EXPECT_EQ(hx->pending_latency_order_count(), 1u)
        << "cancel latency keeps the residual live until advance_time";

    // The residual is adversely filled before the cancel reaches the book.
    // It must retain the original 500us wire delay rather than appearing at
    // once after cancel erased its latency metadata.
    hx->advance_time(tp{us(800)});
    hx->on_mid_price(99.0);
    fills.clear();
    EXPECT_FALSE(hx->poll_fills(fills));

    hx->advance_time(tp{us(1200)});
    EXPECT_EQ(hx->pending_latency_order_count(), 0u)
        << "effective cancel/fill must retire latency state";
    EXPECT_FALSE(hx->poll_fills(fills))
        << "fill must remain behind its original wire latency";

    hx->advance_time(tp{us(1400)});
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 2001u);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 0.0, 1e-9);
}

// Remaining HybridExecutor tests still require Binance paper executor.

TEST(HybridExecutor, RestingLimitSurvivesMidUpdateAndFills)
{
    // Regression: on_mid_price used book_->clear(), which destroyed any
    // strategy limit resting in the shared book — the order stayed "open"
    // engine-side but could never fill. Re-seeding must cancel only the
    // synthetic quotes, and a re-seed that crosses the resting level must
    // surface a maker fill at the order's own limit price.
    auto paper = std::make_shared<BinanceExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<HybridExecutor>(
        paper, book, /*fee_model=*/nullptr,
        std::make_shared<PerfectFillModel>(),
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001);
    hx->on_mid_price(100.0);

    // Passive buy limit well below the market: rests in the shared book.
    // Use a high order id so we do not collide with HybridExecutor's
    // synthetic quote ids (OrderIdGenerator sequential from the re-seeds).
    constexpr std::uint64_t strategy_order_id = 100007;
    order_event o(tp{us(0)}, "BTCUSDT", order_type::limit, order_side::buy,
                  1.0, 99.0, time_in_force::gtc);
    o.set_order_id(strategy_order_id);
    hx->submit_order(o);

    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills));  // nothing crosses 99 yet

    // Mid updates above the limit: the order must survive the re-seed.
    hx->on_mid_price(100.5);
    fills.clear();
    EXPECT_FALSE(hx->poll_fills(fills));
    ASSERT_NE(book->get_order(strategy_order_id), nullptr)
        << "mid re-seed must not clear the shared strategy order";

    // Mid drops below the limit: new synthetic asks (≈ 98.0x) cross the
    // resting buy at 99 → maker fill at its own limit price.
    hx->on_mid_price(98.0);
    fills.clear();
    ASSERT_TRUE(hx->poll_fills(fills))
        << "resting limit destroyed by re-seed — must survive and fill";
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), strategy_order_id);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 99.0);
}

TEST(HybridExecutor, EngineFacingOnBookTradesViaIExecutionAdapter)
{
    // Residual-risk regression: engine used to dynamic_cast to
    // LocalBookAdapter*, so HybridExecutor silently dropped MM book
    // trades. Delivery must work through IExecutionAdapter*.
    auto paper = std::make_shared<BinanceExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<HybridExecutor>(
        paper, book, /*fee_model=*/nullptr,
        std::make_shared<PerfectFillModel>(),
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001);
    hx->on_mid_price(100.0);

    order_event o(tp{us(0)}, "BTCUSDT", order_type::limit, order_side::buy,
                  1.0, 99.0, time_in_force::gtc);
    o.set_order_id(100101);
    hx->submit_order(o);

    // Simulate an external MM trade that fills our resting buy as maker
    // at its own limit (same shape LocalBookAdapter::record_resting_fill
    // expects from orderbook trades).
    trade_info bid_ti{/*orderId=*/100101u, Price::from_double(99.0),
                      static_cast<quantity>(1e8)};
    trade_info ask_ti{/*counterparty quote id*/ 42u, Price::from_double(99.0),
                      static_cast<quantity>(1e8)};
    trades trs;
    trs.emplace_back(bid_ti, ask_ti);

    IExecutionAdapter* iface = hx.get();
    iface->on_book_trades(trs, tp{us(1)});

    std::vector<fill_event> fills;
    ASSERT_TRUE(iface->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 100101u);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 99.0);
}

TEST(HybridExecutor, EngineFacingSweepRestingRangeViaIExecutionAdapter)
{
    // Same residual: bar-range sweep must reach Hybrid's inner LocalBook.
    auto paper = std::make_shared<BinanceExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<HybridExecutor>(
        paper, book, /*fee_model=*/nullptr,
        std::make_shared<PerfectFillModel>(),
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001);
    hx->on_mid_price(100.0);

    order_event o(tp{us(0)}, "BTCUSDT", order_type::limit, order_side::buy,
                  1.0, 99.0, time_in_force::gtc);
    o.set_order_id(100202);
    hx->submit_order(o);

    IExecutionAdapter* iface = hx.get();
    ASSERT_TRUE(iface->sweep_resting_range("BTCUSDT", 98.0, 101.0, tp{us(2)}));

    std::vector<fill_event> fills;
    ASSERT_TRUE(iface->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 100202u);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 99.0);
}

#endif // HAS_BINANCE

#ifdef HAS_BITGET

#include "providers/bitget/bitget_hybrid_executor.h"

TEST(BitgetHybridExecutor, RestingLimitSurvivesMidUpdateAndFills)
{
    using bitget_tp = std::chrono::system_clock::time_point;

    auto paper = std::make_shared<BitgetPaperExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<BitgetHybridExecutor>(
        paper, book, /*fee_model=*/nullptr,
        std::make_shared<PerfectFillModel>(),
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001);
    hx->on_mid_price(100.0);

    order_event limit(bitget_tp{}, "BTCUSDT", order_type::limit,
                      order_side::buy, 1.0, 99.0, time_in_force::gtc);
    const auto strategy_order_id = OrderIdGenerator::next();
    limit.set_order_id(strategy_order_id);
    limit.set_earliest_eligible_ts(bitget_tp{});
    hx->submit_order(limit);

    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills));

    // A non-crossing re-seed must preserve the locally resting strategy order.
    hx->on_mid_price(100.5);
    fills.clear();
    EXPECT_FALSE(hx->poll_fills(fills));

    // The next re-seed crosses 99.0. The surviving order must produce its
    // maker fill at the resting limit, rather than disappearing on clear().
    hx->on_mid_price(98.0);
    fills.clear();
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), strategy_order_id);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 99.0);
}

TEST(BitgetHybridExecutor, AdvanceTimeReleasesWireDelayedFill)
{
    using bitget_tp = std::chrono::system_clock::time_point;
    using bitget_us = std::chrono::microseconds;

    auto paper = std::make_shared<BitgetPaperExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<BitgetHybridExecutor>(
        paper, book, /*fee_model=*/nullptr, /*fill_model=*/nullptr,
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001,
        std::make_shared<FixedLatencyModel>(bitget_us(500)));
    hx->on_mid_price(100.0);

    order_event market(bitget_tp{bitget_us(0)}, "BTCUSDT",
                       order_type::market, order_side::buy, 1.0);
    market.set_order_id(3001);
    market.set_earliest_eligible_ts(bitget_tp{bitget_us(0)});
    hx->submit_order(market);

    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills));
    EXPECT_EQ(hx->pending_latency_order_count(), 1u);
    hx->advance_time(bitget_tp{bitget_us(600)});
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 3001u);
    EXPECT_EQ(hx->pending_latency_order_count(), 0u);
}

TEST(BitgetHybridExecutor, WireLatencyCancelCannotEraseMatchedFill)
{
    using bitget_tp = std::chrono::system_clock::time_point;
    using bitget_us = std::chrono::microseconds;

    auto paper = std::make_shared<BitgetPaperExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<BitgetHybridExecutor>(
        paper, book, /*fee_model=*/nullptr, /*fill_model=*/nullptr,
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001,
        std::make_shared<FixedLatencyModel>(bitget_us(1000)));
    hx->on_mid_price(100.0);

    order_event market(bitget_tp{bitget_us(0)}, "BTCUSDT",
                       order_type::market, order_side::buy, 1.0);
    market.set_order_id(3002);
    market.set_earliest_eligible_ts(bitget_tp{bitget_us(0)});
    hx->submit_order(market);

    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills));
    EXPECT_FALSE(hx->cancel_order(3002));
    EXPECT_EQ(hx->pending_latency_order_count(), 0u);
    hx->advance_time(bitget_tp{bitget_us(2000)});
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 3002u);
}

TEST(BitgetHybridExecutor, QueueModelDemuxesMarketableAndPassiveLimits)
{
    using bitget_tp = std::chrono::system_clock::time_point;
    using bitget_us = std::chrono::microseconds;

    auto paper = std::make_shared<BitgetPaperExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto fees = std::make_shared<TieredFeeModel>(0.001, 0.002);
    auto hx = std::make_shared<BitgetHybridExecutor>(
        paper, book, fees,
        std::make_shared<RealisticFillModel>(
            /*fade_rate=*/1.0, /*base_fill_prob=*/0.0,
            /*distance_decay=*/0.0),
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001,
        /*latency_model=*/nullptr,
        std::make_shared<BackCancelModel>());
    hx->on_mid_price(100.0);

    order_event crossing(bitget_tp{bitget_us(0)}, "BTCUSDT",
                         order_type::limit, order_side::buy, 1.0, 101.0);
    crossing.set_order_id(3003);
    crossing.set_earliest_eligible_ts(bitget_tp{bitget_us(0)});
    hx->submit_order(crossing);

    std::vector<fill_event> fills;
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 3003u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-12);
    EXPECT_NEAR(fills[0].get_commission(),
                fills[0].get_filled_quantity()
                    * fills[0].get_fill_price() * 0.002,
                1e-12);

    hx->on_l2_snapshot("BTCUSDT", {{99.0, 0.0}}, {});
    order_event passive(bitget_tp{bitget_us(1)}, "BTCUSDT",
                        order_type::limit, order_side::buy, 1.0, 99.0);
    passive.set_order_id(3004);
    passive.set_earliest_eligible_ts(bitget_tp{bitget_us(1)});
    hx->submit_order(passive);
    fills.clear();
    EXPECT_FALSE(hx->poll_fills(fills));
    hx->on_trade("BTCUSDT", 99.0, 2.0, bitget_tp{bitget_us(2)});
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 3004u);
    EXPECT_NEAR(fills[0].get_commission(), 99.0 * 0.001, 1e-12);
}

TEST(BitgetHybridExecutor, CancelLatencyStateRetiresWhenCancelBecomesEffective)
{
    using bitget_tp = std::chrono::system_clock::time_point;
    using bitget_us = std::chrono::microseconds;

    auto latency = std::make_shared<FixedLatencyModel>(bitget_us(500));
    auto paper = std::make_shared<BitgetPaperExecutor>();
    paper->set_symbol("BTCUSDT");
    paper->set_last_price(100.0);
    auto book = std::make_shared<orderbook>();
    auto hx = std::make_shared<BitgetHybridExecutor>(
        paper, book, /*fee_model=*/nullptr,
        std::make_shared<PerfectFillModel>(),
        /*qty_scale=*/1e8, /*spread_step_factor=*/0.0001,
        latency, std::make_shared<BackCancelModel>());
    hx->on_mid_price(100.0);
    hx->on_l2_snapshot("BTCUSDT", {{99.0, 0.0}}, {});

    order_event passive(bitget_tp{}, "BTCUSDT", order_type::limit,
                        order_side::buy, 1.0, 99.0);
    passive.set_order_id(3005);
    passive.set_earliest_eligible_ts(bitget_tp{});
    hx->submit_order(passive);
    EXPECT_EQ(hx->pending_latency_order_count(), 1u);
    EXPECT_TRUE(hx->cancel_order(3005));
    EXPECT_EQ(hx->pending_latency_order_count(), 1u);

    hx->advance_time(bitget_tp{bitget_us(400)});
    EXPECT_EQ(hx->pending_latency_order_count(), 1u);
    hx->advance_time(bitget_tp{bitget_us(600)});
    EXPECT_EQ(hx->pending_latency_order_count(), 0u);

    hx->on_trade("BTCUSDT", 99.0, 2.0,
                 bitget_tp{bitget_us(700)});
    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills))
        << "effective cancel must remove the passive queue order";
}

#endif // HAS_BITGET
