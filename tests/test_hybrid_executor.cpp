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
    // release_ts window — fill still held.
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

    // Advance the clock well past the latency window — the cancelled
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
