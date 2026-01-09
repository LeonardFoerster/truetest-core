#include "market_maker.h"
#include <chrono>

MarketMaker::MarketMaker() : gen(std::chrono::system_clock::now().time_since_epoch().count()), dis(0.0, 0.005) {}

void MarketMaker::add_orders(std::shared_ptr<orderbook> ob, double current_price, int num_orders) {
    for (int i = 0; i < num_orders; ++i) {
        double spread = dis(gen);
        double buy_price = current_price * (1 - spread);
        double sell_price = current_price * (1 + spread);

        // Scale prices as in the code
        int buy_price_scaled = static_cast<int>(buy_price * 100);
        int sell_price_scaled = static_cast<int>(sell_price * 100);

        auto buy_order = std::make_shared<order>(ob_order_type::good_till_cancel, 1000000 + i, side::buy, buy_price_scaled, 100);
        auto sell_order = std::make_shared<order>(ob_order_type::good_till_cancel, 2000000 + i, side::sell, sell_price_scaled, 100);

        ob->add_order(buy_order);
        ob->add_order(sell_order);
    }
}