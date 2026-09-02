#pragma once

#include "core/event.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

struct DashboardEngineDebugCounts
{
    std::size_t pending_orders = 0;
    std::size_t pending_stops = 0;
    std::size_t order_meta_size = 0;
};

// Fixed-capacity, allocation-free handoff between the sole engine producer
// and UI/web readers.  It deliberately contains values rather than references:
// once published, a reader can materialize a rich dashboard_snapshot without
// touching any event-thread-owned container.
namespace truetest::dashboard {

template <std::size_t Capacity>
struct fixed_text
{
    static_assert(Capacity > 0);

    std::array<char, Capacity> bytes{};
    std::uint16_t size = 0;

    bool assign(std::string_view value) noexcept
    {
        const auto copied = value.size() < Capacity
            ? value.size() : Capacity - 1U;
        if (copied != 0U)
            std::memcpy(bytes.data(), value.data(), copied);
        bytes[copied] = '\0';
        size = static_cast<std::uint16_t>(copied);
        return copied == value.size();
    }

    std::string_view view() const noexcept
    {
        return {bytes.data(), size};
    }
};

struct collection_state
{
    std::size_t count = 0;
    std::size_t total_count = 0;
    bool complete = true;

    bool appendable(std::size_t capacity) noexcept
    {
        ++total_count;
        if (count < capacity) return true;
        complete = false;
        return false;
    }
};

inline constexpr std::size_t kMaxSymbols = 256;
inline constexpr std::size_t kMaxPositions = kMaxSymbols;
inline constexpr std::size_t kMaxLots = 4096;
inline constexpr std::size_t kMaxOpenOrders = 4096;
inline constexpr std::size_t kMaxRecentFills = 64;
inline constexpr std::size_t kMaxBrackets = 4096;
inline constexpr std::size_t kMaxStrategies = kMaxSymbols;
inline constexpr std::size_t kDepthLevels = 10;
inline constexpr std::size_t kTrendSamples = 60;
inline constexpr std::size_t kMaxStages = 32;
inline constexpr std::size_t kTextCapacity = 128;

using text = fixed_text<kTextCapacity>;

struct mark_row
{
    text symbol;
    double value = 0.0;
};

struct position_row
{
    text symbol;
    double qty = 0.0;
    double cost_basis = 0.0;
};

struct lot_row
{
    std::uint64_t opener_order_id = 0;
    text symbol;
    text strategy_name;
    order_side side = order_side::buy;
    double qty_open = 0.0;
    double entry_price = 0.0;
    std::chrono::system_clock::time_point ts_open{};
};

struct open_order_row
{
    std::uint64_t order_id = 0;
    text symbol;
    text strategy_name;
    char side = '?';
    char type = '?';
    double qty = 0.0;
    double price = 0.0;
    double trigger_price = 0.0;
    bool trigger_price_available = false;
    std::chrono::system_clock::time_point timestamp{};
    const char* status = "";
};

struct fill_row
{
    std::chrono::system_clock::time_point timestamp{};
    text symbol;
    char side = '?';
    double qty = 0.0;
    double price = 0.0;
    double fee = 0.0;
    const char* source = "";
};

struct bracket_row
{
    std::uint64_t opener_order_id = 0;
    text strategy_name;
    text symbol;
    order_side close_side = order_side::sell;
    double qty = 0.0;
    double entry_price = 0.0;
    std::optional<double> stop_loss;
    std::optional<double> take_profit;
    std::chrono::system_clock::time_point ts_armed{};
};

struct strategy_row
{
    text name;
    double pnl = 0.0;
    std::size_t trade_count = 0;
    std::size_t win_count = 0;
    double total_win = 0.0;
    double total_loss = 0.0;
};

struct depth_level
{
    double price = 0.0;
    double size = 0.0;
};

struct book_row
{
    text symbol;
    bool venue_seeded = false;
    bool quantity_valid = true;
    std::size_t total_bid_levels = 0;
    std::size_t total_ask_levels = 0;
    std::size_t bid_count = 0;
    std::size_t ask_count = 0;
    std::array<depth_level, kDepthLevels> bids{};
    std::array<depth_level, kDepthLevels> asks{};
};

struct latency_row
{
    double avg_ns = 0.0;
    std::int64_t min_ns = 0;
    std::int64_t max_ns = 0;
    std::size_t samples = 0;
};

struct stage_row
{
    std::uint8_t index = 0;
    std::uint64_t calls = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t max_ns = 0;
};

struct DashboardProjection
{
    std::uint64_t generation = 0;
    std::uint64_t fulfilled_request_epoch = 0;
    std::chrono::steady_clock::time_point generated_at{};
    std::chrono::system_clock::time_point captured_wall_time{};

    text active_symbol;
    double last_mid = 0.0;
    double cash = 0.0;
    double initial_balance = 0.0;
    std::size_t total_fills = 0;
    std::size_t total_trades = 0;

    std::array<mark_row, kMaxSymbols> marks{};
    collection_state marks_state{};
    std::array<position_row, kMaxPositions> positions{};
    collection_state positions_state{};
    std::array<lot_row, kMaxLots> lots{};
    collection_state lots_state{};
    std::array<open_order_row, kMaxOpenOrders> open_orders{};
    collection_state open_orders_state{};
    std::array<fill_row, kMaxRecentFills> recent_fills{};
    collection_state recent_fills_state{};
    std::array<bracket_row, kMaxBrackets> brackets{};
    collection_state brackets_state{};
    std::array<strategy_row, kMaxStrategies> strategies{};
    collection_state strategies_state{};
    std::array<book_row, kMaxSymbols> books{};
    collection_state books_state{};
    std::array<text, kMaxSymbols> market_symbols{};
    collection_state market_symbols_state{};

    // `queue_diagnostics_available` distinguishes a quiescent observation
    // that found no live quotes from a runtime frame where adapter access is
    // deliberately forbidden.  Never turn "not safely observable" into an
    // authoritative zero.
    bool queue_diagnostics_available = false;
    bool queue_available = false;
    bool queue_diagnostics_complete = true;
    std::uint32_t queue_avg_bps = 0;
    std::size_t queue_submitted_with_queue = 0;
    std::size_t queue_filled_after_drain = 0;
    std::size_t queue_blocked_at_eos = 0;

    bool provider_present = false;
    bool provider_state_available = false;
    text provider_name;
    int provider_state = -1;

    std::array<stage_row, kMaxStages> stages{};
    collection_state stages_state{};

    bool analytics_available = false;
    double avg_markout_bps = 0.0;
    std::size_t markout_samples = 0;
    std::size_t total_orders = 0;
    double win_rate = 0.0;
    double sharpe = 0.0;
    double sortino = 0.0;
    double profit_factor = 0.0;
    double max_drawdown_pct = 0.0;
    latency_row latency{};
    std::array<double, kTrendSamples> equity_tail{};
    std::size_t equity_tail_count = 0;
    std::array<double, kTrendSamples> drawdown_tail{};
    std::size_t drawdown_tail_count = 0;

    bool halted = false;
    std::size_t active_order_count = 0;
    DashboardEngineDebugCounts engine_counts{};
    bool engine_counts_available = false;
    std::size_t open_orders_cache_size = 0;
    std::size_t exit_pending = 0;
    std::size_t exit_armed = 0;

    bool cache_complete = true;
    bool complete = true;
};

} // namespace truetest::dashboard
