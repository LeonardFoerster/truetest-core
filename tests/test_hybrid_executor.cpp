#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/hybrid_executor.h"
#include "execution/latency_model.h"
#include "orderbook/orderbook.h"

#include <chrono>
#include <memory>

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

    // A later submit at t=300us advances now_proxy_ but still below the
    // release_ts window - fill still held.
    auto o2 = make_market(2, order_side::buy, 0.5, tp{us(300)});
    o2.set_earliest_eligible_ts(tp{us(300)});
    hx->submit_order(o2);
    fills.clear();
    EXPECT_FALSE(hx->poll_fills(fills));

    // Submit at t=600us advances now_proxy_ past release_ts: fill(s)
    // for order 1 release. Order 2's release_ts is 300+500=800, still held.
    auto o3 = make_market(3, order_side::buy, 0.5, tp{us(600)});
    o3.set_earliest_eligible_ts(tp{us(600)});
    hx->submit_order(o3);
    fills.clear();
    ASSERT_TRUE(hx->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 1u);
}

TEST(HybridExecutor, WireLatency_Cancel_ClearsDelayedFill)
{
    // A fill buffered inside the wire-latency window gets discarded when
    // the order is cancelled before the window closes. Without this,
    // cancel would lose the race but the fill would still appear later.
    auto latency = std::make_shared<FixedLatencyModel>(us(1000));
    auto hx = make_hybrid(latency);

    auto o = make_market(42, order_side::buy, 1.0, tp{us(0)});
    hx->submit_order(o);

    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills));  // still in latency window

    EXPECT_TRUE(hx->cancel_order(42));

    // Advance the clock well past the latency window - the cancelled
    // fill must not reappear.
    auto o2 = make_market(43, order_side::sell, 0.1, tp{us(5000)});
    o2.set_earliest_eligible_ts(tp{us(5000)});
    hx->submit_order(o2);
    fills.clear();
    hx->poll_fills(fills);
    for (const auto& f : fills)
        EXPECT_NE(f.get_order_id(), 42u);
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

#endif // HAS_BINANCE

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
    order_event o(tp{us(0)}, "BTCUSDT", order_type::limit, order_side::buy,
                  1.0, 99.0, time_in_force::gtc);
    o.set_order_id(100007);
    hx->submit_order(o);

    std::vector<fill_event> fills;
    EXPECT_FALSE(hx->poll_fills(fills));  // nothing crosses 99 yet

    // Mid updates above the limit: the order must survive the re-seed.
    hx->on_mid_price(100.5);
    fills.clear();
    EXPECT_FALSE(hx->poll_fills(fills));

    // Mid drops below the limit: new synthetic asks (≈ 98.0x) cross the
    // resting buy at 99 → maker fill at its own limit price.
    hx->on_mid_price(98.0);
    fills.clear();
    ASSERT_TRUE(hx->poll_fills(fills))
        << "resting limit destroyed by re-seed — must survive and fill";
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 100007u);
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
