#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "ui/console_dashboard.h"  // for event_severity, connection_state

namespace truetest::ui {

// Formatting helpers extracted from ConsoleDashboard to keep the main
// implementation file smaller and focused on rendering + state machines.

std::string make_hline(int n, const char* middle = "─");

// Locale-free thousands separator.
std::string fmt_u64(std::uint64_t v);

std::string fmt_price_fp8(std::int64_t fp, int decimals = 2);

std::string fmt_pnl_fp4(std::int64_t fp4);

// samples==0 renders as em-dash so "0.00 bps" isn't confused with "no data yet".
std::string fmt_toxicity_bps_fp2(std::int32_t fp2, std::uint32_t samples);

std::string fmt_position_fp8(std::int64_t fp8);

std::string fmt_duration(std::chrono::seconds s);

// Counts bytes and treats UTF-8 continuations + ANSI CSI sequences as zero columns.
int visible_width_utf8(std::string_view s);

std::string pad_right(const std::string& s, int target_cols);

// Trailing clear-to-eol wipes leftovers from a previously-longer value.
std::string row(const std::string& content_with_ansi, bool color_on);

const char* severity_color(event_severity s, bool on);
const char* severity_label(event_severity s);

std::string_view state_label(connection_state s);
const char* state_color(connection_state s, bool on);

std::string clock_hhmmss(std::chrono::system_clock::time_point tp);

// Shared by ConsoleDashboard::render_tui() and TabbedDashboard::draw_status_bar()/
// compute_render_digest() - was previously duplicated inline in both files
// with the risk (already realized once - see total_ring_drops()) of the two
// copies drifting out of sync.
double spread_bps(double bid, double ask);

// Sum of all 6 ring-drop counters, in one place so a newly-added ring's
// counter can't silently miss one of the several call sites that need the
// total (tabbed_dashboard.cpp's compute_render_digest() previously summed
// only 3 of the 6 - see AGENTS.md's dashboard_snapshot note for the general
// pattern this kind of drift falls into).
std::uint64_t total_ring_drops(const streaming_stats& s);

}

