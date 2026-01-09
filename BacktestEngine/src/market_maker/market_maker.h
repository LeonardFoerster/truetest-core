#pragma once
#include "../orderbook/orderbook.h"
#include <memory>
#include <random>

class MarketMaker {
public:
    MarketMaker();
    void add_orders(std::shared_ptr<orderbook> ob, double current_price, int num_orders = 10);

private:
    std::mt19937 gen;
    std::uniform_real_distribution<> dis;
};