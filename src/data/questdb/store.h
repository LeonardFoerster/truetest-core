#pragma once
#ifdef HAS_QUESTDB

#include "core/event.h"
#include "execution/order_tracker.h"
#include "ilp_writer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace truetest::questdb {

struct StoreConfig
{
    std::string host = "127.0.0.1";
    std::uint16_t ilp_port = 9009;
    std::uint16_t http_port = 9000;
    std::string run_tag;

    // Metadata written to runs_meta on begin/end.
    std::string mode;       // backtest | shadow | live
    std::string binary;     // engine_backtest | engine_shadow | engine_live
    std::string strategy;
    std::string symbol;
    double initial_equity = 0.0;
    std::string params_json;
    std::string notes;
};

class QuestdbStore
{
public:
    using HttpExecFn = std::function<bool(const std::string& sql)>;

    explicit QuestdbStore(StoreConfig cfg);

    // Test-only constructor: inject a custom ILP writer + HTTP DDL fn.
    QuestdbStore(StoreConfig cfg,
                 std::unique_ptr<IlpWriter> writer,
                 HttpExecFn http_exec);

    virtual ~QuestdbStore();

    // Bootstrap: issues the 7 CREATE TABLE IF NOT EXISTS DDLs via HTTP,
    // opens the persistent ILP socket, inserts the initial runs_meta row.
    // Returns false on any step failure; caller should warn + disable
    // persistence for the session.
    bool begin();

    // Finalise: flush pending ILP lines, append a second runs_meta row
    // carrying ended_at + final equity + counters. Idempotent.
    void end(double final_equity,
             std::size_t total_orders,
             std::size_t total_fills,
             std::size_t total_rejections);

    // Capture points. virtual to enable mock subclasses in tests.
    virtual void record_order_submitted(const order_event& o,
                                        const std::string& initial_status);
    virtual void record_status_transition(std::uint64_t order_id,
                                          order_status old_s,
                                          order_status new_s,
                                          const std::string& reason = {});
    virtual void record_fill(const fill_event& f,
                             std::uint64_t opener_order_id,
                             const std::string& strategy_name,
                             const std::string& source);
    virtual void record_rejection(const order_event& o,
                                  const std::string& reason_category,
                                  const std::string& reason_detail);
    virtual void record_cancellation(std::uint64_t order_id,
                                     const std::string& symbol,
                                     const std::string& strategy_name,
                                     const std::string& reason);
    virtual void record_amendment(std::uint64_t order_id,
                                  const std::string& symbol,
                                  double old_price, double new_price,
                                  double old_qty, double new_qty,
                                  std::chrono::system_clock::time_point ts);

    virtual void tick();   // honours time-based flush cadence
    virtual void flush();  // force-flush ILP buffer

    const std::string& run_tag() const { return cfg_.run_tag; }

private:
    StoreConfig cfg_;
    std::unique_ptr<IlpWriter> ilp_;
    HttpExecFn http_exec_;
    std::chrono::system_clock::time_point started_at_{};

    // Serialises access to ilp_ across engine capture-point callers.
    mutable std::mutex mu_;

    std::string table_name(const char* suffix) const;
    static std::int64_t ns_from(std::chrono::system_clock::time_point ts);
    static std::int64_t now_ns();
    static const char* side_str(order_side s);
    static const char* type_str(order_type t);
    static const char* tif_str(time_in_force t);
    static const char* status_str(order_status s);
};

}

#endif // HAS_QUESTDB
