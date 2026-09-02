// R3 Phase 1 — authoritative order ledger.
//
// Covers the lifecycle matrix the risk path depends on: open-order count and
// remaining quantity after every state transition, partial fills, terminal
// states, duplicate fills, cancel-after-partial, and per-symbol pending
// exposure. See docs/internal/r3-authoritative-risk-accounting.md.

#include <gtest/gtest.h>

#include "execution/order_tracker.h"

#include <chrono>
#include <limits>
#include <string>

namespace {

auto epoch_ms(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

order_event make_order(std::uint64_t id, const std::string& symbol,
                       order_side side, double qty, double price = 100.0,
                       std::int64_t ts_ms = 0)
{
    order_event o(epoch_ms(ts_ms), symbol, order_type::limit, side, qty, price);
    o.set_order_id(id);
    return o;
}

fill_event make_fill(std::uint64_t order_id, const std::string& symbol,
                     order_side side, double qty, double remaining,
                     std::uint64_t fill_id = 0, std::int64_t ts_ms = 0)
{
    return fill_event(epoch_ms(ts_ms), symbol, order_id, side, qty, 100.0, 0.0,
                      remaining, fill_id);
}

fill_event make_exchange_fill(std::uint64_t order_id,
                              const std::string& symbol,
                              order_side side,
                              double qty,
                              double remaining,
                              std::uint64_t local_fill_id,
                              std::string_view native_execution_id,
                              double cumulative,
                              double price = 100.0,
                              double commission = 0.0,
                              std::int64_t ts_ms = 1)
{
    fill_event fill(epoch_ms(ts_ms), symbol, order_id, side, qty, price,
                    commission, remaining, local_fill_id);
    fill.set_source(fill_source::exchange);
    EXPECT_TRUE(fill.set_venue_execution_id(native_execution_id));
    EXPECT_TRUE(fill.set_commission_currency("USDT"));
    fill.set_cumulative_filled_qty(
        cumulative, fill_cumulative_source::venue_reported);
    return fill;
}

// Submits + acknowledges one order and returns the ledger to the caller.
void submit_and_ack(OrderTracker& ledger, const order_event& o)
{
    ledger.register_order(o);
    ledger.set_status(o.get_order_id(), order_status::pending);
    ledger.set_status(o.get_order_id(), order_status::open);
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle matrix
// ---------------------------------------------------------------------------

TEST(OrderLedger, SubmitAckFill)
{
    OrderTracker ledger;
    const auto o = make_order(1, "BTCUSDT", order_side::buy, 5.0);

    ledger.register_order(o);
    EXPECT_EQ(ledger.get_order_status(1), order_status::unknown);
    EXPECT_EQ(ledger.active_count(), 0u);

    ledger.set_status(1, order_status::pending);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 5.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 5.0);

    ledger.set_status(1, order_status::open);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 5.0);

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 5.0, 0.0));
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 5.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
    EXPECT_EQ(ledger.open_exposure("BTCUSDT").open_order_count, 0u);
}

TEST(OrderLedger, SubmitAckPartialFillThenFill)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0));
    EXPECT_EQ(ledger.get_order_status(1), order_status::partially_filled);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 6.0);

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 6.0, 0.0));
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 10.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
}

TEST(OrderLedger, SubmitAckPartialFillThenCancelReleasesOnlyTheRemainder)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::sell, 10.0));

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::sell, 3.0, 7.0));
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_sell_qty, 7.0);

    ledger.set_status(1, order_status::cancelled);
    EXPECT_EQ(ledger.get_order_status(1), order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);
    // The filled 3.0 became a position (portfolio's job); only the unfilled
    // remainder leaves pending exposure.
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 3.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_sell_qty, 0.0);
    EXPECT_EQ(ledger.open_exposure("BTCUSDT").open_order_count, 0u);
}

TEST(OrderLedger, RejectCarriesNoOpenStateOrExposure)
{
    OrderTracker ledger;
    const auto o = make_order(7, "ETHUSDT", order_side::buy, 2.0);
    ledger.register_order(o);
    ledger.set_status(7, order_status::pending);
    EXPECT_EQ(ledger.active_count(), 1u);

    ledger.set_status(7, order_status::rejected);
    EXPECT_EQ(ledger.get_order_status(7), order_status::rejected);
    EXPECT_TRUE(order_status_is_terminal(order_status::rejected));
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(7), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("ETHUSDT").open_buy_qty, 0.0);
}

TEST(OrderLedger, ExpireIsTerminalAndDistinctFromCancel)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(9, "ETHUSDT", order_side::buy, 4.0));
    EXPECT_EQ(ledger.active_count(), 1u);

    ledger.set_status(9, order_status::expired);
    EXPECT_EQ(ledger.get_order_status(9), order_status::expired);
    EXPECT_NE(ledger.get_order_status(9), order_status::cancelled);
    EXPECT_TRUE(order_status_is_terminal(order_status::expired));
    EXPECT_FALSE(order_status_is_open(order_status::expired));
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("ETHUSDT").open_buy_qty, 0.0);
    EXPECT_STREQ(to_string(order_status::expired), "expired");
}

TEST(OrderLedger, CancelWithoutFillReleasesEverything)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(3, "BTCUSDT", order_side::buy, 8.0));
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 8.0);

    ledger.set_status(3, order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(3), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
}

TEST(OrderLedger, MultiplePartialFillsAccumulateExactly)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 9.0));

    const double steps[] = {2.0, 3.0, 1.5, 2.5};
    double filled = 0.0;
    for (double step : steps)
    {
        filled += step;
        ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, step, 9.0 - filled));
        EXPECT_DOUBLE_EQ(ledger.filled_qty(1), filled);
        EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 9.0 - filled);
        EXPECT_NEAR(ledger.open_exposure("BTCUSDT").open_buy_qty, 9.0 - filled, 1e-12);
        EXPECT_EQ(ledger.active_count(), filled < 9.0 ? 1u : 0u);
    }
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
}

TEST(OrderLedger, DuplicateFillIsNotBookedTwice)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    EXPECT_TRUE(ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0, /*fill_id=*/77)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);

    // Same venue fill re-delivered (reconciler replay / transport retry).
    EXPECT_FALSE(ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0, /*fill_id=*/77)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 6.0);

    // A genuinely different fill id still applies.
    EXPECT_TRUE(ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 6.0, 0.0, /*fill_id=*/78)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 10.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
}

TEST(OrderLedger, ReusedLocalFillIdWithChangedEconomicsIsRejected)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));
    ASSERT_TRUE(ledger.on_fill(make_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 77)));

    // A composite adapter must restamp child-local IDs before this boundary.
    // Once admitted here, reusing an identity for different economics is a
    // reconciliation conflict rather than evidence of a second fill.
    const auto changed = make_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 2.0, 77);
    const auto result = ledger.validate_fill(changed);
    EXPECT_TRUE(result.rejected());
    EXPECT_EQ(result.code, fill_apply_code::native_identity_conflict);
    EXPECT_FALSE(ledger.on_fill(changed));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
}

TEST(OrderLedger, DecimalPartialSlicesReachExactCumulativeCursor)
{
    OrderTracker ledger(OrderTracker::default_native_execution_capacity,
                        /*quantity_quantum=*/1e-8);
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, 0.3, 100.0));

    ASSERT_TRUE(ledger.on_fill(make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 0.1, 0.2, 1,
        "native-decimal-1", 0.1)));
    const auto second = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 0.2, 0.0, 2,
        "native-decimal-2", 0.3);
    const auto result = ledger.validate_fill(second);
    EXPECT_TRUE(result.applied());
    EXPECT_TRUE(ledger.on_fill(second));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.3);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
}

TEST(OrderLedger,
     RegistrationCanonicalizesQuantityToTheAdapterGridBeforeFillValidation)
{
    constexpr double quantum = 1e-8;
    constexpr double requested_qty = 8333.3333333333339;
    constexpr double adapter_qty = 8333.33333333;

    OrderTracker ledger(OrderTracker::default_native_execution_capacity,
                        quantum);
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, requested_qty, 120.0));

    // LocalBookAdapter represents quantities as integer(qty * 1e8), so its
    // first 100-unit slice reports this exact grid-aligned remainder.  The
    // authoritative ledger must canonicalize the order total at registration
    // instead of comparing a raw strategy double with an adapter-grid value.
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), adapter_qty);
    fill_event fill(
        epoch_ms(2), "BTCUSDT", 1, order_side::buy,
        /*qty=*/100.0, /*price=*/200.0, /*commission=*/0.0,
        /*remaining=*/8233.33333333, /*fill_id=*/1);
    fill.set_source(fill_source::simulated);

    const auto validation = ledger.validate_fill(
        fill, /*require_exchange_identity=*/false,
        /*require_fill_identity=*/true);
    ASSERT_TRUE(validation.applied()) << to_string(validation.code);
    EXPECT_DOUBLE_EQ(validation.cumulative_qty, 100.0);
    EXPECT_DOUBLE_EQ(validation.economic_quantity, 100.0);
    ASSERT_TRUE(ledger.commit_fill(fill, validation));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 100.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 8233.33333333);
}

TEST(OrderLedger, LargeUlpRawSliceCannotBeNormalizedIntoOrderTotal)
{
    constexpr double original = 0x1p60;
    const double raw_overfill = std::nextafter(
        original, std::numeric_limits<double>::infinity());
    ASSERT_GT(raw_overfill - original, 1.0);

    OrderTracker ledger(OrderTracker::default_native_execution_capacity,
                        /*quantity_quantum=*/1.0);
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, original, 1.0));
    const auto overfill = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, raw_overfill, 0.0, 1,
        "native-raw-ulp-overfill", original, 1.0);

    const auto result = ledger.validate_fill(overfill);
    EXPECT_TRUE(result.rejected());
    EXPECT_FALSE(ledger.on_fill(overfill));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), original);
}

TEST(OrderLedger, OldReplayRemainsIdempotentAfterMoreThanFourLaterFills)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    for (std::uint64_t id = 1; id <= 5; ++id)
    {
        ASSERT_TRUE(ledger.on_fill(make_fill(
            1, "BTCUSDT", order_side::buy, 1.0,
            10.0 - static_cast<double>(id), id)));
    }
    ASSERT_DOUBLE_EQ(ledger.filled_qty(1), 5.0);

    // Reconnect replay of the first execution after it has fallen out of the
    // old four-slot ring. Its venue-reported remaining quantity identifies
    // the already-applied cumulative state and must make the operation a
    // read-only duplicate.
    EXPECT_FALSE(ledger.on_fill(make_fill(
        1, "BTCUSDT", order_side::buy, 1.0, 9.0, /*fill_id=*/1)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 5.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 5.0);
}

TEST(OrderLedger, UnknownOrIdentityMismatchedFillHasNoMutation)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));
    const auto orders_before = ledger.orders_seen();

    EXPECT_FALSE(ledger.on_fill(make_fill(
        999, "BTCUSDT", order_side::buy, 1.0, 0.0, 91)));
    EXPECT_EQ(ledger.orders_seen(), orders_before);

    EXPECT_FALSE(ledger.on_fill(make_fill(
        1, "ETHUSDT", order_side::buy, 1.0, 4.0, 92)));
    EXPECT_FALSE(ledger.on_fill(make_fill(
        1, "BTCUSDT", order_side::sell, 1.0, 4.0, 93)));

    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 5.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::open);
}

TEST(OrderLedger, MalformedFillHasNoLifecycleOrQuantityMutation)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));

    const double invalid_values[] = {
        0.0,
        -1.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()
    };
    std::uint64_t fill_id = 100;
    for (const double invalid_qty : invalid_values)
    {
        auto bad = make_fill(
            1, "BTCUSDT", order_side::buy, invalid_qty, 5.0, fill_id++);
        EXPECT_FALSE(ledger.on_fill(bad));
    }

    auto bad_price = make_fill(
        1, "BTCUSDT", order_side::buy, 1.0, 4.0, fill_id++);
    bad_price = fill_event(
        bad_price.get_timestamp(), bad_price.get_symbol(),
        bad_price.get_order_id(), bad_price.get_side(),
        bad_price.get_filled_quantity(),
        std::numeric_limits<double>::quiet_NaN(), 0.0,
        bad_price.get_remaining_qty(), bad_price.get_fill_id());
    EXPECT_FALSE(ledger.on_fill(bad_price));

    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 5.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::open);
}

TEST(OrderLedger, ExchangeFillRequiresNativeIdentityCumulativeAndFeeCurrency)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));

    auto valid = make_fill(
        1, "BTCUSDT", order_side::buy, 2.0, 3.0, 101,
        /*ts_ms=*/1);
    valid.set_source(fill_source::exchange);
    valid.set_cumulative_filled_qty(
        2.0, fill_cumulative_source::venue_reported);

    EXPECT_FALSE(ledger.on_fill(valid));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);

    ASSERT_TRUE(valid.set_venue_execution_id("venue-fill-101"));
    EXPECT_FALSE(ledger.on_fill(valid));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);

    ASSERT_TRUE(valid.set_commission_currency("USDT"));
    EXPECT_TRUE(ledger.on_fill(valid));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 2.0);
}

TEST(OrderLedger, TrustedLiveIngressCannotSelectWeakerValidationBySourceLabel)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));
    auto mislabeled = make_fill(
        1, "BTCUSDT", order_side::buy, 2.0, 3.0, 101);
    mislabeled.set_cumulative_filled_qty(
        2.0, fill_cumulative_source::engine_accumulated);

    const auto result = ledger.validate_fill(
        mislabeled, /*require_exchange_identity=*/true);
    EXPECT_TRUE(result.rejected());
    EXPECT_EQ(result.code, fill_apply_code::invalid_identity);
    EXPECT_FALSE(ledger.commit_fill(mislabeled, result));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
}

TEST(OrderLedger, ExchangeFillWithEpochZeroTimestampIsRejected)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, 5.0, 100.0, 1000));
    const auto invalid_time = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 2.0, 3.0, 101,
        "native-zero-time", 2.0, 100.0, 0.0, 0);

    EXPECT_TRUE(ledger.validate_fill(invalid_time).rejected());
    EXPECT_FALSE(ledger.on_fill(invalid_time));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
}

TEST(OrderLedger, ExchangeNativeIdConflictCannotAdvanceCumulative)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    ASSERT_TRUE(ledger.on_fill(make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 1, "native-X", 4.0)));
    ASSERT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);

    const auto changed = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 2.0, 2, "native-X", 8.0);
    const auto result = ledger.validate_fill(changed);
    EXPECT_TRUE(result.rejected());
    EXPECT_FALSE(ledger.commit_fill(changed, result));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::partially_filled);
}

TEST(OrderLedger, NativeExecutionCannotBeReboundToAnotherLocalOrder)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 1.0));
    submit_and_ack(ledger, make_order(2, "BTCUSDT", order_side::buy, 1.0));
    ASSERT_TRUE(ledger.on_fill(make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 1.0, 0.0, 1, "native-X", 1.0)));

    const auto rebound = make_exchange_fill(
        2, "BTCUSDT", order_side::buy, 1.0, 0.0, 2, "native-X", 1.0);
    const auto result = ledger.validate_fill(rebound);
    EXPECT_EQ(result.code, fill_apply_code::native_identity_conflict);
    EXPECT_FALSE(ledger.on_fill(rebound));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 1.0);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(2), 0.0);
}

TEST(OrderLedger, ExchangeNativeIdIgnoresChangedLocalDeliveryId)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    ASSERT_TRUE(ledger.on_fill(make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 1, "native-X", 4.0,
        100.0, 0.25, 1000)));
    const auto replay = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 999, "native-X", 4.0,
        100.0, 0.25, 1000);
    const auto result = ledger.validate_fill(replay);
    EXPECT_TRUE(result.idempotent_noop());
    EXPECT_FALSE(ledger.on_fill(replay));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
}

TEST(OrderLedger, AdmissionTokenCannotBeReboundToAnotherNativeExecution)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    const auto fill_a = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 1, "native-A", 4.0);
    const auto token_a = ledger.validate_fill(fill_a);
    ASSERT_TRUE(token_a.applied());

    const auto fill_b = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 2, "native-B", 4.0,
        101.0);
    EXPECT_FALSE(ledger.commit_fill(fill_b, token_a));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 10.0);
}

TEST(OrderLedger, ChangedOldNativeReplayIsNotSilentlyStale)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));
    ASSERT_TRUE(ledger.on_fill(make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 2.0, 8.0, 1, "native-X", 2.0,
        100.0, 0.0, 1000)));
    ASSERT_TRUE(ledger.on_fill(make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 2.0, 6.0, 2, "native-Y", 4.0,
        100.0, 0.0, 2000)));

    const auto changed_old = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 2.0, 8.0, 3, "native-X", 2.0,
        101.0, 0.0, 1000);
    const auto result = ledger.validate_fill(changed_old);
    EXPECT_TRUE(result.rejected());
    EXPECT_EQ(result.code, fill_apply_code::native_identity_conflict);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
}

TEST(OrderLedger, UnseenNativeIdAtCommittedCursorIsRejected)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));
    ASSERT_TRUE(ledger.on_fill(make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 1, "native-X", 4.0)));

    const auto unseen = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 2, "native-Y", 4.0);
    const auto result = ledger.validate_fill(unseen);
    EXPECT_TRUE(result.rejected());
    EXPECT_FALSE(ledger.on_fill(unseen));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_EQ(ledger.native_execution_count(), 1u);
}

TEST(OrderLedger, NativeRegistryCapacityExhaustionIsTransactional)
{
    OrderTracker ledger(/*native_execution_capacity=*/1);
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));
    const auto first = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 1, "native-X", 4.0);
    ASSERT_TRUE(ledger.on_fill(first));
    ASSERT_EQ(ledger.native_execution_count(), 1u);

    const auto* before = ledger.find(1);
    ASSERT_NE(before, nullptr);
    const auto revision_before = before->revision;
    const auto exposure_before = ledger.open_exposure("BTCUSDT");
    const auto second = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 2.0, 4.0, 2, "native-Y", 6.0);
    const auto result = ledger.validate_fill(second);
    EXPECT_EQ(result.code,
              fill_apply_code::native_identity_capacity_exhausted);
    EXPECT_FALSE(ledger.on_fill(second));

    const auto* after = ledger.find(1);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->revision, revision_before);
    EXPECT_DOUBLE_EQ(after->filled_qty, 4.0);
    EXPECT_EQ(after->status, order_status::partially_filled);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty,
                     exposure_before.open_buy_qty);
    EXPECT_EQ(ledger.native_execution_count(), 1u);

    const auto exact_replay = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 4.0, 6.0, 999, "native-X", 4.0);
    EXPECT_TRUE(ledger.validate_fill(exact_replay).idempotent_noop());
    EXPECT_FALSE(ledger.on_fill(exact_replay));
    EXPECT_EQ(ledger.native_execution_count(), 1u);
}

TEST(OrderLedger, LargeOrderFirstUnitFillIsNotCollapsedAsDuplicate)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, 1'000'000'000.0, 1.0));
    const auto first = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 1.0, 999'999'999.0, 1,
        "native-first", 1.0, 1.0);
    const auto result = ledger.validate_fill(first);
    EXPECT_TRUE(result.applied());
    EXPECT_TRUE(ledger.on_fill(first));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 1.0);
}

TEST(OrderLedger, RepresentableUnitAtTwoToThe52IsForwardProgress)
{
    constexpr double original = 4'503'599'627'370'496.0; // 2^52
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, original, 1.0));
    const auto first = make_fill(
        1, "BTCUSDT", order_side::buy, 1.0, original - 1.0, 1);

    EXPECT_TRUE(ledger.validate_fill(first).applied());
    EXPECT_TRUE(ledger.on_fill(first));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 1.0);
}

TEST(OrderLedger, LargeOrderFractionalOverfillIsRejectedWithoutClamp)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, 1'000'000'000.0, 1.0));
    const auto overfill = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, 1'000'000'000.5, 0.0, 1,
        "native-overfill", 1'000'000'000.5, 1.0);
    const auto result = ledger.validate_fill(overfill);
    EXPECT_TRUE(result.rejected());
    EXPECT_FALSE(ledger.on_fill(overfill));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 1'000'000'000.0);
    EXPECT_EQ(ledger.native_execution_count(), 0u);
}

TEST(OrderLedger, OneUlpOverfillIsRejectedRatherThanClamped)
{
    constexpr double original = 1'000'000'000.0;
    const double over = std::nextafter(
        original, std::numeric_limits<double>::infinity());
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, original, 1.0));
    const auto overfill = make_exchange_fill(
        1, "BTCUSDT", order_side::buy, over, 0.0, 1,
        "native-ulp-overfill", over, 1.0);

    EXPECT_TRUE(ledger.validate_fill(overfill).rejected());
    EXPECT_FALSE(ledger.on_fill(overfill));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), original);
}

TEST(OrderLedger, DuplicateProbeIsReadOnlyAndMatchesFillDedupe)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::sell, 10.0));

    EXPECT_FALSE(ledger.has_seen_fill(1, 77));
    EXPECT_FALSE(ledger.has_seen_fill(999, 77));
    EXPECT_FALSE(ledger.has_seen_fill(1, 0));

    EXPECT_TRUE(ledger.on_fill(make_fill(1, "BTCUSDT", order_side::sell,
                                         4.0, 6.0, /*fill_id=*/77)));
    EXPECT_TRUE(ledger.has_seen_fill(1, 77));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);

    // The prevalidation query has no lifecycle side effects and agrees with
    // on_fill's bounded duplicate window.
    EXPECT_FALSE(ledger.on_fill(make_fill(1, "BTCUSDT", order_side::sell,
                                          4.0, 6.0, /*fill_id=*/77)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
}

TEST(OrderLedger, FillNeverExceedsOriginalQuantity)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));

    // Venue over-reports (or a stale duplicate with a fresh id arrives).
    EXPECT_FALSE(ledger.on_fill(
        make_fill(1, "BTCUSDT", order_side::buy, 9.0, 0.0, /*fill_id=*/1)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 5.0);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_EQ(ledger.get_order_status(1), order_status::open);
}

// A cancel that lands after the venue already filled the order must not
// resurrect capacity or exposure: the order is terminal either way.
TEST(OrderLedger, CancelAfterFillKeepsOrderTerminal)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));
    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 5.0, 0.0));
    EXPECT_EQ(ledger.active_count(), 0u);

    ledger.set_status(1, order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 5.0);
}

TEST(OrderLedger, C05_AuthoritativeLifecycleIsCausalAndIdempotent)
{
    OrderTracker ledger;
    const auto order = make_order(
        901, "BTCUSDT", order_side::buy, 5.0);
    ASSERT_TRUE(ledger.register_order(order));
    ledger.set_status(901, order_status::pending);
    const auto ack_ts = order.get_timestamp() + std::chrono::milliseconds(1);

    const auto ack = ledger.validate_lifecycle(
        901, order_status::open, ack_ts);
    ASSERT_TRUE(ack.applied());
    ASSERT_TRUE(ledger.commit_lifecycle(ack));
    EXPECT_EQ(ledger.get_order_status(901), order_status::open);

    const auto duplicate = ledger.validate_lifecycle(
        901, order_status::open, ack_ts);
    EXPECT_EQ(duplicate.code, lifecycle_apply_code::duplicate);
    EXPECT_FALSE(ledger.commit_lifecycle(duplicate));

    const auto stale = ledger.validate_lifecycle(
        901, order_status::cancelled,
        order.get_timestamp() - std::chrono::milliseconds(1));
    EXPECT_EQ(stale.code, lifecycle_apply_code::invalid_timestamp);
    EXPECT_EQ(ledger.get_order_status(901), order_status::open);

    const auto impossible = ledger.validate_lifecycle(
        901, order_status::rejected,
        ack_ts + std::chrono::milliseconds(1));
    EXPECT_EQ(impossible.code, lifecycle_apply_code::invalid_transition);
    EXPECT_EQ(ledger.get_order_status(901), order_status::open);

    const auto canceled = ledger.validate_lifecycle(
        901, order_status::cancelled,
        ack_ts + std::chrono::milliseconds(2));
    ASSERT_TRUE(canceled.applied());
    ASSERT_TRUE(ledger.commit_lifecycle(canceled));
    EXPECT_DOUBLE_EQ(ledger.pending_qty(901), 0.0);

    const auto conflicting_terminal = ledger.validate_lifecycle(
        901, order_status::expired,
        ack_ts + std::chrono::milliseconds(3));
    EXPECT_EQ(conflicting_terminal.code,
              lifecycle_apply_code::invalid_transition);
}

TEST(OrderLedger, C05_FillBeforeAckAndCancelAfterFillPreserveEconomics)
{
    OrderTracker partial_ledger;
    const auto partial_order = make_order(
        902, "BTCUSDT", order_side::buy, 5.0, 100.0, 1);
    ASSERT_TRUE(partial_ledger.register_order(partial_order));
    partial_ledger.set_status(902, order_status::pending);
    ASSERT_TRUE(partial_ledger.on_fill(make_fill(
        902, "BTCUSDT", order_side::buy, 2.0, 3.0, 91, 2)));
    ASSERT_EQ(partial_ledger.get_order_status(902),
              order_status::partially_filled);

    const auto late_ack = partial_ledger.validate_lifecycle(
        902, order_status::open, epoch_ms(1));
    EXPECT_EQ(late_ack.code, lifecycle_apply_code::stale);
    EXPECT_DOUBLE_EQ(partial_ledger.filled_qty(902), 2.0);
    EXPECT_DOUBLE_EQ(partial_ledger.pending_qty(902), 3.0);

    OrderTracker filled_ledger;
    const auto filled_order = make_order(
        903, "BTCUSDT", order_side::sell, 5.0, 100.0, 1);
    ASSERT_TRUE(filled_ledger.register_order(filled_order));
    filled_ledger.set_status(903, order_status::pending);
    ASSERT_TRUE(filled_ledger.on_fill(make_fill(
        903, "BTCUSDT", order_side::sell, 5.0, 0.0, 92, 2)));
    ASSERT_EQ(filled_ledger.get_order_status(903), order_status::filled);

    const auto late_cancel = filled_ledger.validate_lifecycle(
        903, order_status::cancelled, epoch_ms(1));
    EXPECT_EQ(late_cancel.code, lifecycle_apply_code::stale);
    EXPECT_DOUBLE_EQ(filled_ledger.filled_qty(903), 5.0);
    EXPECT_DOUBLE_EQ(filled_ledger.pending_qty(903), 0.0);
}

// The reverse race: a venue fill delivered after a cancel acknowledgement may
// have executed before the cancel became effective. Its quantity is real, but
// the acknowledged cancel remains terminal and carries no pending exposure.
TEST(OrderLedger, FillAfterCancelIsExactlyOnceWithoutReopeningExposure)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));
    ledger.set_status(1, order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);

    const auto first = make_fill(
        1, "BTCUSDT", order_side::buy, 2.0, 3.0, /*fill_id=*/81);
    ASSERT_TRUE(ledger.on_fill(first));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 2.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);

    // Transport replay is a read-only no-op even though the order is already
    // terminal. The raw original-minus-filled remainder remains available for
    // reconciliation, but it must not re-enter pre-trade pending exposure.
    EXPECT_FALSE(ledger.on_fill(first));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 2.0);
    const auto* entry = ledger.find(1);
    ASSERT_NE(entry, nullptr);
    EXPECT_DOUBLE_EQ(entry->remaining_qty(), 3.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

TEST(OrderLedger, MultipleOrdersOnSameInstrumentAggregate)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 2.0));
    submit_and_ack(ledger, make_order(2, "BTCUSDT", order_side::buy, 3.0));
    submit_and_ack(ledger, make_order(3, "BTCUSDT", order_side::buy, 4.0));

    auto exp = ledger.open_exposure("BTCUSDT");
    EXPECT_DOUBLE_EQ(exp.open_buy_qty, 9.0);
    EXPECT_EQ(exp.open_order_count, 3u);
    EXPECT_EQ(ledger.active_count(), 3u);

    ledger.set_status(2, order_status::cancelled);
    exp = ledger.open_exposure("BTCUSDT");
    EXPECT_DOUBLE_EQ(exp.open_buy_qty, 6.0);
    EXPECT_EQ(exp.open_order_count, 2u);
    EXPECT_EQ(ledger.active_count(), 2u);
}

TEST(OrderLedger, SimultaneousBuyAndSellStayOnSeparateSides)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 2.0));
    submit_and_ack(ledger, make_order(2, "BTCUSDT", order_side::sell, 5.0));

    auto exp = ledger.open_exposure("BTCUSDT");
    EXPECT_DOUBLE_EQ(exp.open_buy_qty, 2.0);
    EXPECT_DOUBLE_EQ(exp.open_sell_qty, 5.0);
    EXPECT_EQ(exp.open_order_count, 2u);

    ledger.on_fill(make_fill(2, "BTCUSDT", order_side::sell, 1.0, 4.0));
    exp = ledger.open_exposure("BTCUSDT");
    EXPECT_DOUBLE_EQ(exp.open_buy_qty, 2.0);
    EXPECT_DOUBLE_EQ(exp.open_sell_qty, 4.0);
}

TEST(OrderLedger, MultipleInstrumentsDoNotBleedIntoEachOther)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 2.0));
    submit_and_ack(ledger, make_order(2, "ETHUSDT", order_side::buy, 7.0));
    submit_and_ack(ledger, make_order(3, "SOLUSDT", order_side::sell, 11.0));

    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 2.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("ETHUSDT").open_buy_qty, 7.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("SOLUSDT").open_sell_qty, 11.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("SOLUSDT").open_buy_qty, 0.0);
    EXPECT_EQ(ledger.active_count(), 3u);

    ledger.set_status(2, order_status::filled);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("ETHUSDT").open_buy_qty, 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 2.0);
    EXPECT_EQ(ledger.active_count(), 2u);
}

TEST(OrderLedger, UntrackedSymbolIsDistinguishableFromFlat)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 1.0));

    EXPECT_TRUE(ledger.tracks_symbol("BTCUSDT"));
    EXPECT_FALSE(ledger.tracks_symbol("DOGEUSDT"));
    EXPECT_DOUBLE_EQ(ledger.open_exposure("DOGEUSDT").open_buy_qty, 0.0);
    EXPECT_FALSE(ledger.symbol_capacity_exhausted());
}

// ---------------------------------------------------------------------------
// Amendments, reset, and counter independence
// ---------------------------------------------------------------------------

TEST(OrderLedger, AmendUpdatesOriginalQuantitySoTheSlotCanClose)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));
    ledger.amend(1, /*new_price=*/101.0, /*new_qty=*/4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 4.0);

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 0.0));
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
    EXPECT_EQ(ledger.active_count(), 0u);
}

TEST(OrderLedger, AmendBelowAlreadyFilledQuantityIsRejectedWithoutDataLoss)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));
    ASSERT_TRUE(ledger.on_fill(
        make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0, 1)));

    ledger.amend(1, /*new_price=*/101.0, /*new_qty=*/3.0);

    const auto* entry = ledger.find(1);
    ASSERT_NE(entry, nullptr);
    EXPECT_DOUBLE_EQ(entry->original_qty, 10.0);
    EXPECT_DOUBLE_EQ(entry->filled_qty, 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::partially_filled);
}

TEST(OrderLedger, TerminalLifecycleCannotBeReopenedByLateAcknowledgement)
{
    OrderTracker ledger;
    const auto order = make_order(1, "BTCUSDT", order_side::buy, 5.0);
    ledger.register_order(order);
    ledger.set_status(1, order_status::pending);

    ASSERT_TRUE(ledger.on_fill(
        make_fill(1, "BTCUSDT", order_side::buy, 5.0, 0.0, 1)));
    ASSERT_EQ(ledger.get_order_status(1), order_status::filled);

    ledger.set_status(1, order_status::open);      // delayed ACK
    ledger.set_status(1, order_status::cancelled); // cancel ACK crossed fill

    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
}

TEST(OrderLedger, StaleAdmissionTokenCannotRollBackALaterFill)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    const auto early =
        make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0, 1);
    const auto early_token = ledger.validate_fill(early);
    ASSERT_TRUE(early_token.applied());

    const auto complete =
        make_fill(1, "BTCUSDT", order_side::buy, 10.0, 0.0, 2);
    const auto complete_token = ledger.validate_fill(complete);
    ASSERT_TRUE(complete_token.applied());
    ASSERT_TRUE(ledger.commit_fill(complete, complete_token));

    EXPECT_FALSE(ledger.commit_fill(early, early_token));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 10.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
}

TEST(OrderLedger, AdmissionTokenIsBoundToItsOrder)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));
    submit_and_ack(ledger, make_order(2, "ETHUSDT", order_side::buy, 5.0));

    const auto btc = make_fill(1, "BTCUSDT", order_side::buy, 2.0, 3.0, 1);
    const auto eth = make_fill(2, "ETHUSDT", order_side::buy, 2.0, 3.0, 2);
    const auto btc_token = ledger.validate_fill(btc);
    ASSERT_TRUE(btc_token.applied());

    EXPECT_FALSE(ledger.commit_fill(eth, btc_token));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(2), 0.0);
}

TEST(OrderLedger, ReRegistrationKeepsFilledQuantity)
{
    OrderTracker ledger;
    const auto o = make_order(1, "BTCUSDT", order_side::buy, 10.0);
    submit_and_ack(ledger, o);
    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0));

    // A pending stop converting to a market order re-enters with the same id.
    ledger.register_order(o);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 6.0);
}

TEST(OrderLedger, ReRegistrationCannotShrinkBelowCommittedFills)
{
    OrderTracker ledger;
    const auto original = make_order(1, "BTCUSDT", order_side::buy, 10.0);
    submit_and_ack(ledger, original);
    ASSERT_TRUE(ledger.on_fill(
        make_fill(1, "BTCUSDT", order_side::buy, 6.0, 4.0, 1)));

    const auto invalid_refresh =
        make_order(1, "BTCUSDT", order_side::buy, 4.0, 101.0);
    ledger.register_order(invalid_refresh);

    const auto* entry = ledger.find(1);
    ASSERT_NE(entry, nullptr);
    EXPECT_DOUBLE_EQ(entry->original_qty, 10.0);
    EXPECT_DOUBLE_EQ(entry->filled_qty, 6.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 4.0);
}

TEST(OrderLedger, OpenOrderTermsCannotBypassCanonicalAmendViaRegistration)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, 10.0, 100.0));

    EXPECT_FALSE(ledger.register_order(make_order(
        1, "BTCUSDT", order_side::buy, 100.0, 1.0)));
    const auto* entry = ledger.find(1);
    ASSERT_NE(entry, nullptr);
    EXPECT_DOUBLE_EQ(entry->original_qty, 10.0);
    EXPECT_DOUBLE_EQ(entry->limit_price, 100.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 10.0);
}

TEST(OrderLedger, TerminalOrderIdCannotBeReRegisteredForMoreEconomics)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(
        1, "BTCUSDT", order_side::buy, 5.0));
    ASSERT_TRUE(ledger.on_fill(make_fill(
        1, "BTCUSDT", order_side::buy, 5.0, 0.0, 1)));
    ASSERT_EQ(ledger.get_order_status(1), order_status::filled);

    EXPECT_FALSE(ledger.register_order(make_order(
        1, "BTCUSDT", order_side::buy, 10.0)));
    const auto* entry = ledger.find(1);
    ASSERT_NE(entry, nullptr);
    EXPECT_DOUBLE_EQ(entry->original_qty, 5.0);
    EXPECT_DOUBLE_EQ(entry->filled_qty, 5.0);

    EXPECT_FALSE(ledger.on_fill(make_fill(
        1, "BTCUSDT", order_side::buy, 5.0, 0.0, 2)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 5.0);
}

TEST(OrderLedger, LifecycleTransitionCannotCreateUnknownGhostOrder)
{
    OrderTracker ledger;
    ledger.set_status(999, order_status::pending);
    ledger.set_status(999, order_status::cancelled);

    EXPECT_EQ(ledger.orders_seen(), 0u);
    EXPECT_EQ(ledger.get_order_status(999), order_status::unknown);
    EXPECT_EQ(ledger.active_count(), 0u);
}

TEST(OrderLedger, ResetClearsLedgerAndAggregates)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 2.0));
    submit_and_ack(ledger, make_order(2, "ETHUSDT", order_side::sell, 3.0));
    EXPECT_EQ(ledger.orders_seen(), 2u);

    ledger.reset();
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_EQ(ledger.orders_seen(), 0u);
    EXPECT_TRUE(ledger.get_open_orders().empty());
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
    EXPECT_FALSE(ledger.tracks_symbol("ETHUSDT"));
    EXPECT_EQ(ledger.get_order_status(1), order_status::unknown);
}

TEST(OrderLedger, OpenOrderSetMatchesNonTerminalEntriesExactly)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 1.0));
    submit_and_ack(ledger, make_order(2, "BTCUSDT", order_side::buy, 1.0));
    submit_and_ack(ledger, make_order(3, "BTCUSDT", order_side::sell, 1.0));
    ledger.set_status(2, order_status::filled);
    ledger.set_status(3, order_status::expired);
    submit_and_ack(ledger, make_order(4, "ETHUSDT", order_side::buy, 1.0));

    const auto open = ledger.get_open_orders();
    EXPECT_EQ(open.size(), ledger.active_count());
    EXPECT_EQ(open.size(), 2u);
    for (auto id : open)
        EXPECT_TRUE(order_status_is_open(ledger.get_order_status(id)));

    std::size_t counted = 0;
    ledger.for_each_open([&](const order_ledger_entry& e) {
        ++counted;
        EXPECT_TRUE(e.is_open());
        EXPECT_GE(e.remaining_qty(), 0.0);
        EXPECT_LE(e.filled_qty, e.original_qty);
    });
    EXPECT_EQ(counted, 2u);
}
