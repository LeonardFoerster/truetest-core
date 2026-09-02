#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/health_strip.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/status_badge.h"

#include "imgui.h"

namespace truetest::ui::desk::panels {
namespace {

std::uint64_t total_ring_drops(const dashboard_snapshot::health_view& health)
{
    return health.ring_drops_logging + health.ring_drops_risk + health.ring_drops_stats +
           health.ring_drops_observer + health.ring_drops_risk_stats + health.ring_drops_mm;
}

const char* provider_status(const dashboard_snapshot& snapshot)
{
    if (!snapshot.health.provider_present) return "PROVIDER UNKNOWN";
    switch (snapshot.health.provider_state) {
    case 2:
        return "PROVIDER CONNECTED";
    case 1:
        return "PROVIDER OPENING";
    case 3:
        return "PROVIDER ERROR";
    default:
        return "PROVIDER CLOSED";
    }
}

theme::Tone provider_tone(const dashboard_snapshot& snapshot)
{
    if (!snapshot.health.provider_present) return theme::Tone::warning;
    switch (snapshot.health.provider_state) {
    case 2:
        return theme::Tone::accent;
    case 1:
        return theme::Tone::warning;
    case 3:
        return theme::Tone::danger;
    default:
        return theme::Tone::muted;
    }
}

}  // namespace

void draw_health_strip(const dashboard_snapshot* snapshot, const CommandCenterViewModel* view,
                       bool telemetry_available, bool rate_available)
{
    ImGui::BeginChild("command_center_health", {0.0f, 31.0f}, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!snapshot) {
        widgets::status_badge("SNAPSHOT UNAVAILABLE", theme::Tone::warning);
        ImGui::SameLine();
        ImGui::TextColored(theme::text_faint(), "Awaiting a coherent engine snapshot");
        ImGui::EndChild();
        return;
    }

    widgets::status_badge(provider_status(*snapshot), provider_tone(*snapshot));
    ImGui::SameLine();
    const std::string update_age =
        view && view->snapshot_age_ms
            ? format_duration(std::optional<std::int64_t>{*view->snapshot_age_ms / 1'000})
            : "UNKNOWN";
    ImGui::TextColored(view && view->snapshot_stale ? theme::negative() : theme::text_muted(),
                       "UPDATE AGE %s", update_age.c_str());
    ImGui::SameLine();
    if (telemetry_available && rate_available)
        ImGui::TextColored(theme::text_muted(), "EVENT RATE %.1f/s",
                           snapshot->health.rate_ev_per_sec);
    else
        ImGui::TextColored(theme::text_faint(), "EVENT RATE UNAVAILABLE");
    ImGui::SameLine();
    if (snapshot->health.tick_to_trade_samples > 0)
        ImGui::TextColored(theme::text_muted(), "TICK→TRADE %.0fµs",
                           snapshot->health.avg_tick_to_trade_us);
    else
        ImGui::TextColored(theme::text_faint(), "TICK→TRADE UNAVAILABLE");
    ImGui::SameLine();
    const auto drops = total_ring_drops(snapshot->health);
    ImGui::TextColored(drops > 0 ? theme::warning() : theme::text_muted(), "RING DROPS %llu",
                       static_cast<unsigned long long>(drops));
    ImGui::SameLine();
    ImGui::TextColored(theme::text_faint(), "LAST EVENT AGE UNAVAILABLE");
    ImGui::EndChild();
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
