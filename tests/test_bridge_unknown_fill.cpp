// Pins the unknown-fill handler hook on ExecutionBridge — the seam that
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

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

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

// Hand-written parser that emits exactly what the test wants — no JSON.
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

    fill_tx->inject("{}");

    EXPECT_EQ(handler_calls, 1);

    std::vector<ExecutionBridge::synth_meta> meta;
    EXPECT_FALSE(bridge.poll_synth_meta(meta));
    std::vector<fill_event> fills;
    EXPECT_FALSE(bridge.poll_fills(fills));
}

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

    parser->next = parsed_exec{};
    parser->next.k                = parsed_exec::kind::full_fill;
    parser->next.client_order_id  = "unknown";
    // exchange_order_id intentionally empty
    parser->next.symbol           = "X";

    fill_tx->inject("{}");

    EXPECT_EQ(handler_calls, 0);  // bridge short-circuits without exchange id
}
