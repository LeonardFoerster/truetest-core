// Pins the wire format and parsing of BinanceFuturesBracketAdapter.
// Fake-callable pattern: no real REST call ever happens.

#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_futures_bracket_adapter.h"
#include "exits/exit_intent.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using response = BinanceFuturesBracketAdapter::response;

// Records each call and lets the test program canned responses per call.
// If `responses` is empty, returns `default_resp` for every call.
struct fake_caller
{
    response default_resp{200, ""};
    std::vector<response> responses;
    std::vector<std::pair<std::string, std::string>> log;

    response operator()(std::string_view ep, std::string_view p)
    {
        log.emplace_back(std::string(ep), std::string(p));
        if (log.size() <= responses.size())
            return responses[log.size() - 1];
        return default_resp;
    }
};

std::function<response(std::string_view, std::string_view)>
wrap(std::shared_ptr<fake_caller> f)
{
    return [f](std::string_view ep, std::string_view p) {
        return (*f)(ep, p);
    };
}

truetest::exits::exit_intent make_long_intent(std::uint64_t opener,
                                              double sl, double tp,
                                              double qty = 0.5,
                                              double qty_fraction = 1.0)
{
    truetest::exits::exit_intent ei;
    ei.symbol           = "btcusdt";
    ei.close_side       = order_side::sell;
    ei.qty              = qty;
    ei.qty_fraction     = qty_fraction;
    ei.stop_loss        = sl;
    ei.take_profit      = tp;
    ei.opener_order_id  = opener;
    ei.strategy_name    = "s";
    return ei;
}

} // namespace

TEST(BinanceFuturesBracketAdapter, AdvertisesStopMarketNotOco)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));

    auto c = a.capabilities();
    EXPECT_TRUE(c.stop_market);
    EXPECT_FALSE(c.oco);          // placement is two separate POSTs
    EXPECT_FALSE(c.stop_limit);
    EXPECT_FALSE(c.trailing_stop);
}

TEST(BinanceFuturesBracketAdapter, PlacePostsTwoLegsWithClosePosition)
{
    auto post = std::make_shared<fake_caller>();
    post->responses = {
        {200, R"({"orderId":111,"clientOrderId":"tt-fb-sl-7"})"},
        {200, R"({"orderId":222,"clientOrderId":"tt-fb-tp-7"})"},
    };
    auto del = std::make_shared<fake_caller>();

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    auto h = a.place(7, make_long_intent(7, 100.0, 110.0), /*fill=*/105.0);

    ASSERT_EQ(post->log.size(), 2u);

    // Both legs hit /fapi/v1/order.
    EXPECT_EQ(post->log[0].first, "/fapi/v1/order");
    EXPECT_EQ(post->log[1].first, "/fapi/v1/order");

    // SL leg: STOP_MARKET, stopPrice=100, closePosition+reduceOnly.
    const auto& sl_p = post->log[0].second;
    EXPECT_NE(sl_p.find("symbol=BTCUSDT"),     std::string::npos);
    EXPECT_NE(sl_p.find("side=SELL"),          std::string::npos);
    EXPECT_NE(sl_p.find("type=STOP_MARKET"),   std::string::npos);
    EXPECT_NE(sl_p.find("stopPrice=100"),      std::string::npos);
    EXPECT_NE(sl_p.find("closePosition=true"), std::string::npos);
    EXPECT_NE(sl_p.find("reduceOnly=true"),    std::string::npos);
    EXPECT_EQ(sl_p.find("quantity="),          std::string::npos);
    EXPECT_NE(sl_p.find("newClientOrderId=tt-fb-sl-7"), std::string::npos);

    // TP leg: TAKE_PROFIT_MARKET at stopPrice=110.
    const auto& tp_p = post->log[1].second;
    EXPECT_NE(tp_p.find("type=TAKE_PROFIT_MARKET"), std::string::npos);
    EXPECT_NE(tp_p.find("stopPrice=110"),           std::string::npos);
    EXPECT_NE(tp_p.find("newClientOrderId=tt-fb-tp-7"), std::string::npos);

    EXPECT_EQ(h.sl_exchange_id.value_or(""), "111");
    EXPECT_EQ(h.tp_exchange_id.value_or(""), "222");
    EXPECT_FALSE(h.oco_list_id.has_value());  // no list on futures
}

TEST(BinanceFuturesBracketAdapter, PlaceShortIntentSetsBuySide)
{
    auto post = std::make_shared<fake_caller>();
    post->default_resp = {200, R"({"orderId":1})"};
    auto del = std::make_shared<fake_caller>();

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    auto ei = make_long_intent(8, 100.0, 90.0);
    ei.close_side = order_side::buy;  // closing a short
    auto h = a.place(8, ei, 95.0);

    ASSERT_EQ(post->log.size(), 2u);
    EXPECT_NE(post->log[0].second.find("side=BUY"), std::string::npos);
    EXPECT_NE(post->log[1].second.find("side=BUY"), std::string::npos);
    EXPECT_TRUE(h.sl_exchange_id.has_value());
}

TEST(BinanceFuturesBracketAdapter, PlaceDeclinesOnPartialFraction)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));

    auto ei = make_long_intent(9, 100.0, 110.0, /*qty=*/0.5,
                                /*qty_fraction=*/0.5);
    auto h = a.place(9, ei, 105.0);

    EXPECT_TRUE(h.empty());
    EXPECT_EQ(post->log.size(), 0u);  // never POSTed anything
}

TEST(BinanceFuturesBracketAdapter, PlaceDeclinesOnMissingLeg)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));

    auto ei = make_long_intent(10, 100.0, 110.0);
    ei.take_profit.reset();
    auto h = a.place(10, ei, 105.0);

    EXPECT_TRUE(h.empty());
    EXPECT_EQ(post->log.size(), 0u);
}

TEST(BinanceFuturesBracketAdapter, PlaceFirstLegFailureReturnsEmpty)
{
    auto post = std::make_shared<fake_caller>();
    post->responses = {
        {400, R"({"code":-2010,"msg":"trigger price too close"})"},
    };
    auto del = std::make_shared<fake_caller>();

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    auto h = a.place(11, make_long_intent(11, 100.0, 110.0), 105.0);

    EXPECT_TRUE(h.empty());
    EXPECT_EQ(post->log.size(), 1u);  // didn't attempt TP after SL failed
}

TEST(BinanceFuturesBracketAdapter, PlaceSecondLegFailureLeavesSlOnly)
{
    auto post = std::make_shared<fake_caller>();
    post->responses = {
        {200, R"({"orderId":111})"},
        {400, R"({"code":-2010})"},
    };
    auto del = std::make_shared<fake_caller>();

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    auto h = a.place(12, make_long_intent(12, 100.0, 110.0), 105.0);

    EXPECT_TRUE(h.sl_exchange_id.has_value());
    EXPECT_FALSE(h.tp_exchange_id.has_value());
    EXPECT_EQ(post->log.size(), 2u);
}

TEST(BinanceFuturesBracketAdapter, CancelHitsBothLegEndpointsWithSymbol)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    del->default_resp = {200, "{}"};

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del),
                                    /*get=*/nullptr, "btcusdt");

    truetest::exits::bracket_handles h;
    h.sl_exchange_id = "111";
    h.tp_exchange_id = "222";
    a.cancel(7, h);

    ASSERT_EQ(del->log.size(), 2u);
    EXPECT_EQ(del->log[0].first, "/fapi/v1/order");
    EXPECT_EQ(del->log[1].first, "/fapi/v1/order");
    // /fapi/v1/order DELETE requires symbol; adapter must include it
    // (and uppercase it, matching the rest of the wire format).
    EXPECT_NE(del->log[0].second.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_NE(del->log[1].second.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_NE(del->log[0].second.find("orderId=111"),    std::string::npos);
    EXPECT_NE(del->log[1].second.find("orderId=222"),    std::string::npos);
}

TEST(BinanceFuturesBracketAdapter, CancelEmptyHandlesIsNoop)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));

    truetest::exits::bracket_handles empty;
    a.cancel(1, empty);

    EXPECT_EQ(del->log.size(), 0u);
}

TEST(BinanceFuturesBracketAdapter, CancelTolerates2011And2013)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    // -2011: unknown order. -2013: order does not exist. Both arrive
    // when closePosition=true auto-cancelled the leg already.
    del->responses = {
        {400, R"({"code":-2011,"msg":"Unknown order"})"},
        {400, R"({"code":-2013,"msg":"Order does not exist"})"},
    };

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    truetest::exits::bracket_handles h;
    h.sl_exchange_id = "111";
    h.tp_exchange_id = "222";
    // No exception, no throw — adapter swallows the two well-known
    // already-cancelled codes.
    a.cancel(1, h);
    EXPECT_EQ(del->log.size(), 2u);
}

TEST(BinanceFuturesBracketAdapter, ListOpenRecoversByPrefix)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();

    // Two pairs of brackets: opener=10 (SL+TP) and opener=11 (SL only).
    // One unrelated order from another tool — must be ignored.
    get->responses = {{200, R"([
        {"clientOrderId":"tt-fb-sl-10","symbol":"BTCUSDT","side":"SELL",
         "stopPrice":"95.5","origQty":"0","orderId":1001},
        {"clientOrderId":"tt-fb-tp-10","symbol":"BTCUSDT","side":"SELL",
         "stopPrice":"110.0","origQty":"0","orderId":1002},
        {"clientOrderId":"tt-fb-sl-11","symbol":"ETHUSDT","side":"BUY",
         "stopPrice":"3500.0","origQty":"0","orderId":2001},
        {"clientOrderId":"some-other-tool","symbol":"BTCUSDT","side":"BUY",
         "stopPrice":"50000","origQty":"0.1","orderId":9999}
    ])"}};

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del), wrap(get));
    auto recovered = a.list_open();

    ASSERT_EQ(recovered.size(), 2u);

    // Find the BTCUSDT bracket
    const truetest::exits::IBracketAdapter::recovered_bracket* btc = nullptr;
    const truetest::exits::IBracketAdapter::recovered_bracket* eth = nullptr;
    for (const auto& b : recovered)
    {
        if (b.symbol == "BTCUSDT") btc = &b;
        if (b.symbol == "ETHUSDT") eth = &b;
    }
    ASSERT_NE(btc, nullptr);
    ASSERT_NE(eth, nullptr);

    EXPECT_EQ(btc->opener_order_id, 10u);
    ASSERT_TRUE(btc->stop_loss.has_value());
    ASSERT_TRUE(btc->take_profit.has_value());
    EXPECT_DOUBLE_EQ(*btc->stop_loss,   95.5);
    EXPECT_DOUBLE_EQ(*btc->take_profit, 110.0);
    EXPECT_EQ(btc->handles.sl_exchange_id.value_or(""), "1001");
    EXPECT_EQ(btc->handles.tp_exchange_id.value_or(""), "1002");
    EXPECT_EQ(btc->close_side, order_side::sell);

    EXPECT_EQ(eth->opener_order_id, 11u);
    ASSERT_TRUE(eth->stop_loss.has_value());
    EXPECT_FALSE(eth->take_profit.has_value());
    EXPECT_EQ(eth->close_side, order_side::buy);
}

TEST(BinanceFuturesBracketAdapter, ListOpenWithoutGetCallableReturnsEmpty)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    EXPECT_TRUE(a.list_open().empty());
}

#endif // HAS_BINANCE
