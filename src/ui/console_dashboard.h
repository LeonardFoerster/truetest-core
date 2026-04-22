#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace truetest::ui {

enum class output_mode
{
    auto_detect,  // pick tui if stdout is a TTY and TERM!=dumb; else plain
    tui,          // ANSI cursor dashboard
    plain,        // single line per update, newline terminated
    ndjson,       // one JSON object per update
    off           // no dashboard output at all
};

enum class connection_state : std::uint8_t
{
    idle = 0,
    backfill,
    waiting,
    live,
    reconnecting,
    halted,
    closed
};

enum class event_severity : std::uint8_t
{
    info = 0,
    notice,
    warn,
    error
};

// Snapshot struct — hot-path writers only touch atomic<...> members.
// The display thread reads them with relaxed loads every render tick.
// Padded loosely to keep frequently-co-written counters on one line and
// display-thread-only fields elsewhere; padding is not claimed to be
// perfect, but it's enough to avoid obvious false sharing between the
// event-loop thread and the display thread.
struct alignas(64) streaming_stats
{
    std::atomic<std::uint64_t> events_total{0};
    std::atomic<std::uint64_t> fills_total{0};
    std::atomic<std::uint64_t> trades_total{0};

    // Fixed-point price × 1e8 so the hot path stores an integer atomically
    // instead of needing a lock or a double atomic (which is ABI-fiddly).
    // -1 means "no price yet".
    std::atomic<std::int64_t> last_price_fp8{-1};
    std::atomic<std::int64_t> best_bid_fp8{-1};
    std::atomic<std::int64_t> best_ask_fp8{-1};
    std::atomic<std::uint64_t> last_event_ns{0};

    std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(connection_state::idle)};
    std::atomic<bool>         halt_flag{false};

    std::atomic<std::uint32_t> open_orders{0};
    std::atomic<std::uint32_t> backfill_done{0};
    std::atomic<std::uint32_t> backfill_total{0};

    std::atomic<std::uint64_t> ring_drops_logging{0};
    std::atomic<std::uint64_t> ring_drops_risk{0};
    std::atomic<std::uint64_t> ring_drops_stats{0};
    std::atomic<std::uint64_t> ring_drops_observer{0};
    std::atomic<std::uint64_t> ring_drops_risk_stats{0};
    std::atomic<std::uint64_t> ring_drops_mm{0};

    std::atomic<std::int64_t> realized_pnl_fp4{0};  // pnl × 1e4
    std::atomic<std::int64_t> drawdown_fp4{0};      // drawdown × 1e4
    std::atomic<std::uint32_t> win_rate_bps{0};     // win rate × 1e4 (basis points / 1)
};

struct dashboard_config
{
    output_mode mode = output_mode::auto_detect;
    std::string title = "TrueTest";
    std::string target = "engine";
    std::string feed   = "";            // e.g. "binance/btcusdt@trade"
    std::chrono::milliseconds render_interval{100};
    double risk_max_daily_loss = 0.0;
    int    risk_max_open_orders = 0;
    double initial_balance = 0.0;
};

class ConsoleDashboard
{
public:
    explicit ConsoleDashboard(dashboard_config cfg);
    ~ConsoleDashboard();

    ConsoleDashboard(const ConsoleDashboard&) = delete;
    ConsoleDashboard& operator=(const ConsoleDashboard&) = delete;

    void start();
    void stop();

    streaming_stats& stats() { return stats_; }
    const streaming_stats& stats() const { return stats_; }

    // Fire-and-forget event entry for the "recent" pane. Non-blocking, drops
    // on overflow. Safe from any thread, including the hot event loop.
    void push_event(event_severity sev, std::string_view msg);

    void set_state(connection_state s);
    void set_feed_label(std::string label);

    // Drawn once at start, then replaced each frame by the live dashboard.
    void render_banner();

    // Final summary printed after stop(). Matches the old
    // "Streaming complete:" line behavior for non-TUI modes.
    void print_summary(std::uint64_t events,
                       std::uint64_t trades,
                       std::int64_t  elapsed_ms);

    bool is_tui() const { return resolved_mode_ == output_mode::tui; }

private:
    void render_loop();
    void render_tui(std::string& buf);
    void render_plain(std::string& buf);
    void render_ndjson(std::string& buf);
    static output_mode resolve_mode(output_mode requested);
    static bool stdout_is_tty();
    static bool supports_color();

    // Compute ema events/sec from two successive samples. Display-thread only.
    void update_rate_ema(std::uint64_t now_events,
                         std::chrono::steady_clock::time_point now);

    dashboard_config cfg_;
    output_mode      resolved_mode_{output_mode::plain};
    streaming_stats  stats_;

    std::thread      thread_;
    std::atomic<bool> running_{false};

    std::chrono::steady_clock::time_point last_sample_time_{};
    std::uint64_t                         last_sample_events_{0};
    double                                rate_ema_{0.0};

    std::chrono::steady_clock::time_point start_time_{};

    struct event_entry
    {
        std::chrono::system_clock::time_point ts{};
        event_severity sev{event_severity::info};
        std::string    msg;
    };
    static constexpr std::size_t recent_cap = 128;
    std::array<event_entry, recent_cap> recent_{};
    std::atomic<std::uint64_t>          recent_head_{0};  // monotonically increasing
    std::mutex                          recent_mu_;       // guards recent_[] writes

    // TUI paint state — track previous frame's rendered row bytes so we can
    // diff and only re-emit rows whose content actually changed. Unchanged
    // rows just advance the cursor; the border and steady cells stop
    // flickering on every tick.
    int last_row_count_{0};
    std::vector<std::string> last_rows_;
};

} // namespace truetest::ui
