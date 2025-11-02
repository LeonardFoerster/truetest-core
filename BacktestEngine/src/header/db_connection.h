#pragma once
#include "data_handler.h"

#include <optional>
#include <pqxx/pqxx>

class database_connection
{

private:
    std::optional<pqxx::connection> connection_;  

public:
    std::optional<pqxx::connection> establish_connection();
    int test_connection();
    int load_data();
    void write_data();



};