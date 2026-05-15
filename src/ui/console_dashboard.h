#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace truetest::ui {

enum class output_mode
{
    auto_detect,  // tui if TTY and TERM!=dumb, else plain
    tui,
    plain,
    ndjson,
    off
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

// Split into three cachelines by write frequency so a hot-path store to
// events_total can never share a line with state/halt_flag/etc.
struct alignas(64) streaming_stats
{
    // hot path: touched every event
    alignas(64) std::atomic<std::uint64_t> events_total{0};
    std::atomic<std::uint64_t> fills_total{0};
    std::atomic<std::uint64_t> trades_total{0};
    std::atomic<std::int64_t> last_price_fp8{-1};  // -1 = no price yet
    std::atomic<std::int64_t> realized_pnl_fp4{0};
    std::atomic<std::int64_t> unrealized_pnl_fp4{0};
    std::atomic<std::int64_t> position_qty_fp8{0};
    std::atomic<std::int64_t> drawdown_fp4{0};

    // rare writes: state changes, executor, ring overflow
    alignas(64) std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(connection_state::idle)};
    std::atomic<bool>         halt_flag{false};
    std::atomic<std::uint32_t> open_orders{0};
    std::atomic<std::uint32_t> win_rate_bps{0};
    std::atomic<std::int32_t>  toxicity_bps_fp2{0};
    std::atomic<std::uint32_t> toxicity_samples{0};
    std::atomic<std::uint32_t> live_quotes{0};
    std::atomic<std::uint32_t> avg_queue_pos_bps{0};  // 0=front, 10000=back
    std::atomic<std::uint64_t> ring_drops_logging{0};
    std::atomic<std::uint64_t> ring_drops_risk{0};
    std::atomic<std::uint64_t> ring_drops_stats{0};
    std::atomic<std::uint64_t> ring_drops_observer{0};
    std::atomic<std::uint64_t> ring_drops_risk_stats{0};
    std::atomic<std::uint64_t> ring_drops_mm{0};

    alignas(64) std::atomic<std::int64_t> best_bid_fp8{-1};
    std::atomic<std::int64_t> best_ask_fp8{-1};
    std::atomic<std::uint64_t> last_event_ns{0};
    std::atomic<std::uint32_t> backfill_done{0};
    std::atomic<std::uint32_t> backfill_total{0};

    // Short halt reason published by engine::trigger_halt; the TUI banner
    // and the plain-mode renderer read this when halt_flag is set.
    // Single-writer (gated by halt_flag_.exchange) → standard seqlock:
    // odd seq = mid-write, even = published. Cap is generous for "WS lost
    // — idle 1500ms"-style strings; truncation is fine.
    static constexpr std::size_t shutdown_reason_cap = 96;
    alignas(64) std::atomic<std::uint64_t> shutdown_reason_seq{0};
    std::uint8_t shutdown_reason_len{0};
    char         shutdown_reason_buf[shutdown_reason_cap]{};
};

struct dashboard_config
{
    output_mode mode = output_mode::auto_detect;
    std::string title = "TrueTest";
    std::string target = "engine";
    std::string feed   = "";
    std::chrono::milliseconds render_interval{100};
    double risk_max_daily_loss = 0.0;
    int    risk_max_open_orders = 0;
    double risk_max_position_value = 0.0;  // 0 disables the inventory-skew cell
    double initial_balance = 0.0;

    // Pre-formatted "Threads" row ("full  4 thr / 6 cores  [0,1,2,4]"). Empty = hide.
    std::string threading_summary;
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

    // Thread-safe, non-blocking, drops on overflow.
    void push_event(event_severity sev, std::string_view msg);

    void set_state(connection_state s);
    void set_feed_label(std::string label);

    // Publishes the short halt reason ("market-data WS lost — idle 1500ms")
    // through the seqlock in stats_; renderers call shutdown_reason() to
    // read it. Called once per halt by engine::trigger_halt.
    void set_shutdown_reason(std::string_view msg);
    std::string shutdown_reason() const;

    void render_banner();

    void print_summary(std::uint64_t events,
                       std::uint64_t trades,
                       std::int64_t  elapsed_ms);

    bool is_tui() const { return resolved_mode_ == output_mode::tui; }

    // Public view of one recent event — used by the rich (ncurses)
    // dashboard, which can't read the private seqlocked storage directly.
    struct recent_event_view
    {
        std::chrono::system_clock::time_point ts{};
        event_severity sev{event_severity::info};
        std::string    msg;
    };

    // Snapshot the most recent up-to `max_count` events, oldest first.
    // Thread-safe seqlock read; entries failing the seq check are skipped.
    std::vector<recent_event_view> recent_events_snapshot(std::size_t max_count) const;

    // Pre-formatted "ev/s" rate the renderer maintains internally; the rich
    // TUI shows the same value alongside event totals.
    double rate_ema() const { return rate_ema_; }

    // Returns up to n most recent rate samples (one per render tick).
    // Used by the Overview panel's sparkline strip — the only consumer
    // for now. Lockless: render_loop is the sole writer and the panel
    // reads from the same thread (TabbedDashboard::render_loop_).
    std::vector<double> rate_tail(std::size_t n) const;

private:
    void render_loop();
    void render_tui(std::string& buf);
    void render_plain(std::string& buf);
    void render_ndjson(std::string& buf);
    static output_mode resolve_mode(output_mode requested);
    static bool stdout_is_tty();
    static bool supports_color();

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

    // Small ring of recent rate samples (1 sample per render tick).
    // Bounded so it never reallocates and hot-path stays allocation-free.
    static constexpr std::size_t rate_history_cap = 120;
    std::array<double, rate_history_cap> rate_history_{};
    std::size_t                          rate_history_count_ = 0;
    std::size_t                          rate_history_head_  = 0;

    std::chrono::steady_clock::time_point start_time_{};

    // Trivially-copyable payload; each slot rounds to 128 bytes (2 lines).
    static constexpr std::size_t recent_msg_cap = 104;
    struct event_entry
    {
        std::chrono::system_clock::time_point ts{};
        event_severity sev{event_severity::info};
        std::uint8_t   msg_len{0};
        char           msg[recent_msg_cap]{};
    };
    // Per-slot MPSC seqlock. seq = idx*2+1 (writing) / idx*2+2 (published);
    // readers validate seq before and after copying to reject torn reads.
    struct alignas(64) event_slot
    {
        std::atomic<std::uint64_t> seq{0};
        event_entry                entry{};
    };
    static constexpr std::size_t recent_cap = 128;
    std::array<event_slot, recent_cap> recent_{};
    std::atomic<std::uint64_t>         recent_head_{0};

    // Previous frame's row bytes so unchanged rows just advance the cursor
    // instead of being rewritten — this is what stops the box flickering.
    int last_row_count_{0};
    std::vector<std::string> last_rows_;

    std::vector<std::string> rows_scratch_;

    std::string row_separator_;
    std::string row_bottom_;
    std::string row_recent_header_;

    // TUI frame-skip: if the digest of every displayed atomic and the
    // uptime-second cell haven't changed, skip render+write entirely.
    std::uint64_t last_digest_{0};
    int           last_uptime_sec_{-1};
    bool          have_digest_{false};

    // Plain-mode halt-banner state.
    //   plain_halt_bell_fired_  rising-edge gate: emit '\a' once per halt
    //   plain_halt_banner_last_ wall-clock ms of last banner emission;
    //                           we rate-limit to <=1Hz so a 100 ms render
    //                           tick doesn't spam the terminal.
    bool          plain_halt_bell_fired_{false};
    std::int64_t  plain_halt_banner_last_ms_{0};
};

}
