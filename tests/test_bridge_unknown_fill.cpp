// Pins the unknown-fill handler hook on ExecutionBridge - the seam that
// turns inbound venue-bracket-leg fills (which the bridge can't recognize
// via its by_client_id_ map) into engine-routable fill_events. Drives a
// fake fill transport to feed exec messages without any network or
// HAS_BINANCE-specific code.

#include <gtest/gtest.h>

#include "execution/execution_bridge.h"
#include "execution/order_transport.h"
#include "execution/fill_transport.h"
#include "execution/fill_parser.h"
#include "execution/order_encoder.h"

#ifdef HAS_BINANCE
#include "providers/binance/binance_futures_user_data_parser.h"
#include "providers/binance/binance_user_data_parser.h"
#endif

#ifdef HAS_BITGET
#include "providers/bitget/bitget_futures_user_data_parser.h"
#endif

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

#if defined(HAS_BINANCE) || defined(HAS_BITGET)
struct OperationalReconciler : IReconciler
{
    bool is_operational() const noexcept override { return true; }
    std::string reconcile(const portfolio&, double) override { return {}; }
};

struct OperationalKillSwitch : IKillSwitch
{
    bool is_operational() const noexcept override { return true; }
    bool cancel_all_and_flatten(std::chrono::milliseconds) override
    {
        return true;
    }
};

WriteSafetyReadiness validated_write_safety_readiness()
{
    OperationalReconciler reconciler;
    OperationalKillSwitch kill_switch;
    return validate_startup_safety(
        true, true, true, private_execution_capability::exchange_writes,
        &reconciler, &kill_switch).readiness;
}
#endif

struct FakeOrderTransport : public IOrderTransport
{
    bool open() override { return true; }
    void close() override {}
    IOrderTransport::result submit(std::string_view, std::string_view) override { return {}; }
    IOrderTransport::result cancel(std::string_view, std::string_view) override { return {}; }
};

struct FakeFillTransport : public IFillTransport
{
    message_cb on_msg;
    status_cb  on_st;
    lifecycle  state_ = lifecycle::open;

    bool open() override { return true; }
    void close() override {}
    lifecycle state() const override { return state_; }
    void set_on_message(message_cb cb) override { on_msg = std::move(cb); }
    void set_on_status(status_cb cb) override { on_st = std::move(cb); }

    void inject(std::string_view raw) { if (on_msg) on_msg(raw); }
};

struct FakeEncoder : public IOrderEncoder
{
    encoded_order encode_submit(const order_event&,
                                std::string_view) override { return {}; }
    encoded_order encode_cancel(std::string_view,
                                std::string_view,
                                std::string_view) override { return {}; }
};

// Hand-written parser that emits exactly what the test wants - no JSON.
struct ScriptedParser : public IFillParser
{
    parsed_exec next;
    bool        will_parse = true;

    bool parse(std::string_view, parsed_exec& out) override
    {
        out = next;
        return will_parse;
    }
};

parsed_exec valid_unknown_fill(std::string exchange_order_id,
                               std::string venue_execution_id,
                               std::string symbol = "BTCUSDT")
{
    parsed_exec out;
    out.k = parsed_exec::kind::full_fill;
    out.client_order_id = "unknown-bracket-leg";
    out.exchange_order_id = std::move(exchange_order_id);
    out.symbol = std::move(symbol);
    out.side = order_side::sell;
    out.last_fill_qty = 0.1;
    out.last_fill_price = 100.0;
    out.cumulative_qty = 0.1;
    out.has_cumulative_qty = true;
    out.venue_execution_id = std::move(venue_execution_id);
    out.commission = 0.0;
    out.commission_asset = "USDT";
    out.ts = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1'700'000'000'000));
    return out;
}

#if defined(HAS_BINANCE) || defined(HAS_BITGET)
struct unknown_wire_observation
{
    int handler_calls{0};
    bool meta_polled{false};
    bool fills_polled{false};
    bool fatal_polled{false};
    std::vector<ExecutionBridge::synth_meta> meta;
    std::vector<fill_event> fills;
    std::vector<submit_result> fatal;
};

unknown_wire_observation observe_unknown_wire_payload(
    std::shared_ptr<IFillParser> parser, std::string_view payload)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = std::move(parser);
    ExecutionBridge bridge(std::move(d));

    unknown_wire_observation observed;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec& pe, std::uint64_t fill_id)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++observed.handler_calls;
            fill_event fill(pe.ts, pe.symbol, /*engine_id=*/999,
                            pe.side, pe.last_fill_qty, pe.last_fill_price,
                            pe.commission, /*remaining=*/0.0, fill_id);
            return ExecutionBridge::synth_result{
                std::move(fill), /*opener=*/42, /*strategy=*/"s"};
        });

    fill_tx->inject(payload);
    observed.meta_polled = bridge.poll_synth_meta(observed.meta);
    observed.fills_polled = bridge.poll_fills(observed.fills);
    observed.fatal_polled = bridge.poll_submit_results(observed.fatal);
    return observed;
}

std::optional<venue_lifecycle_event> observe_known_lifecycle_payload(
    std::shared_ptr<IFillParser> parser, std::string_view payload)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = std::move(parser);
    d.write_safety_readiness = validated_write_safety_readiness();
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    if (!bridge.open()) return std::nullopt;

    order_event order(
        std::chrono::system_clock::time_point(
            std::chrono::milliseconds(1'699'999'999'999)),
        "BTCUSDT", order_type::limit, order_side::buy, 0.5, 95.0);
    order.set_order_id(77);
    bridge.submit_order(order);
    fill_tx->inject(payload);

    venue_lifecycle_event event;
    if (!bridge.poll_lifecycle_event(event)) return std::nullopt;
    return event;
}
#endif

}

TEST(ExecutionBridgeUnknownFill, HandlerInvokedAndFillEnqueued)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<ScriptedParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;

    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec& pe, std::uint64_t fill_id)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            EXPECT_EQ(pe.exchange_order_id, "111");
            EXPECT_EQ(pe.symbol, "BTCUSDT");

            fill_event fe(std::chrono::system_clock::now(),
                          pe.symbol,
                          /*engine_id=*/9999,
                          pe.side,
                          pe.last_fill_qty,
                          pe.last_fill_price,
                          /*commission=*/0.0,
                          /*remaining=*/0.0,
                          fill_id);
            return ExecutionBridge::synth_result{
                std::move(fe), /*opener=*/42, /*strategy=*/"mr"};
        });

    parser->next = parsed_exec{};
    parser->next.k                  = parsed_exec::kind::full_fill;
    parser->next.client_order_id    = "unknown-binance-id";  // bridge will miss
    parser->next.exchange_order_id  = "111";
    parser->next.symbol             = "BTCUSDT";
    parser->next.side               = order_side::sell;
    parser->next.last_fill_qty      = 0.5;
    parser->next.last_fill_price    = 95.0;
    parser->next.cumulative_qty     = 0.5;
    parser->next.has_cumulative_qty = true;
    parser->next.venue_execution_id = "venue-fill-111";
    parser->next.commission_asset   = "USDT";
    parser->next.ts = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(123456));

    fill_tx->inject(R"({"e":"executionReport"})");

    EXPECT_EQ(handler_calls, 1);

    std::vector<ExecutionBridge::synth_meta> meta;
    EXPECT_TRUE(bridge.poll_synth_meta(meta));
    ASSERT_EQ(meta.size(), 1u);
    EXPECT_EQ(meta[0].engine_order_id,  9999u);
    EXPECT_EQ(meta[0].opener_order_id,  42u);
    EXPECT_EQ(meta[0].strategy_name,    "mr");

    std::vector<fill_event> fills;
    EXPECT_TRUE(bridge.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(),       9999u);
    EXPECT_EQ(fills[0].get_symbol(),         "BTCUSDT");
    EXPECT_EQ(fills[0].get_side(),           order_side::sell);
    EXPECT_DOUBLE_EQ(fills[0].get_filled_quantity(), 0.5);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(),      95.0);
    EXPECT_EQ(fills[0].get_venue_execution_id(), "venue-fill-111");
    EXPECT_EQ(fills[0].get_commission_currency(), "USDT");
    EXPECT_DOUBLE_EQ(fills[0].get_cumulative_filled_qty(), 0.5);

    // Second poll drains nothing.
    EXPECT_FALSE(bridge.poll_synth_meta(meta));
    EXPECT_FALSE(bridge.poll_fills(fills));
}

TEST(ExecutionBridgeUnknownFill, HandlerReturningNulloptDropsMessage)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<ScriptedParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;

    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec&, std::uint64_t)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            return std::nullopt;  // not one of ours
        });

    parser->next = parsed_exec{};
    parser->next.k                  = parsed_exec::kind::full_fill;
    parser->next.client_order_id    = "unknown";
    parser->next.exchange_order_id  = "999";
    parser->next.symbol             = "X";
    parser->next.side               = order_side::buy;
    parser->next.last_fill_qty      = 1.0;
    parser->next.last_fill_price    = 100.0;
    parser->next.cumulative_qty     = 1.0;
    parser->next.has_cumulative_qty = true;
    parser->next.venue_execution_id = "venue-fill-999";
    parser->next.commission_asset   = "USDT";
    parser->next.ts = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(123456));

    fill_tx->inject("{}");

    EXPECT_EQ(handler_calls, 1);

    std::vector<ExecutionBridge::synth_meta> meta;
    EXPECT_FALSE(bridge.poll_synth_meta(meta));
    std::vector<fill_event> fills;
    EXPECT_FALSE(bridge.poll_fills(fills));
}

TEST(ExecutionBridgeUnknownFill, C09_NativeBracketFillReplayIsExactlyOnce)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<ScriptedParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;
    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec& pe, std::uint64_t fill_id)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            fill_event fill(pe.ts, pe.symbol, /*engine_id=*/91,
                            pe.side, pe.last_fill_qty, pe.last_fill_price,
                            pe.commission, /*remaining=*/0.0, fill_id);
            return ExecutionBridge::synth_result{
                std::move(fill), /*opener=*/17, /*strategy=*/"s"};
        });

    parser->next = parsed_exec{};
    parser->next.k                  = parsed_exec::kind::full_fill;
    parser->next.client_order_id    = "unknown-bracket-leg";
    parser->next.exchange_order_id  = "venue-order-7";
    parser->next.venue_execution_id = "native-leg-fill-7";
    parser->next.symbol             = "BTCUSDT";
    parser->next.side               = order_side::sell;
    parser->next.last_fill_qty      = 0.5;
    parser->next.last_fill_price    = 95.0;
    parser->next.cumulative_qty     = 0.5;
    parser->next.has_cumulative_qty = true;
    parser->next.commission         = 0.01;
    parser->next.commission_asset   = "USDT";
    parser->next.ts = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(123456));

    fill_tx->inject("{}");
    fill_tx->inject("{}");

    EXPECT_EQ(handler_calls, 1)
        << "the native venue execution id is the economic identity";
    std::vector<ExecutionBridge::synth_meta> meta;
    ASSERT_TRUE(bridge.poll_synth_meta(meta));
    EXPECT_EQ(meta.size(), 1u);
    std::vector<fill_event> fills;
    ASSERT_TRUE(bridge.poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
    if (!fills.empty())
    {
        EXPECT_EQ(fills.front().get_venue_execution_id(),
                  "native-leg-fill-7");
    }
}

TEST(ExecutionBridgeUnknownFill,
     C05_UnknownLifecycleFailsClosedWithoutEconomicFill)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<ScriptedParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;

    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec&, std::uint64_t)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            return std::nullopt;
        });

    parser->next = parsed_exec{};
    parser->next.k = parsed_exec::kind::ack;
    parser->next.client_order_id = "unknown-bracket-leg";
    parser->next.exchange_order_id = "BRACKET-111";
    parser->next.symbol = "BTCUSDT";
    parser->next.side = order_side::sell;
    parser->next.ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1}};

    fill_tx->inject("{}");

    EXPECT_EQ(handler_calls, 0)
        << "ACK/cancel/reject/expire/other are lifecycle reports, not fills";
    std::vector<ExecutionBridge::synth_meta> meta;
    std::vector<fill_event> fills;
    std::vector<submit_result> fatal;
    EXPECT_FALSE(bridge.poll_synth_meta(meta));
    EXPECT_FALSE(bridge.poll_fills(fills));
    ASSERT_TRUE(bridge.poll_submit_results(fatal));
    ASSERT_EQ(fatal.size(), 1U);
    EXPECT_TRUE(fatal.front().fatal);
    EXPECT_NE(fatal.front().error.find("unknown"), std::string::npos);
}

#ifdef HAS_BINANCE
TEST(ExecutionBridgeUnknownFill,
     C05_RealBinanceSpotAckReachesAuthoritativeLifecycleHandoff)
{
    const auto event = observe_known_lifecycle_payload(
        std::make_shared<BinanceUserDataParser>(),
        R"({"e":"executionReport","E":1700000000000,"s":"BTCUSDT","c":"tt-77","S":"BUY","x":"NEW","X":"NEW","i":42,"t":0,"l":"0","L":"0","z":"0","n":"0","N":"USDT"})");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->engine_order_id, 77U);
    EXPECT_EQ(event->transition, venue_order_transition::acknowledged);
}

TEST(ExecutionBridgeUnknownFill,
     C05_RealBinanceFuturesCancelReachesAuthoritativeLifecycleHandoff)
{
    const auto event = observe_known_lifecycle_payload(
        std::make_shared<BinanceFuturesUserDataParser>(),
        R"({"e":"ORDER_TRADE_UPDATE","E":1700000000000,"o":{"s":"BTCUSDT","c":"tt-77","S":"BUY","x":"CANCELED","X":"CANCELED","i":42,"t":0,"l":"0","L":"0","z":"0","n":"0","N":"USDT","T":1700000000001}})");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->engine_order_id, 77U);
    EXPECT_EQ(event->transition, venue_order_transition::canceled);
}

TEST(ExecutionBridgeUnknownFill,
     C12_RealBinanceLifecycleCannotSynthesizeUnknownBracketFill)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<BinanceUserDataParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;
    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec&, std::uint64_t)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            return std::nullopt;
        });

    const auto report = [](std::string_view execution_type,
                           std::string_view order_status) {
        return std::string{"{\"e\":\"executionReport\",\"E\":1700000000000,"}
            + "\"s\":\"BTCUSDT\",\"c\":\"unknown-bracket-leg\","
              "\"S\":\"SELL\",\"x\":\""
            + std::string{execution_type} + "\",\"X\":\""
            + std::string{order_status}
            + "\",\"i\":42,\"t\":0,\"l\":\"0\",\"L\":\"0\","
              "\"z\":\"0\",\"n\":\"0\",\"N\":\"USDT\"}";
    };

    const auto inject_lifecycle = [&](std::string payload,
                                      parsed_exec::kind expected) {
        parsed_exec parsed;
        ASSERT_TRUE(parser->parse(payload, parsed));
        ASSERT_EQ(parsed.k, expected)
            << "the integration guard must exercise a recognized lifecycle "
               "event, not pass vacuously because the parser rejected it";
        fill_tx->inject(payload);
    };
    inject_lifecycle(report("NEW", "NEW"), parsed_exec::kind::ack);
    inject_lifecycle(report("CANCELED", "CANCELED"),
                     parsed_exec::kind::canceled);
    inject_lifecycle(report("REJECTED", "REJECTED"),
                     parsed_exec::kind::rejected);
    inject_lifecycle(report("EXPIRED", "EXPIRED"),
                     parsed_exec::kind::expired);

    EXPECT_EQ(handler_calls, 0);
    std::vector<ExecutionBridge::synth_meta> meta;
    std::vector<fill_event> fills;
    std::vector<submit_result> fatal;
    EXPECT_FALSE(bridge.poll_synth_meta(meta));
    EXPECT_FALSE(bridge.poll_fills(fills));
    EXPECT_FALSE(bridge.poll_submit_results(fatal))
        << "recognized ACK/cancel/reject/expire reports are valid lifecycle "
           "events and must not be escalated as malformed";
}

TEST(ExecutionBridgeUnknownFill,
     C12_ContradictoryBinanceTradeCancelTupleFailsClosed)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<BinanceUserDataParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;
    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec& pe, std::uint64_t fill_id)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            fill_event fill(pe.ts, pe.symbol, /*engine_id=*/999,
                            pe.side, pe.last_fill_qty, pe.last_fill_price,
                            pe.commission, /*remaining=*/0.0, fill_id);
            return ExecutionBridge::synth_result{
                std::move(fill), /*opener=*/42, /*strategy=*/"s"};
        });

    fill_tx->inject(
        R"({"e":"executionReport","E":1700000000000,)"
        R"("s":"BTCUSDT","c":"unknown-bracket-leg","S":"SELL",)"
        R"("x":"TRADE","X":"CANCELED","i":42,"t":9001,)"
        R"("l":"0.5","L":"95","z":"0.5","n":"0.01","N":"USDT"})");

    EXPECT_EQ(handler_calls, 0)
        << "a contradictory terminal lifecycle tuple is malformed, not a fill";
    std::vector<ExecutionBridge::synth_meta> meta;
    std::vector<fill_event> fills;
    EXPECT_FALSE(bridge.poll_synth_meta(meta));
    EXPECT_FALSE(bridge.poll_fills(fills));
    std::vector<submit_result> fatal;
    ASSERT_TRUE(bridge.poll_submit_results(fatal));
    ASSERT_EQ(fatal.size(), 1U);
    EXPECT_TRUE(fatal.front().fatal);
}

TEST(ExecutionBridgeUnknownFill,
     C12_ValidBinanceFuturesFillReachesUnknownBracketHandlerOnce)
{
    const auto observed = observe_unknown_wire_payload(
        std::make_shared<BinanceFuturesUserDataParser>(),
        R"({"e":"ORDER_TRADE_UPDATE","E":1700000000000,"o":{)"
        R"("s":"BTCUSDT","c":"unknown-bracket-leg","S":"SELL",)"
        R"("x":"TRADE","X":"FILLED","i":42,"t":9001,)"
        R"("l":"0.5","L":"95","z":"0.5","n":"0.01",)"
        R"("N":"USDT","T":1700000000001}})"
    );

    EXPECT_EQ(observed.handler_calls, 1);
    EXPECT_TRUE(observed.meta_polled);
    ASSERT_EQ(observed.meta.size(), 1U);
    EXPECT_TRUE(observed.fills_polled);
    ASSERT_EQ(observed.fills.size(), 1U);
    EXPECT_EQ(observed.fills.front().get_venue_execution_id(), "9001");
    EXPECT_FALSE(observed.fatal_polled);
    EXPECT_TRUE(observed.fatal.empty());
}

TEST(ExecutionBridgeUnknownFill,
     C12_ContradictoryBinanceFuturesTradeCancelTupleFailsClosed)
{
    const auto observed = observe_unknown_wire_payload(
        std::make_shared<BinanceFuturesUserDataParser>(),
        R"({"e":"ORDER_TRADE_UPDATE","E":1700000000000,"o":{)"
        R"("s":"BTCUSDT","c":"unknown-bracket-leg","S":"SELL",)"
        R"("x":"TRADE","X":"CANCELED","i":42,"t":9001,)"
        R"("l":"0.5","L":"95","z":"0.5","n":"0.01",)"
        R"("N":"USDT","T":1700000000001}})"
    );

    EXPECT_EQ(observed.handler_calls, 0);
    EXPECT_FALSE(observed.meta_polled);
    EXPECT_TRUE(observed.meta.empty());
    EXPECT_FALSE(observed.fills_polled);
    EXPECT_TRUE(observed.fills.empty());
    ASSERT_TRUE(observed.fatal_polled);
    ASSERT_EQ(observed.fatal.size(), 1U);
    EXPECT_TRUE(observed.fatal.front().fatal);
}
#endif

#ifdef HAS_BITGET
TEST(ExecutionBridgeUnknownFill,
     C05_RealBitgetCancelReachesAuthoritativeLifecycleHandoff)
{
    const auto event = observe_known_lifecycle_payload(
        std::make_shared<BitgetFuturesUserDataParser>(),
        R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":[{"symbol":"BTCUSDT","orderId":"42","clientOid":"tt-77","side":"buy","orderStatus":"cancelled","cumExecQty":"0","updatedTime":"1700000000002"}],"ts":1700000000003})");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->engine_order_id, 77U);
    EXPECT_EQ(event->transition, venue_order_transition::canceled);
}

TEST(ExecutionBridgeUnknownFill,
     C12_ValidBitgetFillReachesUnknownBracketHandlerOnce)
{
    const auto observed = observe_unknown_wire_payload(
        std::make_shared<BitgetFuturesUserDataParser>(),
        R"({"action":"snapshot","arg":{"instType":"UTA","topic":"fill"},"data":[{)"
        R"("symbol":"BTCUSDT","orderId":"42","clientOid":"unknown-bracket-leg",)"
        R"("side":"sell","execQty":"0.5","execPrice":"95",)"
        R"("execId":"exec-9001","cumExecQty":"0.5",)"
        R"("execTime":"1700000000002",)"
        R"("feeDetail":[{"feeCoin":"USDT","fee":"0.01"}])"
        R"(}],"ts":1700000000003})"
    );

    EXPECT_EQ(observed.handler_calls, 1);
    EXPECT_TRUE(observed.meta_polled);
    ASSERT_EQ(observed.meta.size(), 1U);
    EXPECT_TRUE(observed.fills_polled);
    ASSERT_EQ(observed.fills.size(), 1U);
    EXPECT_EQ(observed.fills.front().get_venue_execution_id(), "exec-9001");
    EXPECT_FALSE(observed.fatal_polled);
    EXPECT_TRUE(observed.fatal.empty());
}

TEST(ExecutionBridgeUnknownFill,
     C12_TopiclessBitgetCanceledOrderWithExecQtyFailsClosed)
{
    const auto observed = observe_unknown_wire_payload(
        std::make_shared<BitgetFuturesUserDataParser>(),
        R"({"action":"snapshot","arg":{"instType":"UTA"},"data":[{)"
        R"("symbol":"BTCUSDT","orderId":"42","clientOid":"unknown-bracket-leg",)"
        R"("side":"sell","orderStatus":"cancelled",)"
        R"("execQty":"0.5","execPrice":"95","execId":"exec-9001",)"
        R"("cumExecQty":"0.5","execTime":"1700000000002",)"
        R"("feeDetail":[{"feeCoin":"USDT","fee":"0.01"}],)"
        R"("updatedTime":"1700000000002"}],"ts":1700000000003})"
    );

    EXPECT_EQ(observed.handler_calls, 0);
    EXPECT_FALSE(observed.meta_polled);
    EXPECT_TRUE(observed.meta.empty());
    EXPECT_FALSE(observed.fills_polled);
    EXPECT_TRUE(observed.fills.empty());
    ASSERT_TRUE(observed.fatal_polled);
    ASSERT_EQ(observed.fatal.size(), 1U);
    EXPECT_TRUE(observed.fatal.front().fatal);
}
#endif

TEST(ExecutionBridgeUnknownFill, NoExchangeIdSkipsHandler)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<ScriptedParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;

    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec&, std::uint64_t)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            return std::nullopt;
        });

    parser->next = valid_unknown_fill(
        /*exchange_order_id=*/{}, "venue-no-exchange-id", "X");

    fill_tx->inject("{}");

    EXPECT_EQ(handler_calls, 0);  // bridge short-circuits without exchange id
    std::vector<submit_result> fatal;
    EXPECT_FALSE(bridge.poll_submit_results(fatal))
        << "the otherwise valid fill must isolate the missing exchange-order "
           "identity branch";
}

TEST(ExecutionBridgeUnknownFill, ClearHandlerPreventsLateInvocation)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<ScriptedParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;

    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec&, std::uint64_t)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            return std::nullopt;
        });

    // Explicit clear (as engine will do on stop paths).
    bridge.clear_unknown_fill_handler();

    parser->next = valid_unknown_fill("LATE-001", "venue-late-001");

    fill_tx->inject(R"({"e":"executionReport"})");

    EXPECT_EQ(handler_calls, 0);  // must not fire after clear
    std::vector<submit_result> fatal;
    EXPECT_FALSE(bridge.poll_submit_results(fatal))
        << "the valid late fill must be ignored because the handler was "
           "explicitly cleared, not rejected as malformed";
}

TEST(ExecutionBridgeUnknownFill, CloseClearsHandler)
{
    auto fill_tx  = std::make_shared<FakeFillTransport>();
    auto order_tx = std::make_shared<FakeOrderTransport>();
    auto encoder  = std::make_shared<FakeEncoder>();
    auto parser   = std::make_shared<ScriptedParser>();

    ExecutionBridge::deps d;
    d.order_tx = order_tx;
    d.fill_tx  = fill_tx;
    d.encoder  = encoder;
    d.parser   = parser;

    ExecutionBridge bridge(std::move(d));

    int handler_calls = 0;
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec&, std::uint64_t)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++handler_calls;
            return std::nullopt;
        });

    bridge.close();  // should internally clear

    parser->next = valid_unknown_fill(
        "AFTER-CLOSE", "venue-after-close", "ETHUSDT");

    fill_tx->inject("{}");

    EXPECT_EQ(handler_calls, 0);
    std::vector<submit_result> fatal;
    EXPECT_FALSE(bridge.poll_submit_results(fatal))
        << "the valid post-close fill must be ignored because close cleared "
           "the handler, not rejected as malformed";
}
