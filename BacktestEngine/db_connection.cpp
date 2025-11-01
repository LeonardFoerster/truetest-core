#include <iostream>
#include <pqxx/pqxx>
#include <optional>

#include "db_connection.h"


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
    if (connection_ && connection_->is_open()) {
        std::cout << "Connection is active." << std::endl;
        return 0;  // Success
    }
    else {
        std::cout << "No active connection." << std::endl;
        return 1;  // Failure
    }
}

void database_connection::load_data()
{
    if (connection_ && connection_->is_open()) {
        // Use *connection_ here, e.g., for queries
        std::cout << "Loading data from database." << std::endl;
        // Example: pqxx::work txn(*connection_);
    }
    else {
        std::cout << "No active connection to load data." << std::endl;
    }
}

void database_connection::write_data()
{
    if (connection_ && connection_->is_open()) {
        // Use *connection_ here, e.g., for inserts
        std::cout << "Writing data to database." << std::endl;
        // Example: pqxx::work txn(*connection_);
    }
    else {
        std::cout << "No active connection to write data." << std::endl;
    }
}