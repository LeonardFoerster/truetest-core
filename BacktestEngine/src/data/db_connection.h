#pragma once
#include "data_handler.h"

#include <optional>
#include <pqxx/pqxx>
#include <memory>

class data_handler;

class database_connection
{

private:
    std::optional<pqxx::connection> connection_;  

public:
    
    pqxx::connection& establish_connection();
        
    void test_connection();
    int load_data(std::shared_ptr<data_handler> dh);
    void write_data();



};