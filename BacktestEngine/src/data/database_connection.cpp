#include <iostream>
#include <pqxx/pqxx>
#include <optional>
#include <expected>
#include <string>
#include <stdexcept>
#include <array>
#include <chrono>
#include <string_view>
#include <memory>
#include <cstdint>  
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "db_connection.h"
#include "data_handler.h"
#include "../core/event.h"

class data_handler;

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

void database_connection::test_connection()
{
    bool connection_active = false;
    bool write_test_sucessfull = false;
    bool read_test_sucessfull = false;

    auto start = std::chrono::high_resolution_clock::now();

    if (!(connection_ && connection_->is_open())) 
    {
        throw std::runtime_error("Connection failed");
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
       
}

void database_connection::load_data(const std::shared_ptr<data_handler> dh)
{
    std::string cache_file = "data_cache.bin";

    if (std::filesystem::exists(cache_file)) {
        std::cout << "Loading from cache..." << std::endl;
        std::ifstream ifs(cache_file, std::ios::binary);
        size_t size;
        ifs.read(reinterpret_cast<char*>(&size), sizeof(size));

        dh->db_data_symbol.resize(size);
        for (auto& s : dh->db_data_symbol) {
            size_t len;
            ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
            s.resize(len);
            ifs.read(&s[0], len);
        }

        dh->db_data_open_value.resize(size);
        ifs.read(reinterpret_cast<char*>(dh->db_data_open_value.data()), size * sizeof(double));

        dh->db_data_high_value.resize(size);
        ifs.read(reinterpret_cast<char*>(dh->db_data_high_value.data()), size * sizeof(double));

        dh->db_data_low_value.resize(size);
        ifs.read(reinterpret_cast<char*>(dh->db_data_low_value.data()), size * sizeof(double));

        dh->db_data_close_value.resize(size);
        ifs.read(reinterpret_cast<char*>(dh->db_data_close_value.data()), size * sizeof(double));

        dh->db_data_volume_value.resize(size);
        ifs.read(reinterpret_cast<char*>(dh->db_data_volume_value.data()), size * sizeof(int64_t));

        std::cout << "Loaded " << size << " records from cache." << std::endl;
    } else {
        std::cout << "Loading from database..." << std::endl;
        auto start = std::chrono::high_resolution_clock::now();

        if (!(connection_ && connection_->is_open()))
        {
            throw std::runtime_error("Connection not Active");
        }

        pqxx::read_transaction load_data_from_database(*connection_); 
        pqxx::result line_count = load_data_from_database.exec("SELECT COUNT (*) FROM tick_data");
        std::size_t n = line_count[0][0].as<std::size_t>();
        const std::size_t report_interval = n > 0 ? std::max<std::size_t>(std::size_t{ 1 }, n / 100) : 1;

        std::size_t processed = 0;
        std::cout << "\rloading: 0/" << n << std::flush;

        // Execute query once and iterate over results
        pqxx::result r = load_data_from_database.exec("SELECT CAST(symbol AS VARCHAR(8)), CAST(open AS DOUBLE PRECISION), CAST(high AS DOUBLE PRECISION), CAST(low AS DOUBLE PRECISION), CAST(close AS DOUBLE PRECISION), CAST(volume AS INT) FROM tick_data;");
        
        for (auto row : r)
        {
            auto [symbol, open, high, low, close, volume] = row.as<std::string, double, double, double, double, int64_t>();
            dh->load_into_queue(symbol, open, high, low, close, volume, n);
            ++processed;

            if ((processed % report_interval) == 0 || processed == n)
            {
                std::cout << "\rloading: " << processed << "/" << n << std::flush;
            }
        }
        std::cout << std::endl;
         
        // Idea: pqx::stream for executing the command -> from the results reading in the doubles
        // Consider using binary data
        // potential issue: the for loop executes the sql command for every itearion instead of doing it once and fetching the result
               
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
            
        std::cout << "time: " << duration << " seconds" << std::endl;

        // Save to cache
        std::cout << "Saving to cache..." << std::endl;
        std::ofstream ofs(cache_file, std::ios::binary);
        size_t size = dh->db_data_symbol.size();
        ofs.write(reinterpret_cast<const char*>(&size), sizeof(size));

        for (const auto& s : dh->db_data_symbol) {
            size_t len = s.size();
            ofs.write(reinterpret_cast<const char*>(&len), sizeof(len));
            ofs.write(s.data(), len);
        }

        ofs.write(reinterpret_cast<const char*>(dh->db_data_open_value.data()), size * sizeof(double));
        ofs.write(reinterpret_cast<const char*>(dh->db_data_high_value.data()), size * sizeof(double));
        ofs.write(reinterpret_cast<const char*>(dh->db_data_low_value.data()), size * sizeof(double));
        ofs.write(reinterpret_cast<const char*>(dh->db_data_close_value.data()), size * sizeof(double));
        ofs.write(reinterpret_cast<const char*>(dh->db_data_volume_value.data()), size * sizeof(int64_t));

        std::cout << "Cache saved." << std::endl;
    }
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
