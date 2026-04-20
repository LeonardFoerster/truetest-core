#ifdef HAS_SQLITE
#include "sqlite_store.h"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <chrono>

SqliteStore::SqliteStore(const std::string& db_path)
{
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK)
    {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQLite open failed: " + err);
    }

    char* err_msg = nullptr;
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err_msg);
    if (err_msg) sqlite3_free(err_msg);

    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, &err_msg);
    if (err_msg) sqlite3_free(err_msg);

    create_tables();
    prepare_statements();
}

SqliteStore::~SqliteStore()
{
    flush_all();

    if (insert_fill_stmt_)      sqlite3_finalize(insert_fill_stmt_);
    if (insert_portfolio_stmt_) sqlite3_finalize(insert_portfolio_stmt_);
    if (insert_equity_stmt_)    sqlite3_finalize(insert_equity_stmt_);

    if (db_) sqlite3_close(db_);
}

void SqliteStore::check(int rc, const char* context)
{
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(std::string(context) + ": " + sqlite3_errmsg(db_));
}

void SqliteStore::create_tables()
{
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS fills (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp   INTEGER NOT NULL,
            order_id    INTEGER NOT NULL,
            symbol      TEXT NOT NULL,
            side        TEXT NOT NULL,
            quantity    REAL NOT NULL,
            price       REAL NOT NULL,
            commission  REAL NOT NULL DEFAULT 0,
            created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now') * 1000)
        );

        CREATE TABLE IF NOT EXISTS portfolio_snapshots (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp   INTEGER NOT NULL,
            cash        REAL NOT NULL,
            equity      REAL NOT NULL,
            positions   TEXT NOT NULL,
            total_trades INTEGER NOT NULL,
            created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now') * 1000)
        );

        CREATE TABLE IF NOT EXISTS equity_curve (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp   INTEGER NOT NULL,
            equity      REAL NOT NULL
        );

        CREATE TABLE IF NOT EXISTS runs (
            run_id       TEXT PRIMARY KEY,
            started_at   INTEGER NOT NULL,
            ended_at     INTEGER,
            config_json  TEXT NOT NULL,
            status       TEXT NOT NULL,
            final_equity REAL,
            sharpe       REAL,
            max_drawdown REAL,
            trade_count  INTEGER
        );

        CREATE INDEX IF NOT EXISTS idx_fills_timestamp ON fills(timestamp);
        CREATE INDEX IF NOT EXISTS idx_fills_symbol ON fills(symbol);
        CREATE INDEX IF NOT EXISTS idx_equity_timestamp ON equity_curve(timestamp);
        CREATE INDEX IF NOT EXISTS idx_runs_started ON runs(started_at);
    )";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK)
    {
        std::string err = err_msg ? err_msg : "unknown";
        if (err_msg) sqlite3_free(err_msg);
        throw std::runtime_error("SQLite create_tables failed: " + err);
    }
}

void SqliteStore::prepare_statements()
{
    check(sqlite3_prepare_v2(db_,
        "INSERT INTO fills (timestamp, order_id, symbol, side, quantity, price, commission) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &insert_fill_stmt_, nullptr), "prepare insert_fill");

    check(sqlite3_prepare_v2(db_,
        "INSERT INTO portfolio_snapshots (timestamp, cash, equity, positions, total_trades) "
        "VALUES (?, ?, ?, ?, ?)",
        -1, &insert_portfolio_stmt_, nullptr), "prepare insert_portfolio");

    check(sqlite3_prepare_v2(db_,
        "INSERT INTO equity_curve (timestamp, equity) VALUES (?, ?)",
        -1, &insert_equity_stmt_, nullptr), "prepare insert_equity");
}

void SqliteStore::insert_fill(const fill_event& f)
{
    if (!in_fill_transaction_)
    {
        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        in_fill_transaction_ = true;
        fill_batch_count_ = 0;
    }

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        f.get_timestamp().time_since_epoch()).count();
    const char* side_str = (f.get_side() == order_side::buy) ? "buy" : "sell";

    sqlite3_reset(insert_fill_stmt_);
    sqlite3_bind_int64(insert_fill_stmt_, 1, ts_ms);
    sqlite3_bind_int64(insert_fill_stmt_, 2, static_cast<int64_t>(f.get_order_id()));
    sqlite3_bind_text(insert_fill_stmt_, 3, f.get_symbol().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_fill_stmt_, 4, side_str, -1, SQLITE_STATIC);
    sqlite3_bind_double(insert_fill_stmt_, 5, f.get_filled_quantity());
    sqlite3_bind_double(insert_fill_stmt_, 6, f.get_fill_price());
    sqlite3_bind_double(insert_fill_stmt_, 7, f.get_commission());

    int rc = sqlite3_step(insert_fill_stmt_);
    if (rc != SQLITE_DONE)
        check(rc, "insert_fill step");

    fill_batch_count_++;
    if (fill_batch_count_ >= BATCH_SIZE)
        flush_fill_batch();
}

void SqliteStore::insert_portfolio_snapshot(double cash, double equity,
    const std::string& positions_json, std::size_t total_trades,
    int64_t timestamp_ms)
{
    sqlite3_reset(insert_portfolio_stmt_);
    sqlite3_bind_int64(insert_portfolio_stmt_, 1, timestamp_ms);
    sqlite3_bind_double(insert_portfolio_stmt_, 2, cash);
    sqlite3_bind_double(insert_portfolio_stmt_, 3, equity);
    sqlite3_bind_text(insert_portfolio_stmt_, 4, positions_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_portfolio_stmt_, 5, static_cast<int>(total_trades));

    int rc = sqlite3_step(insert_portfolio_stmt_);
    if (rc != SQLITE_DONE)
        check(rc, "insert_portfolio step");
}

void SqliteStore::insert_equity_point(int64_t timestamp_ms, double equity)
{
    if (!in_equity_transaction_)
    {
        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        in_equity_transaction_ = true;
        equity_batch_count_ = 0;
    }

    sqlite3_reset(insert_equity_stmt_);
    sqlite3_bind_int64(insert_equity_stmt_, 1, timestamp_ms);
    sqlite3_bind_double(insert_equity_stmt_, 2, equity);

    int rc = sqlite3_step(insert_equity_stmt_);
    if (rc != SQLITE_DONE)
        check(rc, "insert_equity step");

    equity_batch_count_++;
    if (equity_batch_count_ >= BATCH_SIZE)
        flush_equity_batch();
}

void SqliteStore::flush_fill_batch()
{
    if (in_fill_transaction_)
    {
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        in_fill_transaction_ = false;
        fill_batch_count_ = 0;
    }
}

void SqliteStore::flush_equity_batch()
{
    if (in_equity_transaction_)
    {
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        in_equity_transaction_ = false;
        equity_batch_count_ = 0;
    }
}

void SqliteStore::flush_all()
{
    flush_fill_batch();
    flush_equity_batch();
}

std::string SqliteStore::query_fills_json(const std::string& symbol,
    int limit, int64_t since_ms)
{
    std::string sql = "SELECT id, timestamp, order_id, symbol, side, quantity, price, commission "
                      "FROM fills";
    bool has_where = false;
    if (!symbol.empty())
    {
        sql += " WHERE symbol = ?";
        has_where = true;
    }
    if (since_ms > 0)
    {
        sql += has_where ? " AND " : " WHERE ";
        sql += "timestamp >= ?";
    }
    sql += " ORDER BY timestamp DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr), "prepare query_fills");

    int bind_idx = 1;
    if (!symbol.empty())
        sqlite3_bind_text(stmt, bind_idx++, symbol.c_str(), -1, SQLITE_TRANSIENT);
    if (since_ms > 0)
        sqlite3_bind_int64(stmt, bind_idx++, since_ms);
    sqlite3_bind_int(stmt, bind_idx, limit);

    std::string result = "[";
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (!first) result += ",";
        first = false;

        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"id":%lld,"timestamp":%lld,"order_id":%lld,"symbol":"%s","side":"%s","quantity":%.8g,"price":%.6f,"commission":%.6f})",
            static_cast<long long>(sqlite3_column_int64(stmt, 0)),
            static_cast<long long>(sqlite3_column_int64(stmt, 1)),
            static_cast<long long>(sqlite3_column_int64(stmt, 2)),
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)),
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)),
            sqlite3_column_double(stmt, 5),
            sqlite3_column_double(stmt, 6),
            sqlite3_column_double(stmt, 7));
        result += buf;
    }

    result += "]";
    sqlite3_finalize(stmt);
    return result;
}

std::string SqliteStore::query_equity_json(int limit)
{
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_,
        "SELECT timestamp, equity FROM equity_curve ORDER BY timestamp DESC LIMIT ?",
        -1, &stmt, nullptr), "prepare query_equity");
    sqlite3_bind_int(stmt, 1, limit);

    std::string result = "[";
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (!first) result += ",";
        first = false;

        char buf[128];
        std::snprintf(buf, sizeof(buf),
            R"({"timestamp":%lld,"equity":%.2f})",
            static_cast<long long>(sqlite3_column_int64(stmt, 0)),
            sqlite3_column_double(stmt, 1));
        result += buf;
    }

    result += "]";
    sqlite3_finalize(stmt);
    return result;
}

std::string SqliteStore::begin_run(const std::string& config_json)
{
    static std::atomic<uint64_t> counter{0};
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    char id_buf[64];
    std::snprintf(id_buf, sizeof(id_buf), "run_%lld_%llu",
        static_cast<long long>(now_ms),
        static_cast<unsigned long long>(counter.fetch_add(1)));
    std::string run_id(id_buf);

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_,
        "INSERT INTO runs (run_id, started_at, config_json, status) "
        "VALUES (?, ?, ?, 'running')",
        -1, &stmt, nullptr), "prepare begin_run");

    sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, now_ms);
    sqlite3_bind_text(stmt, 3, config_json.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        check(rc, "begin_run step");

    return run_id;
}

void SqliteStore::end_run(const std::string& run_id, double final_equity,
                          double sharpe, double max_drawdown, int trade_count)
{
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_,
        "UPDATE runs SET ended_at = ?, status = 'completed', "
        "final_equity = ?, sharpe = ?, max_drawdown = ?, trade_count = ? "
        "WHERE run_id = ?",
        -1, &stmt, nullptr), "prepare end_run");

    sqlite3_bind_int64(stmt, 1, now_ms);
    sqlite3_bind_double(stmt, 2, final_equity);
    sqlite3_bind_double(stmt, 3, sharpe);
    sqlite3_bind_double(stmt, 4, max_drawdown);
    sqlite3_bind_int(stmt, 5, trade_count);
    sqlite3_bind_text(stmt, 6, run_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        check(rc, "end_run step");
}

void SqliteStore::fail_run(const std::string& run_id, const std::string& error)
{
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_,
        "UPDATE runs SET ended_at = ?, status = 'failed', "
        "config_json = config_json || ' | error: ' || ? "
        "WHERE run_id = ?",
        -1, &stmt, nullptr), "prepare fail_run");

    sqlite3_bind_int64(stmt, 1, now_ms);
    sqlite3_bind_text(stmt, 2, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, run_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        check(rc, "fail_run step");
}

std::string SqliteStore::query_runs_json(int limit)
{
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_,
        "SELECT run_id, started_at, ended_at, status, final_equity, sharpe, "
        "max_drawdown, trade_count FROM runs ORDER BY started_at DESC LIMIT ?",
        -1, &stmt, nullptr), "prepare query_runs");
    sqlite3_bind_int(stmt, 1, limit);

    std::string result = "[";
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (!first) result += ",";
        first = false;

        const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        bool ended_null = sqlite3_column_type(stmt, 2) == SQLITE_NULL;

        char buf[768];
        std::snprintf(buf, sizeof(buf),
            R"({"run_id":"%s","started_at":%lld,"ended_at":%s,"status":"%s",)"
            R"("final_equity":%.4f,"sharpe":%.4f,"max_drawdown":%.4f,"trade_count":%d})",
            id ? id : "",
            static_cast<long long>(sqlite3_column_int64(stmt, 1)),
            ended_null ? "null" : std::to_string(sqlite3_column_int64(stmt, 2)).c_str(),
            status ? status : "",
            sqlite3_column_double(stmt, 4),
            sqlite3_column_double(stmt, 5),
            sqlite3_column_double(stmt, 6),
            sqlite3_column_int(stmt, 7));
        result += buf;
    }

    result += "]";
    sqlite3_finalize(stmt);
    return result;
}

std::string SqliteStore::query_last_portfolio_json()
{
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_,
        "SELECT timestamp, cash, equity, positions, total_trades "
        "FROM portfolio_snapshots ORDER BY id DESC LIMIT 1",
        -1, &stmt, nullptr), "prepare query_last_portfolio");

    std::string result;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* positions = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            R"({"timestamp":%lld,"cash":%.2f,"equity":%.2f,"positions":%s,"total_trades":%d})",
            static_cast<long long>(sqlite3_column_int64(stmt, 0)),
            sqlite3_column_double(stmt, 1),
            sqlite3_column_double(stmt, 2),
            positions ? positions : "[]",
            sqlite3_column_int(stmt, 4));
        result = buf;
    }

    sqlite3_finalize(stmt);
    return result;
}

#endif // HAS_SQLITE
