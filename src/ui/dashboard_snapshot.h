#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace truetest::ui {

// Engine-side snapshot consumed by the rich TUI panels. Filled under
// engine lock by engine::snapshot_dashboard() so the render thread can
// read a coherent view at the 100 ms tick.
struct dashboard_snapshot
{
    struct position_row
    {
        std::string symbol;
        double      qty           = 0.0;
        double      avg_entry     = 0.0;
        double      mark          = 0.0;
        double      unrealized    = 0.0;
    };

    struct lot_row
    {
        std::uint64_t opener_order_id = 0;
        std::string   symbol;
        std::string   strategy_name;
        char          side          = '?';   // 'L' or 'S'
        double        qty_open      = 0.0;
        double        entry_price   = 0.0;
        std::int64_t  age_seconds   = 0;
    };

    struct open_order_row
    {
        std::uint64_t order_id      = 0;
        std::string   symbol;
        std::string   strategy_name;
        char          side          = '?';   // 'B' or 'S'
        char          type          = '?';   // 'M' / 'L' / 'S' / 's'
        double        qty           = 0.0;
        double        price         = 0.0;
        std::int64_t  age_seconds   = 0;
        const char*   status        = "";
    };

    struct fill_row
    {
        std::chrono::system_clock::time_point ts{};
        std::string   symbol;
        char          side          = '?';
        double        qty           = 0.0;
        double        price         = 0.0;
        double        fee           = 0.0;
        const char*   source        = "";
    };

    struct bracket_row
    {
        std::uint64_t opener_order_id = 0;
        std::string   strategy_name;
        std::string   symbol;
        char          side          = '?';   // 'L' (long entry → SELL closer) / 'S'
        double        qty           = 0.0;
        double        entry_price   = 0.0;
        std::optional<double> stop_loss;
        std::optional<double> take_profit;
        double        mark          = 0.0;   // current mark for distance calc
        bool          venue_managed = false; // true if exchange has resting orders
        std::string   venue_list_id;         // OCO list id, empty if engine-only
        std::int64_t  age_seconds   = 0;
    };

    struct strategy_row
    {
        std::string   name;
        double        pnl           = 0.0;
        std::size_t   trade_count   = 0;
        std::size_t   win_count     = 0;
        double        win_rate      = 0.0;
        double        profit_factor = 0.0;
        double        total_win     = 0.0;
        double        total_loss    = 0.0;
        std::size_t   open_lots     = 0;   // active lots from portfolio
        std::size_t   armed_brackets = 0;  // active brackets in ExitManager
    };

    struct health_view
    {
        double      avg_tick_to_trade_us = 0.0;
        double      min_tick_to_trade_us = 0.0;
        double      max_tick_to_trade_us = 0.0;
        std::size_t tick_to_trade_samples = 0;

        std::size_t events_total = 0;
        std::size_t fills_total  = 0;
        std::size_t orders_total = 0;
        std::size_t trades_total = 0;

        std::int64_t age_last_event_ms = 0;   // wall-clock since last event
        std::int64_t age_last_fill_ms  = 0;
        std::int64_t age_last_bar_ms   = 0;

        std::uint64_t ring_drops_logging = 0;
        std::uint64_t ring_drops_risk    = 0;
        std::uint64_t ring_drops_stats   = 0;
        std::uint64_t ring_drops_observer = 0;
        std::uint64_t ring_drops_risk_stats = 0;
        std::uint64_t ring_drops_mm     = 0;

        bool   provider_present = false;
        std::string provider_name;
        int    provider_state = 0;            // matches connection_state enum

        double rate_ev_per_sec = 0.0;          // EMA from ConsoleDashboard
    };
    health_view health;

    // Engine introspection for the Debug tab. Sourced entirely from
    // existing accessors and atomics — no new hot-path tracking. Each
    // section maps directly to the panel's section layout.
    struct ring_stat
    {
        const char*  name = "";
        std::size_t  size = 0;          // current depth
        std::size_t  hwm = 0;           // high-watermark across run
        std::size_t  capacity = 0;      // 0 = ring not running this preset
        std::uint64_t drops = 0;
    };
    struct pool_stat
    {
        const char*  name = "";
        std::size_t  blocks = 0;        // ObjectPool::block_count()
        std::size_t  block_size = 0;    // slots per block
        std::size_t  capacity = 0;      // blocks * block_size
    };
    struct subsys_error
    {
        const char* name = "";
        std::string msg;                // empty = no error
    };

    struct debug_view
    {
        // Build / target.
        std::string  target;             // "backtest" / "shadow" / "live"
        std::string  mode;               // engine_config.mode as string
        bool         has_binance   = false;
        bool         has_questdb   = false;
        bool         has_debug     = false;
        bool         has_live_data = false;

        // Threading.
        std::string  preset;             // "inline" / "light" / ... / "extended"
        std::size_t  worker_count  = 0;
        bool         cpu_pin       = false;
        std::string  spin_policy;        // "spin" / "yield" / "adaptive"

        // Rings.
        std::vector<ring_stat> rings;

        // Pools.
        std::vector<pool_stat> pools;

        // Engine state.
        std::uint64_t event_count        = 0;
        std::uint64_t next_order_id      = 0;
        std::uint64_t next_fill_id       = 0;
        std::size_t   pending_orders     = 0;
        std::size_t   pending_stops      = 0;
        std::size_t   open_orders_cache  = 0;
        std::size_t   order_meta_size    = 0;
        std::size_t   armed_brackets     = 0;
        std::size_t   handles_size       = 0;

        // Subsystem last-error strings.
        std::vector<subsys_error> errors;

        // ExitManager venue side.
        std::size_t exit_pending          = 0;
        std::size_t exit_armed            = 0;
        std::size_t exit_exchange_to_leg  = 0;

        // Stage timings — populated only on HAS_DEBUG builds; vector
        // stays empty otherwise so the panel can branch on .empty().
        struct stage_row
        {
            const char*  name = "";
            std::uint64_t calls = 0;
            std::uint64_t avg_ns = 0;
            std::uint64_t min_ns = 0;
            std::uint64_t max_ns = 0;
        };
        std::vector<stage_row> stages;
    };
    debug_view debug;

    // Memory composition for the Debug tab's map view. Sourced from
    // /proc/self/* (Linux only — `available=false` elsewhere) plus
    // computed pool/ring footprints (always available since they're
    // derived from in-process metadata).
    struct mem_pool_row
    {
        const char*   name = "";
        std::size_t   blocks = 0;
        std::size_t   slot_size = 0;
        std::uint64_t bytes = 0;
        std::size_t   in_use = 0;
        std::size_t   capacity_slots = 0;  // blocks * BlockSize
    };
    struct mem_ring_row
    {
        const char*   name = "";
        std::size_t   capacity = 0;     // 0 = ring not running this preset
        std::size_t   element_bytes = 0;
        std::uint64_t bytes = 0;
    };
    struct mem_other_seg
    {
        const char*   name = "";        // "heap" / "stacks" / ".so code" / "anonymous" / "other"
        std::uint64_t bytes = 0;
    };
    struct memory_view
    {
        bool          available = false;     // /proc parseable

        std::uint64_t rss_bytes      = 0;
        std::uint64_t vm_bytes       = 0;
        std::uint64_t peak_rss_bytes = 0;
        std::uint64_t heap_bytes     = 0;    // /proc/self/statm data segment

        // Computed in-process footprints — always populated.
        std::uint64_t pool_bytes_total = 0;
        std::uint64_t ring_bytes_total = 0;

        std::vector<mem_pool_row>   pools;
        std::vector<mem_ring_row>   rings;
        // Decomposition of the "other" RSS slice from /proc/self/maps.
        // Empty when /proc/self/maps wasn't parseable.
        std::vector<mem_other_seg>  other_breakdown;
    };
    memory_view memory;

    // L2 depth ladder for the active symbol. Sourced from the engine's
    // orderbook_registry. The `source` field disambiguates real venue
    // depth (subscribed via the provider's L2 stream — Binance
    // --depth-stream) from MM-seeded synthetic depth (paper liquidity
    // the LocalBookAdapter / HybridExecutor matches against).
    enum class l2_source { none, synthetic, venue };

    struct l2_level
    {
        double price = 0.0;
        double size  = 0.0;
        double cum   = 0.0;   // cumulative size from BBO outward
    };
    struct l2_view
    {
        std::string  symbol;
        l2_source    source = l2_source::none;
        std::size_t  total_bid_levels = 0;     // before truncation
        std::size_t  total_ask_levels = 0;
        std::vector<l2_level> bids;            // top-N, high → low
        std::vector<l2_level> asks;            // top-N, low → high
        double       best_bid     = 0.0;
        double       best_ask     = 0.0;
        double       mid          = 0.0;
        double       spread_bps   = 0.0;
        double       microprice   = 0.0;       // size-weighted mid
        double       imbalance    = 0.0;       // (bid_sz - ask_sz) / total, top-N
        double       cum_bid_size = 0.0;       // sum across bids[]
        double       cum_ask_size = 0.0;
    };
    l2_view l2;

    struct risk_view
    {
        bool   halted               = false;
        double daily_loss           = 0.0;
        double daily_loss_limit     = 0.0;
        double max_drawdown_pct     = 0.0;
        double max_drawdown_limit   = 0.0;
        double exposure             = 0.0;
        double exposure_limit       = 0.0;
        std::size_t open_orders     = 0;
        std::size_t open_orders_limit = 0;
    };

    struct perf_view
    {
        std::size_t total_orders    = 0;
        std::size_t total_fills     = 0;
        std::size_t total_trades    = 0;
        double      win_rate        = 0.0;
        double      sharpe          = 0.0;
        double      sortino         = 0.0;
        double      profit_factor   = 0.0;
        double      avg_markout_bps = 0.0;
        std::size_t markout_samples = 0;
    };

    double cash             = 0.0;
    double equity           = 0.0;
    double initial_balance  = 0.0;
    double realized_pnl     = 0.0;
    double unrealized_pnl   = 0.0;

    std::vector<position_row>    positions;
    std::vector<lot_row>         lots;
    std::vector<open_order_row>  open_orders;
    std::vector<fill_row>        recent_fills;   // newest first
    std::vector<bracket_row>     brackets;       // armed brackets, any order
    std::vector<strategy_row>    strategies;     // per-strategy attribution

    risk_view risk;
    perf_view perf;

    // Compact time-series tails the Overview panel renders as sparklines
    // to fill the lower half. Sized to fit the panel width (~60 cells)
    // and refreshed under the same engine snapshot lock as everything
    // else, so values stay coherent with positions/risk/perf above.
    struct trend_view
    {
        std::vector<double> equity_tail;     // last N equity points
        std::vector<double> drawdown_tail;   // last N drawdown % (≥ 0)
        std::vector<double> rate_tail;       // last N event-rate samples
        double equity_now        = 0.0;      // most recent equity
        double equity_change_pct = 0.0;      // vs initial_balance
        double drawdown_now_pct  = 0.0;      // most recent dd
        double rate_now          = 0.0;      // most recent rate
    };
    trend_view trend;
};

}
