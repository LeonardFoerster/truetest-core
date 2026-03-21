#include <gtest/gtest.h>
#include "core/engine.h"
#include "core/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include <sstream>

// RAII helper to silence cout during noisy backtest runs
struct SilenceCout_OT {
    std::streambuf* orig;
    std::ostringstream sink;
    SilenceCout_OT() : orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout_OT() { std::cout.rdbuf(orig); }
};

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// --- Event-level tests ---

TEST(OrderTypes, DefaultTIF_Market)
{
    order_event o(epoch_ms(0), "TEST", order_type::market, order_side::buy, 10, 100.0);
    EXPECT_EQ(o.get_tif(), time_in_force::ioc);
}

TEST(OrderTypes, DefaultTIF_Limit)
{
    order_event o(epoch_ms(0), "TEST", order_type::limit, order_side::buy, 10, 100.0);
    EXPECT_EQ(o.get_tif(), time_in_force::gtc);
}

TEST(OrderTypes, ExplicitTIF)
{
    order_event o(epoch_ms(0), "TEST", order_type::limit, order_side::buy, 10, 100.0, time_in_force::fok);
    EXPECT_EQ(o.get_tif(), time_in_force::fok);
}

TEST(OrderTypes, StopPrice)
{
    order_event o(epoch_ms(0), "TEST", order_type::stop, order_side::sell, 10, 0.0,
                  time_in_force::gtc, 95.0);
    EXPECT_DOUBLE_EQ(o.get_stop_price(), 95.0);
    EXPECT_EQ(o.get_order_type(), order_type::stop);
}

TEST(OrderTypes, StopLimitFields)
{
    order_event o(epoch_ms(0), "TEST", order_type::stop_limit, order_side::buy, 10, 105.0,
                  time_in_force::gtc, 103.0);
    EXPECT_EQ(o.get_order_type(), order_type::stop_limit);
    EXPECT_DOUBLE_EQ(o.get_price(), 105.0);       // limit price
    EXPECT_DOUBLE_EQ(o.get_stop_price(), 103.0);   // trigger price
}

TEST(OrderTypes, ToString_StopLimit)
{
    order_event o(epoch_ms(0), "X", order_type::stop_limit, order_side::buy, 1, 100.0);
    auto s = o.to_string();
    EXPECT_NE(s.find("STOP_LIMIT"), std::string::npos);
}

// --- Orderbook-level IOC tests ---

TEST(OrderTypes, IOC_PartialFill)
{
    orderbook ob;
    // Place a small ask
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::sell,
                                        Price::from_double(100.0), 5);
    ob.add_order(ask);

    // IOC buy for 10 — should fill 5, cancel remaining 5
    auto ioc_buy = std::make_shared<order>(ob_order_type::immediate_or_cancel, 2, side::buy,
                                            Price::from_double(100.0), 10);
    auto result = ob.add_order(ioc_buy);

    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].get_bid_trade().quantity_, 5u);
    // IOC order should be cancelled (not resting on book)
    EXPECT_EQ(ob.size(), 0u);
}

TEST(OrderTypes, IOC_FullFill)
{
    orderbook ob;
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::sell,
                                        Price::from_double(100.0), 10);
    ob.add_order(ask);

    auto ioc_buy = std::make_shared<order>(ob_order_type::immediate_or_cancel, 2, side::buy,
                                            Price::from_double(100.0), 10);
    auto result = ob.add_order(ioc_buy);

    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].get_bid_trade().quantity_, 10u);
    EXPECT_EQ(ob.size(), 0u); // both fully filled
}

TEST(OrderTypes, IOC_NoMatch)
{
    orderbook ob;
    // No liquidity on ask side — IOC buy should get nothing
    auto ioc_buy = std::make_shared<order>(ob_order_type::immediate_or_cancel, 1, side::buy,
                                            Price::from_double(100.0), 10);
    auto result = ob.add_order(ioc_buy);

    EXPECT_EQ(result.size(), 0u);
    // IOC should not rest on book
    EXPECT_EQ(ob.size(), 0u);
}

TEST(OrderTypes, FOK_AllOrNothing)
{
    orderbook ob;
    // Place a small ask (5 qty)
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::sell,
                                        Price::from_double(100.0), 5);
    ob.add_order(ask);

    // FOK buy for 10 — can't fully fill, should reject entirely
    auto fok_buy = std::make_shared<order>(ob_order_type::fill_or_kill, 2, side::buy,
                                            Price::from_double(100.0), 10);
    auto result = ob.add_order(fok_buy);

    EXPECT_EQ(result.size(), 0u);
    EXPECT_EQ(ob.size(), 1u); // only the original ask remains
}

// --- Engine-level stop order tests ---

// Strategy that emits a stop order on bar 2
class StopStrategy : public IStrategy
{
    int call_count_ = 0;
    order_type type_;
    order_side side_;
    double stop_price_;
    double limit_price_;
public:
    bool was_filled = false;

    StopStrategy(order_type type, order_side side, double stop_price, double limit_price = 0.0)
        : type_(type), side_(side), stop_price_(stop_price), limit_price_(limit_price) {}

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        call_count_++;
        if (call_count_ == 2)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(), type_,
                               side_, 10, limit_price_, time_in_force::gtc, stop_price_);
        }
        return std::nullopt;
    }

    void set_position_open(const std::string&, bool open) override
    {
        if (open) was_filled = true;
    }
};

static std::shared_ptr<data_handler> make_bar_data_prices(const std::vector<double>& closes)
{
    auto dh = std::make_shared<data_handler>();
    for (size_t i = 0; i < closes.size(); ++i)
    {
        double c = closes[i];
        dh->load_into_queue("2024-01-01", "TEST", c - 2.0, c + 2.0, c - 3.0, c, 1000);
    }
    return dh;
}

TEST(OrderTypes, StopBuy_Triggers)
{
    SilenceCout_OT quiet;
    // Prices: 100, 100, 105, 110 — stop buy at 104 should trigger on bar 3 (high >= 104)
    auto dh = make_bar_data_prices({100.0, 100.0, 105.0, 110.0});
    auto strat = std::make_shared<StopStrategy>(order_type::stop, order_side::buy, 104.0);

    MarketMaker mm;
    engine eng(dh, nullptr, strat);
    // Seed liquidity on the book
    auto ob = eng.get_orderbook_registry().get_or_create("TEST");
    mm.add_orders(ob, 105.0, 10);

    eng.run();
    EXPECT_TRUE(strat->was_filled);
}

TEST(OrderTypes, StopSell_Triggers)
{
    SilenceCout_OT quiet;
    // Prices: 100, 100, 95, 90 — stop sell at 96 should trigger on bar 3 (low <= 96)
    auto dh = make_bar_data_prices({100.0, 100.0, 95.0, 90.0});

    // Strategy that buys on bar 1, then places stop sell
    class BuyThenStopSell : public IStrategy
    {
        int call_count_ = 0;
        bool position_ = false;
    public:
        bool stop_triggered = false;

        std::optional<order_event> on_market(const market_event& mkt) override
        {
            call_count_++;
            if (call_count_ == 1)
            {
                // Use high + 5 to ensure fill against any ask near market
                return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                                   order_type::market, order_side::buy, 10, mkt.get_high() + 5.0);
            }
            if (call_count_ == 2 && position_)
            {
                return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                                   order_type::stop, order_side::sell, 10, 0.0,
                                   time_in_force::gtc, 96.0);
            }
            return std::nullopt;
        }

        void set_position_open(const std::string&, bool open) override
        {
            if (position_ && !open) stop_triggered = true;
            position_ = open;
        }
    };

    auto strat = std::make_shared<BuyThenStopSell>();
    engine eng(dh, nullptr, strat);
    auto ob = eng.get_orderbook_registry().get_or_create("TEST");
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    eng.run();
    EXPECT_TRUE(strat->stop_triggered);
}

TEST(OrderTypes, StopLimit_ConvertsToLimit)
{
    SilenceCout_OT quiet;
    // Prices: 100, 100, 105, 110 — stop-limit buy: trigger=104, limit=106
    auto dh = make_bar_data_prices({100.0, 100.0, 105.0, 110.0});
    auto strat = std::make_shared<StopStrategy>(order_type::stop_limit, order_side::buy, 104.0, 106.0);

    engine eng(dh, nullptr, strat);
    auto ob = eng.get_orderbook_registry().get_or_create("TEST");
    MarketMaker mm;
    mm.add_orders(ob, 105.0, 10);

    eng.run();
    EXPECT_TRUE(strat->was_filled);
}

TEST(OrderTypes, Stop_NoTrigger)
{
    SilenceCout_OT quiet;
    // Prices: 100, 100, 101, 102 — stop buy at 110 should never trigger
    auto dh = make_bar_data_prices({100.0, 100.0, 101.0, 102.0});
    auto strat = std::make_shared<StopStrategy>(order_type::stop, order_side::buy, 110.0);

    engine eng(dh, nullptr, strat);
    auto ob = eng.get_orderbook_registry().get_or_create("TEST");
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    eng.run();
    EXPECT_FALSE(strat->was_filled);
}

// --- Day order test ---

TEST(OrderTypes, DayOrder_CancelledAtSessionEnd)
{
    SilenceCout_OT quiet;

    // Strategy places a GTC limit and a DAY limit on bar 1, neither should match
    class DayOrderStrategy : public IStrategy
    {
        int call_count_ = 0;
    public:
        std::optional<order_event> on_market(const market_event& mkt) override
        {
            call_count_++;
            if (call_count_ == 1)
            {
                // Place a day limit buy far below market — won't fill
                return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                                   order_type::limit, order_side::buy, 10, 50.0,
                                   time_in_force::day);
            }
            return std::nullopt;
        }
        void set_position_open(const std::string&, bool) override {}
    };

    auto dh = make_bar_data_prices({100.0, 100.0, 100.0});
    auto strat = std::make_shared<DayOrderStrategy>();

    engine eng(dh, nullptr, strat);
    auto ob = eng.get_orderbook_registry().get_or_create("TEST");
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    eng.run();

    // After session end, the day order should have been cancelled from the book
    // The book should only contain market maker orders (no strategy day order)
    auto infos = ob->get_order_infos();
    for (const auto& bid : infos.get_bids())
    {
        // No bid at 50.0 should remain
        EXPECT_NE(bid.price_, Price::from_double(50.0));
    }
}

// --- GTC persists across bars ---

TEST(OrderTypes, GTC_PersistsAcrossBars)
{
    orderbook ob;
    // GTC order placed on book
    auto gtc = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy,
                                        Price::from_double(95.0), 10);
    ob.add_order(gtc);

    // Simulate several bars — GTC should still be on book
    EXPECT_EQ(ob.size(), 1u);

    // Eventually a matching ask arrives
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell,
                                        Price::from_double(95.0), 10);
    auto result = ob.add_order(ask);

    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(ob.size(), 0u);
}

// --- TIF via LocalBookAdapter ---

TEST(OrderTypes, Adapter_IOC_PartialFill)
{
    auto ob = std::make_shared<orderbook>();
    // Seed 5 qty on ask
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::sell,
                                        Price::from_double(100.0), 5);
    ob->add_order(ask);

    LocalBookAdapter adapter(ob, nullptr, nullptr);
    adapter.set_mid_price(100.0);

    // IOC buy for 10
    order_event ioc_order(epoch_ms(0), "TEST", order_type::market, order_side::buy, 10, 100.0);
    ioc_order.set_order_id(42);
    // market orders default to IOC
    EXPECT_EQ(ioc_order.get_tif(), time_in_force::ioc);

    adapter.submit_order(ioc_order);

    std::vector<fill_event> fills;
    EXPECT_TRUE(adapter.poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_filled_quantity(), 5);  // partial fill

    // IOC remainder cancelled — nothing left on book from the buy side
    EXPECT_EQ(ob->size(), 0u);
}
