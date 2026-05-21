#pragma once
#ifdef HAS_QUESTDB

#include <string>
#include <vector>

namespace truetest::questdb::schema {

// DDL for the shared runs_meta table. Idempotent (IF NOT EXISTS).
std::string runs_meta_ddl();

// Six per-run DDL statements, prefixed with run_tag.
std::vector<std::string> per_run_ddls(const std::string& run_tag);

// Convenience — all 7 statements in the order they should be issued.
std::vector<std::string> all_ddls(const std::string& run_tag);

inline constexpr const char* kTableOrders        = "orders";
inline constexpr const char* kTableOrderStatus   = "order_status";
inline constexpr const char* kTableFills         = "fills";
inline constexpr const char* kTableRejections    = "rejections";
inline constexpr const char* kTableCancellations = "cancellations";
inline constexpr const char* kTableAmendments    = "amendments";
inline constexpr const char* kTableFunding       = "funding";   // Phase 2: funding settlements (cash deltas)

}

#endif // HAS_QUESTDB
