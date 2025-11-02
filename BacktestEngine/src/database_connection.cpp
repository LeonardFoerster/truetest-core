#include <iostream>
#include <pqxx/pqxx>
#include <optional>
#include <expected>
#include <string>
#include <stdexcept>
#include <array>
#include <chrono>

#include "header/db_connection.h"
#include "header/data_handler.h"

//data_handler dh;

std::optional<pqxx::connection> database_connection::establish_connection()
{
    try 
    {
        pqxx::connection conn("dbname=storage user=leonard password=leonard host=localhost port=5433");
        if (conn.is_open()) 
        {
            std::cout << "Connected to database: " << conn.dbname() << std::endl;
            connection_ = std::move(conn);
            return conn;
        }
        else 
        {
            std::cout << "Failed to connect." << std::endl;
            return std::nullopt;
        }
    }
    catch (const std::exception& e) 
    {
        std::cerr << "Connection error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

int database_connection::test_connection()
{
    bool connection_active = false;
    bool write_test_sucessfull = false;
    bool read_test_sucessfull = false;


    if (!(connection_ && connection_->is_open())) 
    {
        std::cout << "Connection failed" << std::endl;
        return 0;
    }
    else
    {
        connection_active = true;
    }

    try
    {
        pqxx::work tx{ *connection_ };

        tx.exec("DROP TABLE IF EXISTS test");
        tx.exec("CREATE TABLE test (ticker VARCHAR(80), price NUMERIC);");
        
        connection_->prepare("ins_test", "INSERT INTO test (ticker, price) VALUES ($1, $2)");
        {
            tx.exec("INSERT INTO test (ticker, price) VALUES ($1, $2)",
                pqxx::params{ "IBM", 193.523 });

            tx.exec(pqxx::prepped{ "ins_test" },
                pqxx::params{ "AAPL", 193.523 });

            tx.commit();
        }
        write_test_sucessfull = true;
    }
    catch (std::exception e)
    {
        std::cerr << "Writing Test failed: " << e.what() << std::endl;
    }

    try
    {
        pqxx::read_transaction rtx{ *connection_ }; 
        pqxx::result r = rtx.exec("SELECT * FROM test");

        for (std::size_t i = 0; i < r.size(); ++i) 
        {
            auto [ticker, price] = r[i].as<std::string, double>();
        }
        read_test_sucessfull = true;
    }
    catch(std::exception e)    
    {
        std::cerr << "Reading Test failed" << std::endl;
    }

    std::cout << "---------------Test Results--------------" << std::endl;
    std::cout << "Connection Test Passed: " << std::boolalpha << connection_active << std::endl;
    std::cout << "Writing Test Passed: " << std::boolalpha  << write_test_sucessfull << std::endl;
    std::cout << "Read Test Passed: " << std::boolalpha << read_test_sucessfull << std::endl;
    std::cout << "----------------------------------" << std::endl;
    return 0;
}

void database_connection::write_data()
{
    if (connection_ && connection_->is_open()) 
    {
     
        std::cout << "Writing data to database." << std::endl;

    }
    else 
    {
        std::cout << "No active connection to write data." << std::endl;
    }
}