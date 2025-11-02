#include <iostream>
#include <vector>

#include "header/db_connection.h"
#include "header/data_handler.h"


data_handler dh;

int database_connection::load_data()
{
    auto start = std::chrono::high_resolution_clock::now();

    if (!(connection_ && connection_->is_open()))
    {
        std::cerr << "Connection not active" << std::endl;
        return 1;      // error handling needs a rework
    }

    pqxx::work load_data_from_database(*connection_);

    pqxx::result line_count = load_data_from_database.exec("SELECT COUNT (*) FROM tick_data");
    std::size_t n = line_count[0][0].as<std::size_t>();

    auto stream = load_data_from_database.stream<double, double, double, double>("SELECT open, high, low, close FROM tick_data ORDER BY tick_id");

    dh.ohlc_database_data.reserve(n);
    std::cout << "size: " << sizeof(dh.ohlc_database_data) * dh.ohlc_database_data.capacity() << " bytes" << std::endl;;
    //dh.ohlc_database_data.emplace_back(stream); 








    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "time: " << duration << " microseconds" << std::endl;
}

