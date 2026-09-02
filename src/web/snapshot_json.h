#pragma once

#include <string>

namespace truetest::ui { struct dashboard_snapshot; }

namespace truetest::web {

// Serialize a live dashboard snapshot to the SnapshotFrame JSON contract.
//
// This is a *faithful projection* of dashboard_snapshot's field names — no
// semantic translation. The frontend adapter (src/web/frontend/src/adapters)
// maps these wire fields onto the component prop shapes. `schema_version` is
// bumped on any breaking change to the wire shape.
//
// Pure function over the struct; no engine/threading deps, off the hot path.
std::string snapshot_to_json(const truetest::ui::dashboard_snapshot& s);

// v1: account/positions/health/l2/trend (initial web UI)
// v2: additive `memory` + `debug` blocks for Trading desk engine telemetry
// v3: unavailable dashboard metrics are null and carry an effective
//     availability flag; open orders expose their stop trigger; fill
//     provenance is the closed exchange/simulated/unknown set; top-level
//     generated_at_ms projects the snapshot's monotonic age onto Unix time.
inline constexpr int snapshot_schema_version = 3;

} // namespace truetest::web
