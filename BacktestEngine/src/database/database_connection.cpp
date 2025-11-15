#include <iostream>
#include <pqxx/pqxx>
#include <optional>
#include <expected>
#include <string>
#include <stdexcept>
#include <array>
#include <chrono>
#include <string_view>


#include "../header/db_connection.h"
#include "../header/data_handler.h"


pqxx::connection& database_connection::establish_connection()
{
    try
    {
        
        std::cout << "pgpass.conf file path:" << std::endl;
        std::string password_file;
        std::cin >> password_file;

        std::string conn_string =
            "dbname=storage user=leonard host=localhost port=5433 "
            "passfile='" + password_file + "'";

        connection_.emplace(conn_string);

        if (!connection_->is_open())
        {
            throw std::runtime_error("Failed to connect. Check pgpass.conf and credentials.");
        }

        std::cout << "Connected to database: " << connection_->dbname() << std::endl;
        return *connection_;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Connection error: " << e.what() << std::endl;
        connection_.reset();
        throw;
    }
}


int database_connection::test_connection()
{
    bool connection_active = false;
    bool write_test_sucessfull = false;
    bool read_test_sucessfull = false;

    auto start = std::chrono::high_resolution_clock::now();

    if (!(connection_ && connection_->is_open())) 
    {
        std::cout << "Connection failed" << std::endl;
        return 0;
    }
    else
    {
        connection_active = true;
        std::cout << "Connection Test Passed: " << std::boolalpha << connection_active << std::endl;
        auto time_stamp_connection_valid_test = std::chrono::high_resolution_clock::now();
        auto time_stamp_connection_valid_result = std::chrono::duration_cast<std::chrono::milliseconds>(time_stamp_connection_valid_test - start);
        std::cout << "Connection time: " << time_stamp_connection_valid_result << std::endl;
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

        std::cout << "Writing Test Passed: " << std::boolalpha << write_test_sucessfull << std::endl;
        auto timestamp_write_test = std::chrono::high_resolution_clock::now();
        auto write_valid_test_result = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp_write_test - start);
        std::cout << "Write test time: " << write_valid_test_result << std::endl;

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

        std::cout << "Read Test Passed: " << std::boolalpha << read_test_sucessfull << std::endl;
        auto timestamp_read_test = std::chrono::high_resolution_clock::now();
        auto read_valid_test_result = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp_read_test - start);
        std::cout << "Read Test time: " << read_valid_test_result << std::endl;
    }
    catch(std::exception e)    
    {
        std::cerr << "Reading Test failed" << std::endl;
    }
    
    return 0;
}


int database_connection::load_data(std::shared_ptr<data_handler> dh)
{
    auto start = std::chrono::high_resolution_clock::now();

    if (!(connection_ && connection_->is_open()))
    {
        throw std::runtime_error("Connection not Active");
    }

    pqxx::read_transaction load_data_from_database(*connection_); 
    pqxx::result line_count = load_data_from_database.exec("SELECT COUNT (*) FROM tick_data");
    std::size_t n = line_count[0][0].as<std::size_t>();


    dh->db_data_open_value.reserve(n);
    dh->db_data_high_value.reserve(n);
    dh->db_data_low_value.reserve(n);
    dh->db_data_close_value.reserve(n);

    auto timestamp = std::chrono::high_resolution_clock::now();
    auto timestamp_duration = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - start);

    std::cout << "timestamp after reserve: " << timestamp_duration << std::endl;


    for (auto [open, high, low, close] : load_data_from_database.query<double, double, double, double>
            ("SELECT CAST (open as DOUBLE PRECISION), CAST (high as DOUBLE PRECISION), CAST (low as DOUBLE PRECISION), CAST (close as DOUBLE PRECISION)FROM tick_data ORDER BY tick_id")
        )
    {
                
        dh->db_data_open_value.emplace_back(open);
        dh->db_data_high_value.emplace_back(high);
        dh->db_data_low_value.emplace_back(low);
        dh->db_data_close_value.emplace_back(close); 
    }
     
    // Idea: pqx::stream for executing the command -> from the results reading in the doubles
    // Consider using binary data
    // potential issue: the for loop executes the sql command for every itearion instead of doing it once and fetching the result
           
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
        
    std::cout << "time: " << duration << " seconds" << std::endl;
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