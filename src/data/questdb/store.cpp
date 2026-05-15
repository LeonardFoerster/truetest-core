#ifdef HAS_QUESTDB

#include "store.h"
#include "http_client.h"
#include "schema.h"

#include <iostream>
#include <utility>

namespace truetest::questdb {

namespace {

// Default HTTP DDL executor wired to the real query_exec.
QuestdbStore::HttpExecFn make_default_http_exec(const std::string& host,
                                                std::uint16_t http_port)
{
    return [host, http_port](const std::string& sql) -> bool {
        auto resp = query_exec(host, http_port, sql);
        if (!resp)
        {
            std::cerr << "[questdb] DDL HTTP request failed (no response)\n";
            return false;
        }
        if (resp->status != 200)
        {
            std::cerr << "[questdb] DDL failed (HTTP " << resp->status
                      << "): " << sql.substr(0, 80) << "...\n"
                      << "  body: " << resp->body.substr(0, 200) << "\n";
            return false;
        }
        return true;
    };
}

}

QuestdbStore::QuestdbStore(StoreConfig cfg)
    : cfg_(std::move(cfg))
    , ilp_(std::make_unique<IlpWriter>(cfg_.host, cfg_.ilp_port))
    , http_exec_(make_default_http_exec(cfg_.host, cfg_.http_port))
{}

QuestdbStore::QuestdbStore(StoreConfig cfg,
                           std::unique_ptr<IlpWriter> writer,
                           HttpExecFn http_exec)
    : cfg_(std::move(cfg))
    , ilp_(std::move(writer))
    , http_exec_(std::move(http_exec))
{}

QuestdbStore::~QuestdbStore()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (ilp_) ilp_->flush();
}

std::string QuestdbStore::table_name(const char* suffix) const
{
    return cfg_.run_tag + "_" + suffix;
}

std::int64_t QuestdbStore::ns_from(std::chrono::system_clock::time_point ts)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        ts.time_since_epoch()).count();
}

std::int64_t QuestdbStore::now_ns()
{
    return ns_from(std::chrono::system_clock::now());
}

const char* QuestdbStore::side_str(order_side s)
{
    return (s == order_side::buy) ? "buy" : "sell";
}

const char* QuestdbStore::type_str(order_type t)
{
    switch (t)
    {
        case order_type::market:     return "mkt";
        case order_type::limit:      return "lmt";
        case order_type::stop:       return "stop";
        case order_type::stop_limit: return "slmt";
    }
    return "unk";
}

const char* QuestdbStore::tif_str(time_in_force t)
{
    switch (t)
    {
        case time_in_force::ioc: return "ioc";
        case time_in_force::fok: return "fok";
        case time_in_force::gtc: return "gtc";
        case time_in_force::day: return "day";
    }
    return "unk";
}

const char* QuestdbStore::status_str(order_status s)
{
    switch (s)
    {
        case order_status::pending:          return "pending";
        case order_status::open:             return "open";
        case order_status::partially_filled: return "partial";
        case order_status::filled:           return "filled";
        case order_status::cancelled:        return "cancelled";
        case order_status::rejected:         return "rejected";
    }
    return "unknown";
}

bool QuestdbStore::begin()
{
    started_at_ = std::chrono::system_clock::now();

    // 1. DDLs (7 statements) via HTTP.
    for (const auto& ddl : schema::all_ddls(cfg_.run_tag))
    {
        if (!http_exec_(ddl))
        {
            std::cerr << "[questdb] begin() aborted: DDL step failed\n";
            return false;
        }
    }

    // 2. Open persistent ILP socket.
    if (!ilp_->connect())
    {
        std::cerr << "[questdb] begin() aborted: ILP connect to "
                  << cfg_.host << ":" << cfg_.ilp_port << " failed\n";
        return false;
    }

    // 3. Initial runs_meta row.
    LineBuilder lb("runs_meta");
    lb.add_tag("run_tag", cfg_.run_tag);
    lb.add_tag("mode", cfg_.mode.empty() ? "backtest" : cfg_.mode);
    lb.add_tag("binary", cfg_.binary.empty() ? "engine_backtest" : cfg_.binary);
    lb.add_tag("strategy", cfg_.strategy.empty() ? "unknown" : cfg_.strategy);
    lb.add_tag("symbol", cfg_.symbol.empty() ? "unknown" : cfg_.symbol);
    lb.add_field_str("params", cfg_.params_json);
    lb.add_field_double("initial_equity", cfg_.initial_equity);
    lb.add_field_str("notes", cfg_.notes);
    std::lock_guard<std::mutex> lk(mu_);
    ilp_->enqueue(lb.finish(ns_from(started_at_)));
    ilp_->flush();
    return true;
}

void QuestdbStore::end(double final_equity,
                       std::size_t total_orders,
                       std::size_t total_fills,
                       std::size_t total_rejections)
{
    const auto now = std::chrono::system_clock::now();
    LineBuilder lb("runs_meta");
    lb.add_tag("run_tag", cfg_.run_tag);
    lb.add_tag("mode", cfg_.mode.empty() ? "backtest" : cfg_.mode);
    lb.add_tag("binary", cfg_.binary.empty() ? "engine_backtest" : cfg_.binary);
    lb.add_tag("strategy", cfg_.strategy.empty() ? "unknown" : cfg_.strategy);
    lb.add_tag("symbol", cfg_.symbol.empty() ? "unknown" : cfg_.symbol);
    lb.add_field_long("ended_at", ns_from(now));
    lb.add_field_double("final_equity", final_equity);
    lb.add_field_long("total_orders", static_cast<std::int64_t>(total_orders));
    lb.add_field_long("total_fills", static_cast<std::int64_t>(total_fills));
    lb.add_field_long("total_rejections",
                      static_cast<std::int64_t>(total_rejections));
    std::lock_guard<std::mutex> lk(mu_);
    ilp_->enqueue(lb.finish(ns_from(now)));
    ilp_->flush();
}

void QuestdbStore::record_order_submitted(const order_event& o,
                                          const std::string& initial_status)
{
    LineBuilder lb(table_name("orders"));
    lb.add_tag("run_tag", cfg_.run_tag);
    lb.add_tag("symbol", o.get_symbol());
    lb.add_tag("side", side_str(o.get_side()));
    lb.add_tag("type", type_str(o.get_order_type()));
    lb.add_tag("tif", tif_str(o.get_tif()));
    lb.add_tag("strategy_name",
               o.get_strategy_name().empty() ? "unknown" : o.get_strategy_name());
    lb.add_tag("initial_status", initial_status);
    lb.add_field_long("order_id", static_cast<std::int64_t>(o.get_order_id()));
    lb.add_field_double("qty", o.get_quantity());
    lb.add_field_double("price", o.get_price());
    lb.add_field_double("stop_price", o.get_stop_price());
    lb.add_field_long("opener_order_id",
                      static_cast<std::int64_t>(o.get_opener_order_id()));
    std::lock_guard<std::mutex> lk(mu_);
    ilp_->enqueue(lb.finish(ns_from(o.get_timestamp())));
}

void QuestdbStore::record_status_transition(std::uint64_t order_id,
                                            order_status old_s,
                                            order_status new_s,
                                            const std::string& reason)
{
    LineBuilder lb(table_name("order_status"));
    lb.add_tag("run_tag", cfg_.run_tag);
    lb.add_tag("old_status", status_str(old_s));
    lb.add_tag("new_status", status_str(new_s));
    lb.add_field_long("order_id", static_cast<std::int64_t>(order_id));
    lb.add_field_str("reason", reason);
    std::lock_guard<std::mutex> lk(mu_);
    ilp_->enqueue(lb.finish(now_ns()));
}

void QuestdbStore::record_fill(const fill_event& f,
                               std::uint64_t opener_order_id,
                               const std::string& strategy_name,
                               const std::string& source)
{
    LineBuilder lb(table_name("fills"));
    lb.add_tag("run_tag", cfg_.run_tag);
    lb.add_tag("symbol", f.get_symbol());
    lb.add_tag("side", side_str(f.get_side()));
    lb.add_tag("strategy_name",
               strategy_name.empty() ? "unknown" : strategy_name);
    lb.add_tag("source", source.empty() ? "unknown" : source);
    lb.add_field_long("fill_id", static_cast<std::int64_t>(f.get_fill_id()));
    lb.add_field_long("order_id", static_cast<std::int64_t>(f.get_order_id()));
    lb.add_field_long("opener_order_id",
                      static_cast<std::int64_t>(opener_order_id));
    lb.add_field_double("qty", f.get_filled_quantity());
    lb.add_field_double("price", f.get_fill_price());
    lb.add_field_double("remaining_qty", f.get_remaining_qty());
    lb.add_field_double("fee", f.get_commission());
    std::lock_guard<std::mutex> lk(mu_);
    ilp_->enqueue(lb.finish(ns_from(f.get_timestamp())));
}

void QuestdbStore::record_rejection(const order_event& o,
                                    const std::string& reason_category,
                                    const std::string& reason_detail)
{
    LineBuilder lb(table_name("rejections"));
    lb.add_tag("run_tag", cfg_.run_tag);
    lb.add_tag("symbol", o.get_symbol());
    lb.add_tag("side", side_str(o.get_side()));
    lb.add_tag("strategy_name",
               o.get_strategy_name().empty() ? "unknown" : o.get_strategy_name());
    lb.add_tag("reason",
               reason_category.empty() ? "unknown" : reason_category);
    lb.add_field_long("order_id", static_cast<std::int64_t>(o.get_order_id()));
    lb.add_field_double("qty", o.get_quantity());
    lb.add_field_double("price", o.get_price());
    lb.add_field_str("reason_detail", reason_detail);
    std::lock_guard<std::mutex> lk(mu_);
    ilp_->enqueue(lb.finish(ns_from(o.get_timestamp())));
}

void QuestdbStore::record_cancellation(std::uint64_t order_id,
                                       const std::string& symbol,
                                       const std::string& strategy_name,
                                       const std::string& reason)
{
    LineBuilder lb(table_name("cancellations"));
    lb.add_tag("run_tag", cfg_.run_tag);
    lb.add_tag("symbol", symbol.empty() ? "unknown" : symbol);
    lb.add_tag("strategy_name",
               strategy_name.empty() ? "unknown" : strategy_name);
    lb.add_tag("reason", reason.empty() ? "manual" : reason);
    lb.add_field_long("order_id", static_cast<std::int64_t>(order_id));
    std::lock_guard<std::mutex> lk(mu_);
    ilp_->enqueue(lb.finish(now_ns()));
}

void QuestdbStore::record_amendment(std::uint64_t order_id,
                                    const std::string& symbol,
                                    double old_price, double new_price,
                                    double old_qty, double new_qty,
                                    std::chrono::system_clock::time_point ts)
{
    LineBuilder lb(table_name("amendments"));
    lb.add_tag("run_tag", cfg_.run_tag);
    lb.add_tag("symbol", symbol.empty() ? "unknown" : symbol);
    lb.add_field_long("order_id", static_cast<std::int64_t>(order_id));
    lb.add_field_double("old_price", old_price);
    lb.add_field_double("new_price", new_price);
    lb.add_field_double("old_qty", old_qty);
    lb.add_field_double("new_qty", new_qty);
    std::lock_guard<std::mutex> lk(mu_);
    ilp_->enqueue(lb.finish(ns_from(ts)));
}

void QuestdbStore::tick()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (ilp_) ilp_->maybe_time_flush();
}

void QuestdbStore::flush()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (ilp_) ilp_->flush();
}

}

#endif // HAS_QUESTDB
