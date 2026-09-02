#pragma once

#include <string>

struct AnalyticsReport;

namespace truetest::analytics_json {

// Serialize a completed analytics report to the stable schema-v1 results
// contract. Deterministic-run hashing parses this representation into the
// stricter canonical JSON form before computing a digest.
[[nodiscard]] std::string report_to_json(const AnalyticsReport& report);

inline constexpr int report_schema_version = 1;

} // namespace truetest::analytics_json
