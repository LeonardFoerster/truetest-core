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

    // Phase 2 strict mode
    bool strict = false;
    std::string fallback_path;   // if non-empty, write raw ILP lines here on persistent failure

    // Phase 4: Optional retention for per-run tables (e.g. 90 for 90 days)
    // When > 0, DDLs will include "TTL <value> DAYS"
    int ttl_days = 0;
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
    // carrying ended_at + final equity + counters + Phase 4 rich analytics.
    // Idempotent.
    void end(double final_equity,
             std::size_t total_orders,
             std::size_t total_fills,
             std::size_t total_rejections,
             // Phase 4 richer campaign summary (optional, defaults to 0/NaN)
             double max_drawdown = 0.0,
             double sharpe_ratio = 0.0,
             double sortino_ratio = 0.0,
             double profit_factor = 0.0,
             double win_rate = 0.0,
             double calmar_ratio = 0.0,
             std::size_t total_trades = 0,
             std::size_t winning_trades = 0);

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
    // Sparse variant for cases without a full order_event (e.g. async transport errors).
    // Delegates to the same internal writer for schema-identical rows (fallbacks used).
    virtual void record_rejection(std::uint64_t order_id,
                                  const std::string& symbol,
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

    // Phase 2: funding settlements (cash deltas)
    virtual void record_funding(const funding_event& fe, const std::string& run_tag);

    // Phase 3: Generic richer logic/decision event capture
    virtual void record_event(const std::string& event_type,
                              const std::string& symbol,
                              const std::string& strategy_name,
                              std::uint64_t order_id,
                              const std::string& severity,
                              const std::string& message,
                              const std::string& details_json = {});

    virtual void tick();   // honours time-based flush cadence
    virtual void flush();  // force-flush ILP buffer

    const std::string& run_tag() const { return cfg_.run_tag; }

    // Minimal health snapshot for TUI / observability (Phase 0 + Phase 2).
    struct Health
    {
        bool     connected = false;
        std::size_t pending_lines = 0;
        std::size_t dropped_lines = 0;
        std::size_t fallback_lines = 0;
        std::chrono::steady_clock::time_point last_flush{};
        bool     strict_mode = false;
    };

    Health health() const;

private:
    StoreConfig cfg_;
    std::unique_ptr<IlpWriter> ilp_;
    HttpExecFn http_exec_;
    std::chrono::system_clock::time_point started_at_{};

    // Phase 2 fallback support
    std::unique_ptr<std::ofstream> fallback_file_;
    std::size_t fallback_lines_written_ = 0;

    // Serialises access to ilp_ across engine capture-point callers.
    mutable std::mutex mu_;

    std::string table_name(const char* suffix) const;
    static std::int64_t ns_from(std::chrono::system_clock::time_point ts);
    static std::int64_t now_ns();
    static const char* side_str(order_side s);
    static const char* type_str(order_type t);
    static const char* tif_str(time_in_force t);
    static const char* status_str(order_status s);

    // Internal dedup for rich + sparse rejection paths. All string work here.
    void write_rejection_line(const std::string& symbol,
                              const std::string& side,
                              const std::string& strategy,
                              std::uint64_t order_id,
                              double qty,
                              double price,
                              const std::string& reason,
                              const std::string& detail,
                              std::int64_t ts_ns);
};

}

#endif // HAS_QUESTDB
