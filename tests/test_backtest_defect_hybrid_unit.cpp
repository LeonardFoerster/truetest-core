// Hybrid/queue/local adapter unit locks (backtest defect closure).
#include "helpers/backtest_defect_helpers.h"

#include <utility>

namespace
{
std::shared_ptr<LocalBookAdapter> make_hybrid_taker(
    const std::shared_ptr<orderbook>& ob,
    std::shared_ptr<IFeeModel> fees = nullptr)
{
    return std::make_shared<LocalBookAdapter>(
        ob, std::move(fees), std::make_shared<PerfectFillModel>(),
        42, 1.1, /*qty_scale=*/1.0);
}
}

TEST(BacktestDefects, FR01_HybridPaper_MarketOrderFills)
{
    auto ob = std::make_shared<orderbook>();
    // Seed a sell so market buy can match.
    ob->add_external_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9001,
        side::sell, Price::from_double(100.0), /*qty=*/10));

    auto local = std::make_shared<LocalBookAdapter>(
        ob, nullptr, nullptr, 42, 1.1, /*qty_scale=*/1.0);
    local->set_mid_price(100.0);
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<UniformCancelModel>());
    HybridPaperAdapter hybrid(local, qa, ob, make_hybrid_taker(ob));

    order_event mkt(t0(), "X", order_type::market, order_side::buy, 5.0, 100.0);
    mkt.set_order_id(1);
    mkt.set_earliest_eligible_ts(t0());
    hybrid.submit_order(mkt);

    std::vector<fill_event> fills;
    ASSERT_TRUE(hybrid.poll_fills(fills));
    ASSERT_FALSE(fills.empty());
    double qty = 0;
    for (const auto& f : fills) qty += f.get_filled_quantity();
    EXPECT_NEAR(qty, 5.0, 1e-9);
}

TEST(BacktestDefects, FR01_HybridPaper_LimitGoesToQueueAware)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_external_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9001,
        side::buy, Price::from_double(98.0), 10));
    ob->add_external_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9002,
        side::sell, Price::from_double(100.0), 10));
    auto local = std::make_shared<LocalBookAdapter>(ob, nullptr, nullptr, 42, 1.1, 1.0);
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<BackCancelModel>());
    HybridPaperAdapter hybrid(local, qa, ob, make_hybrid_taker(ob));

    order_event lim(t0(), "X", order_type::limit, order_side::buy, 5.0, 99.0);
    lim.set_order_id(7);
    lim.set_earliest_eligible_ts(t0());
    hybrid.submit_order(lim);

    EXPECT_EQ(qa->live_order_count(), 1u);
    EXPECT_EQ(local->live_quote_count(), 0u);
}

TEST(BacktestDefects, HybridPaper_MarketableConvertedLimitUsesLocalTaker)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_external_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9001,
        side::buy, Price::from_double(99.0), 10));
    ob->add_external_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9002,
        side::sell, Price::from_double(100.0), 10));

    auto fees = std::make_shared<TieredFeeModel>(
        /*maker_rate=*/0.001, /*taker_rate=*/0.002);
    auto local = std::make_shared<LocalBookAdapter>(
        ob, fees,
        std::make_shared<RealisticFillModel>(
            /*fade_rate=*/0.0, /*base_fill_prob=*/0.0,
            /*distance_decay=*/0.0),
        42, 1.1, /*qty_scale=*/1.0);
    auto aggressive = std::make_shared<LocalBookAdapter>(
        ob, fees, std::make_shared<PerfectFillModel>(),
        42, 1.1, /*qty_scale=*/1.0);
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<BackCancelModel>(), fees);
    HybridPaperAdapter hybrid(local, qa, ob, aggressive);

    // Stop-limit triggers are converted by the engine to ordinary limits
    // before reaching the adapter. Crossing the shared-book ask must select
    // LocalBook immediately, not wait for QueueAware tape activity.
    order_event converted(t0(), "X", order_type::limit,
                          order_side::buy, 2.0, 100.0);
    converted.set_order_id(7);
    converted.set_earliest_eligible_ts(t0());
    hybrid.submit_order(converted);

    EXPECT_EQ(qa->live_order_count(), 0u)
        << "marketable limit must not enter the passive queue";
    std::vector<fill_event> fills;
    ASSERT_TRUE(hybrid.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 7u);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 100.0);
    EXPECT_NEAR(fills[0].get_commission(), 0.4, 1e-12)
        << "marketable limit must pay taker, not maker, fees";
}

TEST(BacktestDefects, HybridPaper_MarketablePartialMigratesResidualToQueue)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_external_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9101,
        side::buy, Price::from_double(99.0), 10));
    ob->add_external_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9102,
        side::sell, Price::from_double(100.0), 1));

    auto local = std::make_shared<LocalBookAdapter>(
        ob, nullptr, nullptr, 42, 1.1, /*qty_scale=*/1.0);
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<BackCancelModel>());
    HybridPaperAdapter hybrid(
        local, qa, ob, make_hybrid_taker(ob));
    hybrid.on_l2_snapshot("X", {{100.0, 0.0}}, {});

    order_event crossing(t0(), "X", order_type::limit,
                         order_side::buy, 2.0, 100.0);
    crossing.set_order_id(8);
    crossing.set_earliest_eligible_ts(t0());
    hybrid.submit_order(crossing);

    EXPECT_EQ(qa->live_order_count(), 1u)
        << "unfilled taker remainder must become a passive queue order";
    EXPECT_EQ(ob->get_order(8), nullptr)
        << "residual must not remain owned by the temporary taker adapter";

    std::vector<fill_event> fills;
    ASSERT_TRUE(hybrid.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-12);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 1.0, 1e-12);

    fills.clear();
    hybrid.on_trade("X", 100.0, 2.0, t_at(1));
    ASSERT_TRUE(hybrid.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 8u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-12);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 0.0, 1e-12);
}

TEST(BacktestDefects, HybridPaper_LocalOrderNeverDefinesVenueMarketability)
{
    auto ob = std::make_shared<orderbook>();
    const std::pair<Price, quantity> bids[] = {
        {Price::from_double(99.0), 10}};
    const std::pair<Price, quantity> asks[] = {
        {Price::from_double(101.0), 10}};
    ob->apply_l2_snapshot(bids, 1, asks, 1);
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 7001, side::sell,
        Price::from_double(100.0), 2));

    auto local = std::make_shared<LocalBookAdapter>(
        ob, nullptr, nullptr, 42, 1.1, 1.0);
    auto queue = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<BackCancelModel>());
    HybridPaperAdapter hybrid(local, queue, ob, make_hybrid_taker(ob));

    order_event passive(t0(), "X", order_type::limit,
                        order_side::buy, 1.0, 100.0);
    passive.set_order_id(7002);
    passive.set_earliest_eligible_ts(t0());
    hybrid.submit_order(passive);

    EXPECT_EQ(queue->live_order_count(), 1u);
    EXPECT_NE(ob->get_order(7001), nullptr);
}

TEST(BacktestDefects, HybridPaper_ExternalTakerSkipsBetterLocalContra)
{
    auto ob = std::make_shared<orderbook>();
    const std::pair<Price, quantity> bids[] = {
        {Price::from_double(99.0), 10}};
    const std::pair<Price, quantity> asks[] = {
        {Price::from_double(101.0), 10}};
    ob->apply_l2_snapshot(bids, 1, asks, 1);
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 7101, side::sell,
        Price::from_double(100.0), 2));

    auto local = std::make_shared<LocalBookAdapter>(
        ob, nullptr, nullptr, 42, 1.1, 1.0);
    auto queue = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<BackCancelModel>());
    HybridPaperAdapter hybrid(local, queue, ob, make_hybrid_taker(ob));

    order_event crossing(t0(), "X", order_type::limit,
                         order_side::buy, 1.0, 101.0);
    crossing.set_order_id(7102);
    crossing.set_earliest_eligible_ts(t0());
    hybrid.submit_order(crossing);

    std::vector<fill_event> fills;
    ASSERT_TRUE(hybrid.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 101.0);
    EXPECT_NE(ob->get_order(7101), nullptr);
}

// ── FR-03: fill-fade matches book qty ──────────────────────────────────────

TEST(BacktestDefects, FR03_FillFade_BookAndPortfolioQtyAgree)
{
    auto ob = std::make_shared<orderbook>();
    // Resting ask 10 units at 100.
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9001,
        side::sell, Price::from_double(100.0), 10));

    auto fade = std::make_shared<RealisticFillModel>(/*fade*/0.5, /*base_prob*/1.0, /*decay*/0.0);
    LocalBookAdapter adapter(ob, nullptr, fade, 42, 1.1, /*qty_scale=*/1.0);
    adapter.set_mid_price(100.0);

    order_event o(t0(), "X", order_type::market, order_side::buy, 10.0, 100.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(t0());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    double fill_qty = 0;
    for (const auto& f : fills) fill_qty += f.get_filled_quantity();
    // Fade 0.5 → match 5 of 10; book should have 5 remaining on the ask.
    EXPECT_NEAR(fill_qty, 5.0, 1e-9);

    // Remaining ask liquidity: 10 - 5 = 5.
    auto infos = ob->get_order_infos();
    ASSERT_FALSE(infos.get_asks().empty());
    EXPECT_NEAR(static_cast<double>(infos.get_asks().front().quantity_), 5.0, 1e-9);
}

// ── EL-02: batch advance completes cancel latency ──────────────────────────

TEST(BacktestDefects, EL02_BatchAdvanceCompletesCancel)
{
    auto ob = std::make_shared<orderbook>();
    auto lat = std::make_shared<FixedLatencyModel>(
        latency_duration(0), latency_duration(0),
        std::chrono::duration_cast<latency_duration>(100ms));
    LocalBookAdapter adapter(ob, nullptr, nullptr, 42, 1.1, 1e8, lat);

    order_event buy(t0(), "X", order_type::limit, order_side::buy, 10, 100.0);
    buy.set_order_id(42);
    buy.set_earliest_eligible_ts(t0());
    adapter.submit_order(buy);
    adapter.advance_time(t0());
    EXPECT_TRUE(adapter.cancel_order(42));
    // Before window: order still live (tracked via cancel pending).
    adapter.advance_time(t0() + 50ms);
    // After window: cancel applied.
    adapter.advance_time(t0() + 200ms);

    // Counter sell should NOT fill against our cancelled buy.
    order_event counter(t0() + 250ms, "X", order_type::limit,
                        order_side::sell, 10, 100.0);
    counter.set_order_id(99);
    counter.set_earliest_eligible_ts(t0() + 250ms);
    adapter.submit_order(counter);
    std::vector<fill_event> fills;
    adapter.poll_fills(fills);
    for (const auto& f : fills)
        EXPECT_NE(f.get_order_id(), 99u) << "cancel window must have drained";
}

// ── EL-01: tick path single strategy dispatch ──────────────────────────────

TEST(BacktestDefects, HybridModify_QueueOnlyLimitFailsClosed)
{
    auto ob = std::make_shared<orderbook>();
    auto local = std::make_shared<LocalBookAdapter>(ob, nullptr, nullptr, 42, 1.1, 1.0);
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<UniformCancelModel>());
    HybridPaperAdapter hybrid(local, qa, ob, make_hybrid_taker(ob));

    order_event lim(t0(), "X", order_type::limit, order_side::buy, 5.0, 99.0);
    lim.set_order_id(7);
    lim.set_earliest_eligible_ts(t0());
    hybrid.submit_order(lim);
    ASSERT_EQ(qa->live_order_count(), 1u);

    // Must NOT cancel the queue order and claim amend success.
    EXPECT_FALSE(hybrid.modify_order(7, 98.0, 5.0));
    EXPECT_EQ(qa->live_order_count(), 1u) << "modify must leave queue order live";
}

TEST(BacktestDefects, HybridCancel_UnknownIdReturnsFalse)
{
    auto ob = std::make_shared<orderbook>();
    auto local = std::make_shared<LocalBookAdapter>(ob, nullptr, nullptr, 42, 1.1, 1.0);
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<UniformCancelModel>());
    HybridPaperAdapter hybrid(local, qa, ob, make_hybrid_taker(ob));
    EXPECT_FALSE(hybrid.cancel_order(99999));
}

TEST(BacktestDefects, HybridPaper_SweepForwardsToQueueAware)
{
    auto ob = std::make_shared<orderbook>();
    auto local = std::make_shared<LocalBookAdapter>(ob, nullptr, nullptr, 42, 1.1, 1.0);
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<UniformCancelModel>());
    HybridPaperAdapter hybrid(local, qa, ob, make_hybrid_taker(ob));

    order_event lim(t0(), "X", order_type::limit, order_side::buy, 5.0, 99.5);
    lim.set_order_id(1);
    lim.set_earliest_eligible_ts(t0());
    hybrid.submit_order(lim);
    ASSERT_EQ(qa->live_order_count(), 1u);

    // Wick through 99.5 with close away from limit.
    EXPECT_TRUE(hybrid.sweep_resting_range("X", 99.0, 100.5, t_at(100)));
    std::vector<fill_event> fills;
    ASSERT_TRUE(hybrid.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 99.5, 1e-9);
    EXPECT_EQ(qa->live_order_count(), 0u);
}

// ── HIGH-01: DAY limit under hybrid is cleared at EOS (no residual live queue) ─

TEST(BacktestDefects, FR_LocalModify_RebindsRestingForBarSweep)
{
    // FR-local-modify-drops-resting: after amend, bar sweep must still fill.
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter a(ob, nullptr, nullptr, 42, 1.1, 1.0);

    order_event lim(t0(), "X", order_type::limit, order_side::buy, 5.0, 90.0);
    lim.set_order_id(1);
    lim.set_earliest_eligible_ts(t0());
    a.submit_order(lim);

    ASSERT_TRUE(a.modify_order(1, 99.5, 5.0));
    // Bar low 99 touches amended 99.5 — would miss if resting_ was erased.
    EXPECT_TRUE(a.sweep_resting_range("X", 99.0, 100.5, t_at(100), 100.0));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 99.5, 1e-9);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 5.0, 1e-9);
    EXPECT_NEAR(a.last_sweep_fill_qty(), 5.0, 1e-9);
}

// HIGH-01: unknown amend must fail closed (no book mutation).
TEST(BacktestDefects, LocalModify_UnknownIdReturnsFalse)
{
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter a(ob, nullptr, nullptr, 42, 1.1, 1.0);
    EXPECT_FALSE(a.modify_order(/*unknown*/ 999, 100.0, 1.0));
    EXPECT_EQ(ob->size(), 0u);
}

// HIGH-01: after successful amend, second bar-sweep uses live remaining body.
TEST(BacktestDefects, LocalModify_PartialSweepUsesLiveBodyRemaining)
{
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter a(ob, nullptr, nullptr, 42, 1.1, 1.0);

    order_event lim(t0(), "X", order_type::limit, order_side::buy, 10.0, 99.5);
    lim.set_order_id(1);
    lim.set_earliest_eligible_ts(t0());
    a.submit_order(lim);
    ASSERT_TRUE(a.modify_order(1, 99.0, 10.0)); // reprice + same qty

    EXPECT_TRUE(a.sweep_resting_range("X", 98.0, 100.0, t_at(100), /*vol=*/4.0));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_NEAR(fills[0].get_filled_quantity(), 4.0, 1e-9);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 6.0, 1e-9);
    EXPECT_NEAR(fills[0].get_fill_price(), 99.0, 1e-9);

    // Residual must still be trackable from live body after partial.
    EXPECT_TRUE(a.sweep_resting_range("X", 98.0, 100.0, t_at(200), /*vol=*/6.0));
    fills.clear();
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_NEAR(fills[0].get_filled_quantity(), 6.0, 1e-9);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 0.0, 1e-9);
}

// MEDIUM-03: hybrid volume budget — local residual + queue limit ≤ bar_volume.
// Dual-leg: seed local directly (bypassing hybrid submit which routes limits
// to queue only) so both backends can fill on the same bar sweep.
TEST(BacktestDefects, HybridSweep_VolumeBudgetNotDoubleApplied)
{
    auto ob = std::make_shared<orderbook>();
    auto local = std::make_shared<LocalBookAdapter>(ob, nullptr, nullptr, 42, 1.1, 1.0);
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<UniformCancelModel>());
    HybridPaperAdapter hybrid(local, qa, ob, make_hybrid_taker(ob));

    // Local-only resting limit qty 10 @ 99.0
    order_event local_lim(t0(), "X", order_type::limit, order_side::buy, 10.0, 99.0);
    local_lim.set_order_id(1);
    local_lim.set_earliest_eligible_ts(t0());
    local->submit_order(local_lim);

    // Queue resting limit qty 10 @ 99.5 (via hybrid submit)
    order_event q_lim(t0(), "X", order_type::limit, order_side::buy, 10.0, 99.5);
    q_lim.set_order_id(2);
    q_lim.set_earliest_eligible_ts(t0());
    hybrid.submit_order(q_lim);
    ASSERT_EQ(qa->live_order_count(), 1u);

    // Bar volume 5: without residual budget, local 10 + queue 10 could fill 20.
    // With subtract of local last_sweep_fill_qty, total must be ≤ 5.
    EXPECT_TRUE(hybrid.sweep_resting_range("X", 98.0, 100.5, t_at(100), /*vol=*/5.0));
    std::vector<fill_event> fills;
    ASSERT_TRUE(hybrid.poll_fills(fills));
    double total = 0.0;
    for (const auto& f : fills) total += f.get_filled_quantity();
    EXPECT_LE(total, 5.0 + 1e-9) << "must not over-fill vs bar volume across backends";
    EXPECT_NEAR(total, 5.0, 1e-9);
    EXPECT_NEAR(local->last_sweep_fill_qty(), 5.0, 1e-9)
        << "local should consume full budget first";
}

// MEDIUM-04: QueueAware caps undrained pending fills.
TEST(BacktestDefects, QueueAware_PendingFillsCapFailClosed)
{
    auto qa = std::make_shared<QueueAwareBookAdapter>(
        std::make_shared<UniformCancelModel>());
    qa->set_join_front_without_l2(true);
    qa->set_max_pending_fills(1);

    order_event a(t0(), "X", order_type::limit, order_side::buy, 1.0, 100.0);
    a.set_order_id(1);
    a.set_earliest_eligible_ts(t0());
    order_event b(t0(), "X", order_type::limit, order_side::buy, 1.0, 100.0);
    b.set_order_id(2);
    b.set_earliest_eligible_ts(t_at(1));
    qa->submit_order(a);
    qa->submit_order(b);

    // Two fills available from tape; cap=1 → only one emplaced, one dropped.
    qa->on_trade("X", 100.0, 10.0, t_at(100));
    EXPECT_GE(qa->dropped_fills_for_cap(), 1u);
    std::vector<fill_event> fills;
    ASSERT_TRUE(qa->poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
    // Second order still live (fill refused, qty not reduced) or first filled.
    EXPECT_GE(qa->live_order_count(), 1u);
}

// HIGH-03: MC reuse refuses strategies without supports_mc_trial_reuse.
TEST(BacktestDefects, FR_BarSweep_VolumeCapsFullRemaining)
{
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter a(ob, nullptr, nullptr, 42, 1.1, 1.0);

    order_event lim(t0(), "X", order_type::limit, order_side::buy, 10.0, 99.5);
    lim.set_order_id(1);
    lim.set_earliest_eligible_ts(t0());
    a.submit_order(lim);

    // Volume 3 of 10 → partial, residual stays resting for next bar.
    EXPECT_TRUE(a.sweep_resting_range("X", 99.0, 100.5, t_at(100), /*bar_volume=*/3.0));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 7.0, 1e-9);

    EXPECT_TRUE(a.sweep_resting_range("X", 99.0, 100.5, t_at(200), /*bar_volume=*/7.0));
    fills.clear();
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_NEAR(fills[0].get_filled_quantity(), 7.0, 1e-9);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 0.0, 1e-9);
}

TEST(BacktestDefects, BF11_VolumeCappedSweepFillsInSubmitOrder)
{
    for (int repeat = 0; repeat < 16; ++repeat)
    {
        SCOPED_TRACE(repeat);
        auto ob = std::make_shared<orderbook>();
        LocalBookAdapter a(ob, nullptr, nullptr, 42, 1.1, 1.0);

        const uint64_t first_id = (repeat % 2 == 0) ? 20 : 10;
        const uint64_t second_id = (repeat % 2 == 0) ? 10 : 20;
        const uint64_t third_id = 30;
        const auto submit = [&](uint64_t id, auto ts, const char* symbol = "X") {
            order_event o(ts, symbol, order_type::limit,
                          order_side::buy, 3.0, 99.5);
            o.set_order_id(id);
            o.set_earliest_eligible_ts(ts);
            a.submit_order(o);
        };

        submit(first_id, t_at(10));
        if (repeat % 2 != 0)
        {
            for (uint64_t i = 0; i < 32; ++i)
                submit(1000 + i, t_at(11), "NOISE");
            for (uint64_t i = 0; i < 32; ++i)
                ASSERT_TRUE(a.cancel_order(1000 + i));
        }
        submit(second_id, t_at(20));
        submit(third_id, t_at(30));

        ASSERT_TRUE(a.sweep_resting_range(
            "X", 99.0, 100.5, t_at(100), /*bar_volume=*/4.0));
        std::vector<fill_event> fills;
        ASSERT_TRUE(a.poll_fills(fills));
        ASSERT_EQ(fills.size(), 2u);
        EXPECT_EQ(fills[0].get_order_id(), first_id);
        EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
        EXPECT_NEAR(fills[0].get_remaining_qty(), 0.0, 1e-9);
        EXPECT_EQ(fills[1].get_order_id(), second_id);
        EXPECT_NEAR(fills[1].get_filled_quantity(), 1.0, 1e-9);
        EXPECT_NEAR(fills[1].get_remaining_qty(), 2.0, 1e-9);
        EXPECT_NEAR(a.last_sweep_fill_qty(), 4.0, 1e-9);

        fills.clear();
        ASSERT_TRUE(a.sweep_resting_range(
            "X", 99.0, 100.5, t_at(200), /*bar_volume=*/3.0));
        ASSERT_TRUE(a.poll_fills(fills));
        ASSERT_EQ(fills.size(), 2u);
        EXPECT_EQ(fills[0].get_order_id(), second_id);
        EXPECT_NEAR(fills[0].get_filled_quantity(), 2.0, 1e-9);
        EXPECT_NEAR(fills[0].get_remaining_qty(), 0.0, 1e-9);
        EXPECT_EQ(fills[1].get_order_id(), third_id);
        EXPECT_NEAR(fills[1].get_filled_quantity(), 1.0, 1e-9);
        EXPECT_NEAR(fills[1].get_remaining_qty(), 2.0, 1e-9);
        EXPECT_NEAR(a.last_sweep_fill_qty(), 3.0, 1e-9);
    }
}

TEST(BacktestDefects, BF11_ModifyMovesRestingOrderToFifoBack)
{
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter a(ob, nullptr, nullptr, 42, 1.1, 1.0);
    const auto submit = [&](uint64_t id, auto ts) {
        order_event o(ts, "X", order_type::limit,
                      order_side::buy, 3.0, 99.5);
        o.set_order_id(id);
        o.set_earliest_eligible_ts(ts);
        a.submit_order(o);
    };
    submit(20, t_at(10));
    submit(10, t_at(20));
    ASSERT_TRUE(a.modify_order(20, 99.5, 3.0));

    ASSERT_TRUE(a.sweep_resting_range(
        "X", 99.0, 100.5, t_at(100), /*bar_volume=*/4.0));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].get_order_id(), 10u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
    EXPECT_EQ(fills[1].get_order_id(), 20u);
    EXPECT_NEAR(fills[1].get_filled_quantity(), 1.0, 1e-9);
}

TEST(BacktestDefects, BF11_CancelUnlinksWithoutReorderingSurvivors)
{
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter a(ob, nullptr, nullptr, 42, 1.1, 1.0);
    for (const uint64_t id : {30u, 20u, 10u})
    {
        order_event o(t0(), "X", order_type::limit,
                      order_side::buy, 3.0, 99.5);
        o.set_order_id(id);
        o.set_earliest_eligible_ts(t0());
        a.submit_order(o);
    }
    ASSERT_TRUE(a.cancel_order(20));

    ASSERT_TRUE(a.sweep_resting_range(
        "X", 99.0, 100.5, t_at(100), /*bar_volume=*/4.0));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].get_order_id(), 30u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
    EXPECT_EQ(fills[1].get_order_id(), 10u);
    EXPECT_NEAR(fills[1].get_filled_quantity(), 1.0, 1e-9);
}
