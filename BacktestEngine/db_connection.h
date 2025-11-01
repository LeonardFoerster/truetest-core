#pragma once

#include <optional>
#include <pqxx/pqxx>

class database_connection
{
private:
    std::optional<pqxx::connection> connection_;  // Stores the connection if established

public:
    std::optional<pqxx::connection> establish_connection();
    int test_connection();
    void load_data();
    void write_data();
};