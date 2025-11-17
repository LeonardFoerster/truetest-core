#include <iostream>
#include <string>
#include <pqxx/pqxx>
#include <fstream>
#include <cstdlib>

#include "core/backtest_core.h"
#include "data/db_connection.h"
#include "orderbook/orderbook.h"


void test_ob()
{
    orderbook order_book_;
    const order_id order_id = 1;
    order_book_.add_order(std::make_shared<order>(order_type::good_till_cancel, order_id, side::buy, 100, 10));
    std::cout << order_book_.size() << std::endl;
}


int main()
{
    auto db = std::make_shared<database_connection>();
    auto dh = std::make_shared<data_handler>();
    
    db->establish_connection();
    db->test_connection();
    db->load_data(dh);
    
    std::cout << "----------------------" << std::endl;
    test_ob();

    return 0;

}