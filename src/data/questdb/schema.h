#pragma once
#ifdef HAS_QUESTDB

#include <string>
#include <vector>

namespace truetest::questdb::schema {

// DDL for the shared runs_meta table. Idempotent (IF NOT EXISTS).
std::string runs_meta_ddl();

// Per-run DDL statements (Phase 3 adds events, Phase 4 adds optional TTL).
std::vector<std::string> per_run_ddls(const std::string& run_tag, int ttl_days = 0);

// Convenience — all 8 statements in the order they should be issued.
std::vector<std::string> all_ddls(const std::string& run_tag, int ttl_days = 0);

inline constexpr const char* kTableOrders        = "orders";
inline constexpr const char* kTableOrderStatus   = "order_status";
inline constexpr const char* kTableFills         = "fills";
inline constexpr const char* kTableRejections    = "rejections";
inline constexpr const char* kTableCancellations = "cancellations";
inline constexpr const char* kTableAmendments    = "amendments";
inline constexpr const char* kTableFunding       = "funding";   // Phase 2: funding settlements (cash deltas)
inline constexpr const char* kTableEvents        = "events";    // Phase 3: richer logic/decision capture

}

#endif // HAS_QUESTDB
