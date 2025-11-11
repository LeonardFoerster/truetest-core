#include "../header/data_handler.h"



#include <sstream>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <iterator>
#include <utility>

void data_handler::move_db_data_into_vector(double open, double high, double low, double close)
{
    data_handler dh;

    dh.db_data_open_value.emplace_back(open);
    dh.db_data_high_value.emplace_back(high);
    dh.db_data_low_value.emplace_back(low);
    dh.db_data_close_value.emplace_back(close);

}





