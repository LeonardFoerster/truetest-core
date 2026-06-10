#ifdef HAS_QUESTDB

#include "schema.h"

namespace truetest::questdb::schema {

std::string runs_meta_ddl()
{
    return R"(CREATE TABLE IF NOT EXISTS runs_meta (
    run_tag          SYMBOL CAPACITY 10000 INDEX,
    started_at       TIMESTAMP,
    ended_at         TIMESTAMP,
    mode             SYMBOL CAPACITY 4,
    binary           SYMBOL CAPACITY 4,
    strategy         SYMBOL CAPACITY 64,
    symbol           SYMBOL CAPACITY 1024,
    params           STRING,
    initial_equity   DOUBLE,
    final_equity     DOUBLE,
    total_orders     LONG,
    total_fills      LONG,
    total_rejections LONG,
    notes            STRING,

    -- Phase 4: Richer campaign summary for long-running analysis
    max_drawdown     DOUBLE,
    sharpe_ratio     DOUBLE,
    sortino_ratio    DOUBLE,
    profit_factor    DOUBLE,
    win_rate         DOUBLE,
    calmar_ratio     DOUBLE,
    total_trades     LONG,
    winning_trades   LONG
) TIMESTAMP(started_at) PARTITION BY WEEK)";
}

namespace {

std::string orders_ddl(const std::string& p)
{
    return "CREATE TABLE IF NOT EXISTS " + p + "_orders (\n"
        "    ts               TIMESTAMP,\n"
        "    run_tag          SYMBOL CAPACITY 10000,\n"
        "    order_id         LONG,\n"
        "    symbol           SYMBOL CAPACITY 1024,\n"
        "    side             SYMBOL CAPACITY 2,\n"
        "    type             SYMBOL CAPACITY 4,\n"
        "    tif              SYMBOL CAPACITY 4,\n"
        "    qty              DOUBLE,\n"
        "    price            DOUBLE,\n"
        "    stop_price       DOUBLE,\n"
        "    strategy_name    SYMBOL CAPACITY 64,\n"
        "    opener_order_id  LONG,\n"
        "    initial_status   SYMBOL CAPACITY 8\n"
        ") TIMESTAMP(ts) PARTITION BY DAY";
}

std::string order_status_ddl(const std::string& p)
{
    return "CREATE TABLE IF NOT EXISTS " + p + "_order_status (\n"
        "    ts               TIMESTAMP,\n"
        "    run_tag          SYMBOL CAPACITY 10000,\n"
        "    order_id         LONG,\n"
        "    old_status       SYMBOL CAPACITY 8,\n"
        "    new_status       SYMBOL CAPACITY 8,\n"
        "    reason           STRING\n"
        ") TIMESTAMP(ts) PARTITION BY DAY";
}

std::string fills_ddl(const std::string& p)
{
    return "CREATE TABLE IF NOT EXISTS " + p + "_fills (\n"
        "    ts               TIMESTAMP,\n"
        "    run_tag          SYMBOL CAPACITY 10000,\n"
        "    fill_id          LONG,\n"
        "    order_id         LONG,\n"
        "    opener_order_id  LONG,\n"
        "    symbol           SYMBOL CAPACITY 1024,\n"
        "    side             SYMBOL CAPACITY 2,\n"
        "    qty              DOUBLE,\n"
        "    price            DOUBLE,\n"
        "    remaining_qty    DOUBLE,\n"
        "    fee              DOUBLE,\n"
        "    strategy_name    SYMBOL CAPACITY 64,\n"
        "    source           SYMBOL CAPACITY 8\n"
        ") TIMESTAMP(ts) PARTITION BY DAY";
}

std::string rejections_ddl(const std::string& p)
{
    return "CREATE TABLE IF NOT EXISTS " + p + "_rejections (\n"
        "    ts               TIMESTAMP,\n"
        "    run_tag          SYMBOL CAPACITY 10000,\n"
        "    order_id         LONG,\n"
        "    symbol           SYMBOL CAPACITY 1024,\n"
        "    side             SYMBOL CAPACITY 2,\n"
        "    qty              DOUBLE,\n"
        "    price            DOUBLE,\n"
        "    strategy_name    SYMBOL CAPACITY 64,\n"
        "    reason           SYMBOL CAPACITY 32,\n"
        "    reason_detail    STRING\n"
        ") TIMESTAMP(ts) PARTITION BY DAY";
}

std::string cancellations_ddl(const std::string& p)
{
    return "CREATE TABLE IF NOT EXISTS " + p + "_cancellations (\n"
        "    ts               TIMESTAMP,\n"
        "    run_tag          SYMBOL CAPACITY 10000,\n"
        "    order_id         LONG,\n"
        "    symbol           SYMBOL CAPACITY 1024,\n"
        "    strategy_name    SYMBOL CAPACITY 64,\n"
        "    reason           SYMBOL CAPACITY 16\n"
        ") TIMESTAMP(ts) PARTITION BY DAY";
}

std::string amendments_ddl(const std::string& p)
{
    return "CREATE TABLE IF NOT EXISTS " + p + "_amendments (\n"
        "    ts               TIMESTAMP,\n"
        "    run_tag          SYMBOL CAPACITY 10000,\n"
        "    order_id         LONG,\n"
        "    symbol           SYMBOL CAPACITY 1024,\n"
        "    old_price        DOUBLE,\n"
        "    new_price        DOUBLE,\n"
        "    old_qty          DOUBLE,\n"
        "    new_qty          DOUBLE\n"
        ") TIMESTAMP(ts) PARTITION BY DAY";
}

std::string funding_ddl(const std::string& p)
{
    return "CREATE TABLE IF NOT EXISTS " + p + "_funding (\n"
        "    ts               TIMESTAMP,\n"
        "    run_tag          SYMBOL CAPACITY 10000,\n"
        "    symbol           SYMBOL CAPACITY 1024,\n"
        "    qty_change       DOUBLE,\n"
        "    cash_delta       DOUBLE,\n"
        "    reason           SYMBOL CAPACITY 32\n"
        ") TIMESTAMP(ts) PARTITION BY DAY";
}

std::string events_ddl(const std::string& p)
{
    // Phase 3: Generic event/decision log for richer logic capture
    // (strategy decisions, risk reasons with context, snapshots, etc.)
    return "CREATE TABLE IF NOT EXISTS " + p + "_events (\n"
        "    ts               TIMESTAMP,\n"
        "    run_tag          SYMBOL CAPACITY 10000,\n"
        "    event_type       SYMBOL CAPACITY 32,\n"
        "    symbol           SYMBOL CAPACITY 1024,\n"
        "    strategy_name    SYMBOL CAPACITY 64,\n"
        "    order_id         LONG,\n"
        "    severity         SYMBOL CAPACITY 8,\n"
        "    message          STRING,\n"
        "    details          STRING\n"
        ") TIMESTAMP(ts) PARTITION BY DAY";
}

// Phase 4 helper: appends TTL clause if ttl_days > 0
std::string with_ttl(const std::string& ddl, int ttl_days)
{
    if (ttl_days <= 0) return ddl;
    return ddl + " TTL " + std::to_string(ttl_days) + " DAYS";
}

}  // end anonymous namespace

std::vector<std::string> per_run_ddls(const std::string& run_tag, int ttl_days)
{
    return {
        with_ttl(orders_ddl(run_tag), ttl_days),
        with_ttl(order_status_ddl(run_tag), ttl_days),
        with_ttl(fills_ddl(run_tag), ttl_days),
        with_ttl(funding_ddl(run_tag), ttl_days),   // Phase 2: funding settlements
        with_ttl(events_ddl(run_tag), ttl_days),    // Phase 3: richer logic + decision capture
        with_ttl(rejections_ddl(run_tag), ttl_days),
        with_ttl(cancellations_ddl(run_tag), ttl_days),
        with_ttl(amendments_ddl(run_tag), ttl_days),
    };
}

std::vector<std::string> all_ddls(const std::string& run_tag, int ttl_days)
{
    std::vector<std::string> out;
    out.reserve(8);
    out.push_back(runs_meta_ddl());
    for (auto& d : per_run_ddls(run_tag, ttl_days)) out.push_back(std::move(d));
    return out;
}

} // namespace truetest::questdb::schema

#endif // HAS_QUESTDB
