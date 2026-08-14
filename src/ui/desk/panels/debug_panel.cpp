#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/debug_panel.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_window_names.h"
#include "ui/desk/panels/panel_helpers.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>

namespace truetest::ui::desk {

namespace {

void section(const char* label)
{
    ImGui::Spacing();
    theme::section_header(label, nullptr, theme::info());
}

double mib(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

} // namespace

void draw_debug_panel(const dashboard_snapshot& snap)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::debug)))
    {
        ImGui::End();
        return;
    }

    section("BUILD / THREADING");
    ImGui::Text("target %s   mode %s",
                snap.debug.target.empty() ? "N/A" : snap.debug.target.c_str(),
                snap.debug.mode.empty() ? "N/A" : snap.debug.mode.c_str());
    ImGui::Text("features  binance %s  questdb %s  debug %s  live-data %s",
                snap.debug.has_binance ? "on" : "off",
                snap.debug.has_questdb ? "on" : "off",
                snap.debug.has_debug ? "on" : "off",
                snap.debug.has_live_data ? "on" : "off");
    ImGui::Text("preset %s   wired worker rings %zu   pin %s   spin %s",
                snap.debug.preset.empty() ? "N/A" : snap.debug.preset.c_str(),
                snap.debug.worker_count,
                snap.debug.cpu_pin ? "on" : "off",
                snap.debug.spin_policy.empty() ? "N/A" : snap.debug.spin_policy.c_str());

    section("RINGS");
    static constexpr panels::TableColumn kRingsColumns[] = {
        {"Name"}, {"Depth"}, {"HWM"}, {"Capacity"}, {"Drops"},
    };
    if (panels::begin_table("debug_rings", kRingsColumns,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_Resizable))
    {
        for (const auto& ring : snap.debug.rings)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ring.name ? ring.name : "N/A");
            if (ring.capacity == 0)
            {
                ImGui::TableNextColumn();
                ImGui::TextColored(theme::tx_faint(), "N/A");
                ImGui::TableNextColumn();
                ImGui::TextColored(theme::tx_faint(), "N/A");
                ImGui::TableNextColumn();
                ImGui::TextColored(theme::tx_faint(), "not exposed / inactive");
                ImGui::TableNextColumn();
                ImGui::TextColored(theme::tx_faint(), "N/A");
                continue;
            }
            ImGui::TableNextColumn();
            if (ring.size > ring.capacity)
                ImGui::TextColored(theme::warn(), "%zu (inconsistent sample)", ring.size);
            else
                ImGui::Text("%zu", ring.size);
            ImGui::TableNextColumn();
            const auto bounded_hwm = std::min(ring.hwm, ring.capacity);
            ImGui::Text("%zu (%.1f%%)", ring.hwm,
                        100.0 * static_cast<double>(bounded_hwm) / ring.capacity);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", ring.capacity);
            ImGui::TableNextColumn();
            ImGui::TextColored(ring.drops ? theme::down() : theme::tx_mid(),
                               "%llu", static_cast<unsigned long long>(ring.drops));
        }
        ImGui::EndTable();
    }

    section("POOLS");
    static constexpr panels::TableColumn kPoolsColumns[] = {
        {"Name"}, {"Blocks"}, {"In use"}, {"Capacity"}, {"Fill"}, {"Runtime grows"},
    };
    if (panels::begin_table("debug_pools", kPoolsColumns,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_Resizable))
    {
        for (const auto& pool : snap.debug.pools)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pool.name ? pool.name : "N/A");
            ImGui::TableNextColumn();
            ImGui::Text("%zu", pool.blocks);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", pool.in_use);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", pool.capacity);
            ImGui::TableNextColumn();
            const double fill = pool.capacity
                ? 100.0 * static_cast<double>(pool.in_use) / pool.capacity
                : 0.0;
            ImGui::Text("%.1f%%", fill);
            ImGui::TableNextColumn();
            ImGui::TextColored(pool.grow_count ? theme::down() : theme::tx_mid(),
                               "%zu", pool.grow_count);
        }
        ImGui::EndTable();
    }

    section("QUEUE / ENGINE STATE");
    const bool queue_observed = snap.queue.avg_bps != 0
        || snap.queue.submitted_with_queue != 0
        || snap.queue.filled_after_drain != 0
        || snap.queue.blocked_at_eos != 0;
    if (queue_observed)
    {
        ImGui::Text("queue position %.1f%% toward back   submitted %zu   drained fills %zu   blocked EOS %zu",
                    static_cast<double>(snap.queue.avg_bps) / 100.0,
                    snap.queue.submitted_with_queue,
                    snap.queue.filled_after_drain,
                    snap.queue.blocked_at_eos);
    }
    else
    {
        ImGui::TextColored(theme::tx_faint(),
                           "Queue observations N/A (model may be inactive)");
    }
    ImGui::Text("open-order cache %zu   armed brackets %zu",
                snap.debug.open_orders_cache, snap.debug.armed_brackets);
    ImGui::Text("exit pending %zu   exit armed %zu",
                snap.debug.exit_pending, snap.debug.exit_armed);
    ImGui::TextColored(theme::tx_faint(),
                       "Pending order/stop, id, metadata and venue-map counters are not exposed.");

    section("SUBSYSTEM ERRORS");
    bool reported_error = false;
    for (const auto& error : snap.debug.errors)
    {
        if (error.msg.empty())
            continue;
        reported_error = true;
        ImGui::TextColored(theme::down(), "%s: %s",
                           error.name ? error.name : "subsystem", error.msg.c_str());
    }
    if (!reported_error)
        ImGui::TextColored(theme::tx_faint(), "No subsystem error text reported (not a health assertion)");

    section("STAGE TIMINGS");
    static constexpr panels::TableColumn kStagesColumns[] = {
        {"Stage"}, {"Calls"}, {"Avg ns"}, {"Min ns"}, {"Max ns"},
    };
    if (snap.debug.stages.empty())
    {
        ImGui::TextColored(theme::tx_faint(),
                           snap.debug.has_debug ? "No stage samples yet" : "N/A — build with ENABLE_DEBUG");
    }
    else if (panels::begin_table("debug_stages", kStagesColumns,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        for (const auto& stage : snap.debug.stages)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(stage.name ? stage.name : "N/A");
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stage.calls));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stage.avg_ns));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stage.min_ns));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stage.max_ns));
        }
        ImGui::EndTable();
    }

    section("MEMORY");
    if (snap.memory.available)
    {
        ImGui::Text("RSS %.1f MiB   VM %.1f MiB   peak RSS %.1f MiB   data-segment estimate %.1f MiB",
                    mib(snap.memory.rss_bytes), mib(snap.memory.vm_bytes),
                    mib(snap.memory.peak_rss_bytes), mib(snap.memory.heap_bytes));
    }
    else
    {
        ImGui::TextColored(theme::tx_faint(), "Process memory N/A on this platform");
    }
    ImGui::Text("pool estimate %.1f MiB   ring payload estimate %.1f MiB",
                mib(snap.memory.pool_bytes_total), mib(snap.memory.ring_bytes_total));

    static constexpr panels::TableColumn kMemoryPoolsColumns[] = {
        {"Pool"}, {"Reserved MiB"}, {"In use"}, {"Capacity slots"},
    };
    if (!snap.memory.pools.empty()
        && panels::begin_table("debug_memory_pools", kMemoryPoolsColumns,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        for (const auto& pool : snap.memory.pools)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(pool.name ? pool.name : "N/A");
            ImGui::TableNextColumn(); ImGui::Text("%.2f", mib(pool.bytes));
            ImGui::TableNextColumn(); ImGui::Text("%zu", pool.in_use);
            ImGui::TableNextColumn(); ImGui::Text("%zu", pool.capacity_slots);
        }
        ImGui::EndTable();
    }

    static constexpr panels::TableColumn kMemoryRingsColumns[] = {
        {"Ring"}, {"Capacity"}, {"Element bytes"}, {"Payload MiB"},
    };
    if (!snap.memory.rings.empty()
        && panels::begin_table("debug_memory_rings", kMemoryRingsColumns,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        for (const auto& ring : snap.memory.rings)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ring.name ? ring.name : "N/A");
            ImGui::TableNextColumn(); ImGui::Text("%zu", ring.capacity);
            ImGui::TableNextColumn(); ImGui::Text("%zu", ring.element_bytes);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", mib(ring.bytes));
        }
        ImGui::EndTable();
    }

    if (!snap.memory.other_breakdown.empty())
    {
        ImGui::TextColored(theme::tx_faint(),
                           "Mapped virtual ranges below are not an RSS decomposition.");
        static constexpr panels::TableColumn kMemoryMapsColumns[] = {
            {"Mapping class"}, {"Virtual MiB"},
        };
        if (panels::begin_table("debug_memory_maps", kMemoryMapsColumns,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            for (const auto& segment : snap.memory.other_breakdown)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(segment.name ? segment.name : "N/A");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", mib(segment.bytes));
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

} // namespace truetest::ui::desk

#endif // HAS_IMGUI_DESK
