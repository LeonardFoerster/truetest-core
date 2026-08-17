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

std::string algo_ack(std::uint64_t id, std::string_view client,
                     std::string_view type, std::string_view side,
                     std::string_view trigger)
{
    return "{\"algoId\":" + std::to_string(id)
        + ",\"clientAlgoId\":\"" + std::string(client)
        + "\",\"algoType\":\"CONDITIONAL\",\"orderType\":\""
        + std::string(type) + "\",\"symbol\":\"BTCUSDT\",\"side\":\""
        + std::string(side)
        + "\",\"positionSide\":\"BOTH\",\"algoStatus\":\"NEW\""
          ",\"triggerPrice\":\"" + std::string(trigger)
        + "\",\"closePosition\":true,\"reduceOnly\":false}";
}

std::string cancel_ack(std::uint64_t id, std::string_view client)
{
    return "{\"algoId\":" + std::to_string(id)
        + ",\"clientAlgoId\":\"" + std::string(client)
        + "\",\"code\":\"200\",\"msg\":\"success\"}";
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

}

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
        {200, algo_ack(111, "tt-fb-sl-7", "STOP_MARKET", "SELL", "100")},
        {200, algo_ack(222, "tt-fb-tp-7", "TAKE_PROFIT_MARKET", "SELL", "110")},
    };
    auto del = std::make_shared<fake_caller>();

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    auto h = a.place(7, make_long_intent(7, 100.0, 110.0), /*fill=*/105.0);

    ASSERT_EQ(post->log.size(), 2u);

    EXPECT_EQ(post->log[0].first, "/fapi/v1/algoOrder");
    EXPECT_EQ(post->log[1].first, "/fapi/v1/algoOrder");

    // SL leg: STOP_MARKET, triggerPrice=100, closePosition only. Binance
    // forbids reduceOnly together with closePosition.
    const auto& sl_p = post->log[0].second;
    EXPECT_NE(sl_p.find("symbol=BTCUSDT"),     std::string::npos);
    EXPECT_NE(sl_p.find("side=SELL"),          std::string::npos);
    EXPECT_NE(sl_p.find("type=STOP_MARKET"),   std::string::npos);
    EXPECT_NE(sl_p.find("algoType=CONDITIONAL"), std::string::npos);
    EXPECT_NE(sl_p.find("triggerPrice=100"),   std::string::npos);
    EXPECT_NE(sl_p.find("closePosition=true"), std::string::npos);
    EXPECT_EQ(sl_p.find("reduceOnly"),         std::string::npos);
    EXPECT_EQ(sl_p.find("quantity="),          std::string::npos);
    EXPECT_NE(sl_p.find("clientAlgoId=tt-fb-sl-7"), std::string::npos);

    // TP leg: TAKE_PROFIT_MARKET at triggerPrice=110.
    const auto& tp_p = post->log[1].second;
    EXPECT_NE(tp_p.find("type=TAKE_PROFIT_MARKET"), std::string::npos);
    EXPECT_NE(tp_p.find("triggerPrice=110"),        std::string::npos);
    EXPECT_NE(tp_p.find("clientAlgoId=tt-fb-tp-7"), std::string::npos);

    EXPECT_EQ(h.sl_exchange_id.value_or(""), "111");
    EXPECT_EQ(h.tp_exchange_id.value_or(""), "222");
    EXPECT_FALSE(h.oco_list_id.has_value());  // no list on futures
}

TEST(BinanceFuturesBracketAdapter, PlaceShortIntentSetsBuySide)
{
    auto post = std::make_shared<fake_caller>();
    post->responses = {
        {200, algo_ack(1, "tt-fb-sl-8", "STOP_MARKET", "BUY", "100")},
        {200, algo_ack(2, "tt-fb-tp-8", "TAKE_PROFIT_MARKET", "BUY", "90")},
    };
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
        {200, algo_ack(111, "tt-fb-sl-12", "STOP_MARKET", "SELL", "100")},
        {400, R"({"code":-2010})"},
    };
    auto del = std::make_shared<fake_caller>();

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    auto h = a.place(12, make_long_intent(12, 100.0, 110.0), 105.0);

    EXPECT_TRUE(h.sl_exchange_id.has_value());
    EXPECT_FALSE(h.tp_exchange_id.has_value());
    EXPECT_EQ(post->log.size(), 2u);
}

TEST(BinanceFuturesBracketAdapter, PlaceMalformedSuccessThrowsAndScrubsKnownLeg)
{
    auto post = std::make_shared<fake_caller>();
    auto del = std::make_shared<fake_caller>();
    del->default_resp = {200, cancel_ack(111, "tt-fb-sl-13")};

    post->responses = {{200, R"({"nested":{"algoId":111,"clientAlgoId":"tt-fb-sl-13"}})"}};
    BinanceFuturesBracketAdapter first_bad(wrap(post), wrap(del));
    EXPECT_THROW(
        first_bad.place(13, make_long_intent(13, 100.0, 110.0), 105.0),
        std::runtime_error);
    EXPECT_TRUE(del->log.empty());

    post->log.clear();
    del->log.clear();
    post->responses = {
        {200, algo_ack(111, "tt-fb-sl-13", "STOP_MARKET", "SELL", "100")},
        {200, R"({"algoId":222,"algoId":333,"clientAlgoId":"tt-fb-tp-13"})"},
    };
    BinanceFuturesBracketAdapter second_bad(wrap(post), wrap(del));
    EXPECT_THROW(
        second_bad.place(13, make_long_intent(13, 100.0, 110.0), 105.0),
        std::runtime_error);
    ASSERT_EQ(del->log.size(), 1u);
    EXPECT_NE(del->log.front().second.find("algoId=111"), std::string::npos);
}

TEST(BinanceFuturesBracketAdapter, CancelHitsBothLegEndpointsWithSymbol)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    del->responses = {
        {200, cancel_ack(111, "tt-fb-sl-7")},
        {200, cancel_ack(222, "tt-fb-tp-7")},
    };

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));

    // Symbol now travels with the handles, populated by place()
    // / list_open() at runtime. Tests construct it directly here.
    truetest::exits::bracket_handles h;
    h.sl_exchange_id = "111";
    h.tp_exchange_id = "222";
    h.symbol         = "BTCUSDT";
    a.cancel(7, h);

    ASSERT_EQ(del->log.size(), 2u);
    EXPECT_EQ(del->log[0].first, "/fapi/v1/algoOrder");
    EXPECT_EQ(del->log[1].first, "/fapi/v1/algoOrder");
    EXPECT_NE(del->log[0].second.find("algoId=111"), std::string::npos);
    EXPECT_NE(del->log[1].second.find("algoId=222"), std::string::npos);
}

TEST(BinanceFuturesBracketAdapter, CancelMalformedSuccessThrows)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    del->responses = {
        {200, R"({"nested":{"algoId":111,"clientAlgoId":"tt-fb-sl-7","code":"200","msg":"success"}})"},
    };
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    truetest::exits::bracket_handles h;
    h.sl_exchange_id = "111";
    h.symbol = "BTCUSDT";
    EXPECT_THROW(a.cancel(7, h), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, CancelExceptionStillAttemptsOtherLeg)
{
    auto post = std::make_shared<fake_caller>();
    int calls = 0;
    BinanceFuturesBracketAdapter a(
        wrap(post),
        [&](std::string_view, std::string_view params) -> response {
            ++calls;
            if (calls == 1) throw std::runtime_error("ambiguous SL cancel");
            EXPECT_NE(params.find("algoId=222"), std::string_view::npos);
            return {200, cancel_ack(222, "tt-fb-tp-7")};
        });
    truetest::exits::bracket_handles h;
    h.sl_exchange_id = "111";
    h.tp_exchange_id = "222";
    h.symbol = "BTCUSDT";

    EXPECT_THROW(a.cancel(7, h), std::runtime_error);
    EXPECT_EQ(calls, 2);
}

TEST(BinanceFuturesBracketAdapter, PlacePopulatesHandlesSymbol)
{
    auto post = std::make_shared<fake_caller>();
    post->responses = {
        {200, algo_ack(111, "tt-fb-sl-7", "STOP_MARKET", "SELL", "100")},
        {200, algo_ack(222, "tt-fb-tp-7", "TAKE_PROFIT_MARKET", "SELL", "110")},
    };
    auto del = std::make_shared<fake_caller>();

    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    auto h = a.place(7, make_long_intent(7, 100.0, 110.0), 105.0);

    // Symbol is uppercased (matches the wire format) and travels with
    // the handles for downstream cancel().
    EXPECT_EQ(h.symbol, "BTCUSDT");
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
    // No exception, no throw - adapter swallows the two well-known
    // already-cancelled codes.
    a.cancel(1, h);
    EXPECT_EQ(del->log.size(), 2u);
}

TEST(BinanceFuturesBracketAdapter, ListOpenRecoversByPrefix)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();

    // One configured-symbol pair plus an unrelated client id.
    get->responses = {{200, R"([
        {"algoId":1001,"clientAlgoId":"tt-fb-sl-10","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"95.5","closePosition":true,"reduceOnly":false},
        {"algoId":1002,"clientAlgoId":"tt-fb-tp-10","algoType":"CONDITIONAL","orderType":"TAKE_PROFIT_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"110.0","closePosition":true,"reduceOnly":false},
        {"algoId":9999,"clientAlgoId":"some-other-tool","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"BUY","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"50000","closePosition":true,"reduceOnly":false}
    ])"}};

    BinanceFuturesBracketAdapter a(
        wrap(post), wrap(del), wrap(get), "BTCUSDT");
    auto recovered = a.list_open();
    ASSERT_EQ(get->log.front().first, "/fapi/v1/openAlgoOrders");
    EXPECT_NE(get->log.front().second.find("algoType=CONDITIONAL"),
              std::string::npos);

    ASSERT_EQ(recovered.size(), 1u);

    // Find the BTCUSDT bracket
    const truetest::exits::IBracketAdapter::recovered_bracket* btc = nullptr;
    for (const auto& b : recovered)
    {
        if (b.symbol == "BTCUSDT") btc = &b;
    }
    ASSERT_NE(btc, nullptr);

    EXPECT_EQ(btc->opener_order_id, 10u);
    ASSERT_TRUE(btc->stop_loss.has_value());
    ASSERT_TRUE(btc->take_profit.has_value());
    EXPECT_DOUBLE_EQ(*btc->stop_loss,   95.5);
    EXPECT_DOUBLE_EQ(*btc->take_profit, 110.0);
    EXPECT_EQ(btc->handles.sl_exchange_id.value_or(""), "1001");
    EXPECT_EQ(btc->handles.tp_exchange_id.value_or(""), "1002");
    EXPECT_EQ(btc->close_side, order_side::sell);

}

TEST(BinanceFuturesBracketAdapter, ListOpenRefusesUnexpectedConfiguredSymbol)
{
    auto post = std::make_shared<fake_caller>();
    auto del = std::make_shared<fake_caller>();
    auto get = std::make_shared<fake_caller>();
    get->responses = {{200, R"([{"algoId":1,"clientAlgoId":"tt-fb-sl-7","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"ETHUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"90","closePosition":true,"reduceOnly":false}])"}};
    BinanceFuturesBracketAdapter a(
        wrap(post), wrap(del), wrap(get), "BTCUSDT");
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, ListOpenWithoutGetCallableRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del));
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, ListOpenHttpFailureRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->responses = {{503, R"({"msg":"unavailable"})"}};
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del), wrap(get));
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, ListOpenMalformedSuccessRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->responses = {{200, R"({"unexpected":[]})"}};
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del), wrap(get));
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, ListOpenMissingIdentityRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->responses = {{200, R"([{"nested":{"algoId":1,"clientAlgoId":"tt-fb-sl-7","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"90","closePosition":true,"reduceOnly":false}}])"}};
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del), wrap(get));
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, ListOpenDuplicateRowOrLegRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->responses = {{200, R"([
        {"algoId":1,"clientAlgoId":"tt-fb-sl-7","clientAlgoId":"tt-fb-tp-7","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"90","closePosition":true,"reduceOnly":false}
    ])"}};
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del), wrap(get));
    EXPECT_THROW(a.list_open(), std::runtime_error);

    get->log.clear();
    get->responses = {{200, R"([
        {"algoId":1,"clientAlgoId":"tt-fb-sl-7","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"90","closePosition":true,"reduceOnly":false},
        {"algoId":2,"clientAlgoId":"tt-fb-sl-7","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"91","closePosition":true,"reduceOnly":false}
    ])"}};
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, ListOpenInvalidSemanticFieldsRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->responses = {{200, R"([
        {"algoId":1,"clientAlgoId":"tt-fb-sl-7","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"UNKNOWN","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"junk","closePosition":true,"reduceOnly":false}
    ])"}};
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del), wrap(get));
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, ListOpenTrailingIdentityRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->responses = {{200, R"([{
      "algoId":1,"clientAlgoId":"tt-fb-sl-7evil","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"90","closePosition":true,"reduceOnly":false
    }])"}};
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del), wrap(get));
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceFuturesBracketAdapter, ListOpenDuplicateVenueOrderIdRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del = std::make_shared<fake_caller>();
    auto get = std::make_shared<fake_caller>();
    get->responses = {{200, R"([
      {"algoId":1,"clientAlgoId":"tt-fb-sl-7","algoType":"CONDITIONAL","orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"90","closePosition":true,"reduceOnly":false},
      {"algoId":1,"clientAlgoId":"tt-fb-tp-8","algoType":"CONDITIONAL","orderType":"TAKE_PROFIT_MARKET","symbol":"BTCUSDT","side":"SELL","positionSide":"BOTH","algoStatus":"NEW","triggerPrice":"110","closePosition":true,"reduceOnly":false}
    ])"}};
    BinanceFuturesBracketAdapter a(wrap(post), wrap(del), wrap(get));
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

#endif // HAS_BINANCE
