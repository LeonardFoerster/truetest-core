#pragma once

#include "ui/dashboard_snapshot.h"

namespace truetest::ui::desk {

// Small, cold, UI-owned capability model — replaces scattering ad-hoc
// checks such as `actions_.flatten ? ... : ...`, `snap.debug.has_debug`, or
// `research != nullptr` across individual panels with one place that
// answers "can this control/surface actually do anything right now".
//
// Deliberately NOT an alternative engine state machine: every field is
// derived straight from data the desk already holds (a snapshot, hook
// presence, whether a research view was published this frame) — never
// inferred, and never defaulted to "available"/"healthy" when unknown.
// Fields for roadmap seams that do not exist yet stay explicitly false
// (see docs/internal/imgui-desk-design.md, "Research: current vs future
// wiring") rather than being omitted, so a caller cannot mistake "field
// absent" for "assume available".
struct DeskCapabilities
{
    bool snapshot_available = false;
    bool pause_available = false;
    bool flatten_available = false;
    bool kill_available = false;
    bool debug_telemetry_available = false;
    bool questdb_active = false;
    bool research_surface_available = false;

    // Not exposed to the desk today — see RESEARCH workspace docs.
    bool research_report_available = false;
    bool research_launcher_available = false;
    bool research_resolved_config_available = false;
};

constexpr DeskCapabilities derive_desk_capabilities(bool has_snapshot,
                                                     const dashboard_snapshot* snap,
                                                     bool pause_hook_present,
                                                     bool flatten_hook_present,
                                                     bool kill_hook_present,
                                                     bool research_surface_present) noexcept
{
    DeskCapabilities caps;
    caps.snapshot_available = has_snapshot;
    caps.pause_available = pause_hook_present;
    caps.flatten_available = flatten_hook_present;
    caps.kill_available = kill_hook_present;
    caps.debug_telemetry_available = has_snapshot && snap != nullptr && snap->debug.has_debug;
    caps.questdb_active = has_snapshot && snap != nullptr && snap->health.questdb.active;
    caps.research_surface_available = research_surface_present;
    return caps;
}

} // namespace truetest::ui::desk
