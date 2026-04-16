#ifdef HAS_POSTGRESQL

#include "pg_data_source.h"
#include "data_handler.h"
#include "../core/event.h"

#include <iostream>
#include <pqxx/pqxx>
#include <optional>
#include <string>
#include <stdexcept>
#include <chrono>
#include <memory>
#include <cstdint>
#include <algorithm>

pqxx::connection& PgDataSource::establish_connection() {
    std::cout << "pgpass.conf file path:" << std::endl;
    std::string password_file;
    std::cin >> password_file;

    std::string conn_string =
        "dbname=storage user=leonard host=localhost port=5433 "
        "passfile='" + password_file + "'";

    retry_config cfg;
    cfg.max_attempts = 5;
    cfg.initial_delay = std::chrono::milliseconds(1000);
    cfg.max_delay = std::chrono::milliseconds(16000);
    cfg.on_retry = [](unsigned attempt, std::exception_ptr ex) {
        std::cerr << "PostgreSQL connection attempt " << attempt << " failed";
        if (ex) {
            try { std::rethrow_exception(ex); }
            catch (const std::exception& e) { std::cerr << ": " << e.what(); }
            catch (...) {}
        }
        std::cerr << ", retrying...\n";
    };

    bool ok = retry_with_backoff([&]() {
        connection_.reset();
        connection_.emplace(conn_string);
        return connection_->is_open();
    }, cfg);

    if (!ok || !connection_ || !connection_->is_open()) {
        connection_.reset();
        throw std::runtime_error("PostgreSQL connection failed after retries");
    }

    std::cout << "Connected to database: " << connection_->dbname() << std::endl;
    return *connection_;
}

void PgDataSource::test_connection() {
    bool connection_active = false;
    bool write_test_sucessfull = false;
    bool read_test_sucessfull = false;

    auto start = std::chrono::high_resolution_clock::now();

    if (!(connection_ && connection_->is_open())) {
        throw std::runtime_error("Connection failed");
    } else {
        connection_active = true;
        std::cout << "Connection Test Passed: " << std::boolalpha << connection_active << std::endl;
        auto time_stamp_connection_valid_test = std::chrono::high_resolution_clock::now();
        auto time_stamp_connection_valid_result = std::chrono::duration_cast<std::chrono::milliseconds>(time_stamp_connection_valid_test - start);
        std::cout << "Connection time: " << time_stamp_connection_valid_result.count() << "ms" << std::endl;
    }

    try {
        pqxx::work tx{*connection_};

        tx.exec("DROP TABLE IF EXISTS test");
        tx.exec("CREATE TABLE test (ticker VARCHAR(80), price NUMERIC);");

        connection_->prepare("ins_test", "INSERT INTO test (ticker, price) VALUES ($1, $2)");
        {
            tx.exec_params("INSERT INTO test (ticker, price) VALUES ($1, $2)",
                "IBM", 193.523);

            tx.exec_prepared("ins_test",
                "AAPL", 193.523);

            tx.commit();
        }
        write_test_sucessfull = true;

        std::cout << "Writing Test Passed: " << std::boolalpha << write_test_sucessfull << std::endl;
        auto timestamp_write_test = std::chrono::high_resolution_clock::now();
        auto write_valid_test_result = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp_write_test - start);
        std::cout << "Write test time: " << write_valid_test_result.count() << "ms" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Writing Test failed: " << e.what() << std::endl;
    }

    try {
        pqxx::read_transaction rtx{*connection_};
        pqxx::result r = rtx.exec("SELECT * FROM test");

        for (std::size_t i = 0; i < static_cast<std::size_t>(r.size()); ++i) {
            auto [ticker, price] = r[static_cast<pqxx::row::size_type>(i)].as<std::string, double>();
        }
        read_test_sucessfull = true;

        std::cout << "Read Test Passed: " << std::boolalpha << read_test_sucessfull << std::endl;
        auto timestamp_read_test = std::chrono::high_resolution_clock::now();
        auto read_valid_test_result = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp_read_test - start);
        std::cout << "Read Test time: " << read_valid_test_result.count() << "ms" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Reading Test failed" << std::endl;
    }
}

bool PgDataSource::load_data(const std::shared_ptr<data_handler> dh) {
    std::cout << "Loading from database..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    if (!(connection_ && connection_->is_open())) {
        throw std::runtime_error("Connection not Active");
    }

    pqxx::read_transaction load_data_from_database(*connection_);
    pqxx::result line_count = load_data_from_database.exec("SELECT COUNT (*) FROM tick_data");
    std::size_t n = line_count[0][0].as<std::size_t>();
    const std::size_t report_interval = n > 0 ? std::max<std::size_t>(std::size_t{1}, n / 100) : 1;

    std::size_t processed = 0;
    std::cout << "\rloading: 0/" << n << std::flush;

    pqxx::result r = load_data_from_database.exec("SELECT CAST(symbol AS VARCHAR(8)), CAST(open AS DOUBLE PRECISION), CAST(high AS DOUBLE PRECISION), CAST(low AS DOUBLE PRECISION), CAST(close AS DOUBLE PRECISION), CAST(volume AS INT) FROM tick_data;");

    for (auto row : r) {
        auto [symbol, open, high, low, close, volume] = row.as<std::string, double, double, double, double, int64_t>();
        dh->load_into_queue("", symbol, open, high, low, close, volume);
        ++processed;

        if ((processed % report_interval) == 0 || processed == n) {
            std::cout << "\rloading: " << processed << "/" << n << std::flush;
        }
    }
    std::cout << std::endl;

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);

    std::cout << "time: " << duration.count() << " seconds" << std::endl;
    return true;
}

#endif
