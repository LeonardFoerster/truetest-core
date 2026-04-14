#pragma once
#ifdef HAS_SQLITE

#include "../core/event.h"

#include <sqlite3.h>
#include <cstdint>
#include <memory>
#include <string>

class SqliteStore
{
public:
    explicit SqliteStore(const std::string& db_path = "truetest.db");
    ~SqliteStore();

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    // Write path
    void insert_fill(const fill_event& f);
    void insert_portfolio_snapshot(double cash, double equity,
        const std::string& positions_json, std::size_t total_trades,
        int64_t timestamp_ms);
    void insert_equity_point(int64_t timestamp_ms, double equity);

    // Read path
    std::string query_fills_json(const std::string& symbol = "",
        int limit = 200, int64_t since_ms = 0);
    std::string query_equity_json(int limit = 500);
    std::string query_last_portfolio_json();

    // Flush pending batches
    void flush_equity_batch();
    void flush_fill_batch();
    void flush_all();

private:
    void create_tables();
    void prepare_statements();
    void check(int rc, const char* context);

    sqlite3* db_ = nullptr;

    // Prepared statements
    sqlite3_stmt* insert_fill_stmt_ = nullptr;
    sqlite3_stmt* insert_portfolio_stmt_ = nullptr;
    sqlite3_stmt* insert_equity_stmt_ = nullptr;

    // Batching for equity and fill inserts
    static constexpr std::size_t BATCH_SIZE = 100;

    std::size_t equity_batch_count_ = 0;
    bool in_equity_transaction_ = false;

    std::size_t fill_batch_count_ = 0;
    bool in_fill_transaction_ = false;
};

#endif // HAS_SQLITE
