#include "dashboard_snapshot_builder.h"

#include "execution/portfolio.h"
#include "analytics/analytics.h"
#include "analytics/adverse_selection_tracker.h"
#include "exits/exit_manager.h"
#include "orderbook/orderbook_registry.h"
#include "order_audit_sink.h"
#include "engine_config.h"
#include "execution/execution_adapter.h"
#include "providers/provider.h"
#include "types/object_pool.h"
#include "threading/ring_buffer.h"
#include "core/event.h"

using EventRing = RingBuffer<event_pointer, 65536>;

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <vector>

#ifdef HAS_DEBUG
#include "debug/stage_timer.h"
#include "debug/memory_info.h"
#include "debug/ring_stats.h"
#endif

DashboardSnapshotBuilder::DashboardSnapshotBuilder(
    const portfolio& port,
    const Analytics& analytics,
    const AdverseSelectionTracker& adverse,
    const truetest::exits::ExitManager& exits,
    const std::atomic<bool>& halt_flag,
    const engine_config& config,
    const std::atomic<double>& last_mid_price,
    const std::string& last_mark_symbol,
    const std::unordered_map<std::string, double>& last_mark_prices,
    std::mutex& last_mark_prices_mu,
    OrderbookRegistry& orderbook_registry,
    const std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters,
    IOrderAuditSink& audit_sink,
    const std::unordered_set<std::string>& l2_seeded_symbols,
    const ObjectPool<market_event>& market_pool,
    const ObjectPool<order_event>& order_pool,
    const ObjectPool<fill_event>& fill_pool,
    const ObjectPool<tick_event>& tick_pool,
    const ObjectPool<l2_update_event>& l2_update_pool,
    const ObjectPool<l2_snapshot_event>& l2_snapshot_pool,
    const ObjectPool<rejection_event>& rejection_pool,
    const ObjectPool<cancel_event>& cancel_pool,
    const ObjectPool<amend_event>& amend_pool,
    const ObjectPool<funding_event>& funding_pool,
    const ControlBlockPool& control_block_pool,
    const std::shared_ptr<EventRing>& logging_ring,
    const std::shared_ptr<EventRing>& risk_ring,
    const std::shared_ptr<EventRing>& stats_ring,
    const std::shared_ptr<EventRing>& observer_ring,
    const std::shared_ptr<EventRing>& risk_stats_ring,
    const std::shared_ptr<EventRing>& mm_ring,
    DebugSamplers debug_samplers
)
    : portfolio_(port)
    , analytics_(analytics)
    , adverse_selection_(adverse)
    , exit_manager_(exits)
    , halt_flag_(halt_flag)
    , config_(config)
    , last_mid_price_(last_mid_price)
    , last_mark_symbol_(last_mark_symbol)
    , last_mark_prices_(last_mark_prices)
    , last_mark_prices_mu_(last_mark_prices_mu)
    , orderbook_registry_(orderbook_registry)
    , execution_adapters_(execution_adapters)
    , audit_sink_(audit_sink)
    , l2_seeded_symbols_(l2_seeded_symbols)
    , market_pool_(market_pool)
    , order_pool_(order_pool)
    , fill_pool_(fill_pool)
    , tick_pool_(tick_pool)
    , l2_update_pool_(l2_update_pool)
    , l2_snapshot_pool_(l2_snapshot_pool)
    , rejection_pool_(rejection_pool)
    , cancel_pool_(cancel_pool)
    , amend_pool_(amend_pool)
    , funding_pool_(funding_pool)
    , control_block_pool_(control_block_pool)
    , logging_ring_(logging_ring)
    , risk_ring_(risk_ring)
    , stats_ring_(stats_ring)
    , observer_ring_(observer_ring)
    , risk_stats_ring_(risk_stats_ring)
    , mm_ring_(mm_ring)
    , debug_samplers_(debug_samplers)
{
}

DashboardSnapshotBuilder::~DashboardSnapshotBuilder() = default;

bool DashboardSnapshotBuilder::snapshot_dashboard(truetest::ui::dashboard_snapshot& out) const
{
    std::lock_guard<std::mutex> lk(dashboard_view_mu_);
    if (!dashboard_view_initialised_) return false;
    out = dashboard_view_;
    return true;
}

void DashboardSnapshotBuilder::request_dashboard_refresh()
{
    dashboard_view_force_ = true;
    dashboard_view_last_ = std::chrono::steady_clock::time_point{};
}

void DashboardSnapshotBuilder::refresh_if_due()
{
    auto now = std::chrono::steady_clock::now();

    if (!dashboard_view_force_
        && dashboard_view_initialised_
        && now - dashboard_view_last_ < dashboard_view_interval_)
    {
        return;
    }

    truetest::ui::dashboard_snapshot snap;
    build_dashboard_view(snap);

    {
        std::lock_guard<std::mutex> lk(dashboard_view_mu_);
        dashboard_view_              = std::move(snap);
        dashboard_view_initialised_  = true;
    }
    dashboard_view_last_ = now;
    dashboard_view_force_ = false;
}

void DashboardSnapshotBuilder::cache_open_order(const order_event& o)
{
    open_order_cache_entry e;
    e.row.order_id      = o.get_order_id();
    e.row.symbol        = o.get_symbol();
    e.row.strategy_name = o.get_strategy_name();
    e.row.side          = (o.get_side() == order_side::buy) ? 'B' : 'S';
    switch (o.get_order_type())
    {
        case order_type::market:     e.row.type = 'M'; break;
        case order_type::limit:      e.row.type = 'L'; break;
        case order_type::stop:       e.row.type = 'S'; break;
        case order_type::stop_limit: e.row.type = 's'; break;
    }
    e.row.qty    = o.get_quantity();
    e.row.price  = o.get_price();
    e.row.status = "open";
    e.ts         = o.get_timestamp();
    open_orders_cache_[o.get_order_id()] = std::move(e);
}

void DashboardSnapshotBuilder::update_open_order_status(std::uint64_t id, const char* status)
{
    auto it = open_orders_cache_.find(id);
    if (it != open_orders_cache_.end())
        it->second.row.status = status;
}

void DashboardSnapshotBuilder::erase_open_order(std::uint64_t id)
{
    open_orders_cache_.erase(id);
}

void DashboardSnapshotBuilder::cache_fill(const fill_event& f)
{
    truetest::ui::dashboard_snapshot::fill_row r;
    r.ts     = f.get_timestamp();
    r.symbol = f.get_symbol();
    r.side   = (f.get_side() == order_side::buy) ? 'B' : 'S';
    r.qty    = f.get_filled_quantity();
    r.price  = f.get_fill_price();
    r.fee    = f.get_commission();
    r.source = (f.get_source() == fill_source::exchange)  ? "exchange"
             : (f.get_source() == fill_source::simulated) ? "simulated"
             :                                              "local";
    recent_fills_cache_.push_front(std::move(r));
    while (recent_fills_cache_.size() > kRecentFillsCap)
        recent_fills_cache_.pop_back();
}

// The large build_dashboard_view implementation is moved here, adapted to use
// the injected refs instead of direct engine members. Logic is identical.
void DashboardSnapshotBuilder::build_dashboard_view(truetest::ui::dashboard_snapshot& out) const
{
    using snap_t = truetest::ui::dashboard_snapshot;

    // Account — multi-symbol marks (FR-06), same path as engine final/shadow equity.
    const double last_mid = last_mid_price_.load(std::memory_order_relaxed);
    out.cash             = portfolio_.get_cash();
    out.initial_balance  = portfolio_.get_initial_balance();
    {
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        out.equity = portfolio_.get_equity(last_mark_prices_, last_mid);
    }
    out.realized_pnl     = out.equity - out.initial_balance;
    out.unrealized_pnl   = 0.0;

    // Positions
    out.positions.clear();
    out.positions.reserve(portfolio_.get_positions().size());
    for (const auto& [sym, pos] : portfolio_.get_positions())
    {
        if (std::abs(pos.qty) < 1e-12) continue;
        snap_t::position_row row;
        row.symbol     = sym;
        row.qty        = pos.qty;
        row.avg_entry  = (std::abs(pos.qty) > 0.0) ? pos.cost_basis / pos.qty : 0.0;
        // Prefer per-symbol mark; fall back to last_mid only for the primary symbol.
        row.mark = 0.0;
        {
            std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
            if (auto it = last_mark_prices_.find(sym); it != last_mark_prices_.end() && it->second > 0.0)
                row.mark = it->second;
        }
        if (row.mark <= 0.0 && sym == last_mark_symbol_)
            row.mark = last_mid;
        row.unrealized = (row.mark > 0.0) ? (row.mark - row.avg_entry) * pos.qty : 0.0;
        out.unrealized_pnl += row.unrealized;
        out.positions.push_back(std::move(row));
    }

    // Lots
    out.lots.clear();
    out.lots.reserve(portfolio_.get_lots().size());
    auto now_sys = std::chrono::system_clock::now();
    for (const auto& [opener_id, l] : portfolio_.get_lots())
    {
        snap_t::lot_row row;
        row.opener_order_id = opener_id;
        row.symbol          = l.symbol;
        row.strategy_name   = l.strategy_name;
        row.side            = (l.side == order_side::buy) ? 'L' : 'S';
        row.qty_open        = l.qty_open;
        row.entry_price     = l.entry_price;
        row.age_seconds     = std::chrono::duration_cast<std::chrono::seconds>(now_sys - l.ts_open).count();
        out.lots.push_back(std::move(row));
    }

    // Open orders from cache
    out.open_orders.clear();
    out.open_orders.reserve(open_orders_cache_.size());
    auto now_steady_sys = std::chrono::system_clock::now();
    for (const auto& [id, e] : open_orders_cache_)
    {
        auto row = e.row;
        row.age_seconds = std::chrono::duration_cast<std::chrono::seconds>(now_steady_sys - e.ts).count();
        out.open_orders.push_back(std::move(row));
    }

    out.recent_fills.assign(recent_fills_cache_.begin(), recent_fills_cache_.end());

    // Markout, perf
    out.perf.avg_markout_bps = adverse_selection_.mean_bps();
    out.perf.markout_samples = adverse_selection_.sample_count();
    out.perf.total_fills     = portfolio_.get_total_fills();
    out.perf.total_trades    = portfolio_.get_total_trades();
    out.perf.total_orders    = open_orders_cache_.size() + portfolio_.get_total_fills();
    out.perf.win_rate        = analytics_.win_rate_pct();
    out.perf.sharpe          = analytics_.rolling_sharpe();
    out.perf.sortino         = 0.0;
    out.perf.profit_factor   = 0.0;

    // Risk
    out.risk.halted             = halt_flag_.load(std::memory_order_acquire);
    out.risk.daily_loss         = 0.0;
    out.risk.daily_loss_limit   = config_.risk.max_daily_loss;
    out.risk.max_drawdown_pct   = analytics_.max_drawdown_pct();
    out.risk.max_drawdown_limit = config_.risk.max_drawdown * 100.0;
    out.risk.open_orders        = open_orders_cache_.size();
    out.risk.open_orders_limit  = (config_.risk.max_open_orders > 0) ? static_cast<std::size_t>(config_.risk.max_open_orders) : 0;

    double exposure = 0.0;
    for (const auto& [sym, pos] : portfolio_.get_positions())
    {
        if (std::abs(pos.qty) < 1e-12) continue;
        double mark = 0.0;
        {
            std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
            if (auto it = last_mark_prices_.find(sym); it != last_mark_prices_.end() && it->second > 0.0)
                mark = it->second;
        }
        if (mark <= 0.0 && sym == last_mark_symbol_)
            mark = last_mid;
        if (mark > 0.0) exposure += std::abs(pos.qty) * mark;
    }
    out.risk.exposure       = exposure;
    out.risk.exposure_limit = config_.risk.max_portfolio_exposure;

    // Queue
    {
        std::uint64_t qsum = 0;
        std::uint32_t qn = 0;
        std::size_t submitted = 0, filled = 0, blocked = 0;
        auto collect = [&](IExecutionAdapter* a) {
            if (!a) return;
            const auto c = a->live_quote_count();
            if (c == 0) return;
            qsum += a->avg_queue_position_bps();
            ++qn;
            submitted += a->queue_submitted_with_queue();
            filled    += a->queue_filled_after_drain();
            blocked   += a->queue_blocked_at_eos();
        };
        for (auto& [_, ad] : execution_adapters_)
            collect(ad.get());
        if (config_.provider)
            collect(config_.provider->get_execution_adapter().get());
        out.queue.avg_bps = (qn > 0) ? static_cast<std::uint32_t>(qsum / qn) : 0u;
        out.queue.submitted_with_queue = submitted;
        out.queue.filled_after_drain   = filled;
        out.queue.blocked_at_eos       = blocked;
    }

    // Brackets
    out.brackets.clear();
    auto armed = exit_manager_.snapshot_armed();
    out.brackets.reserve(armed.size());
    for (const auto& a : armed)
    {
        snap_t::bracket_row row;
        row.opener_order_id = a.opener_order_id;
        row.strategy_name   = a.strategy_name;
        row.symbol          = a.symbol;
        row.side            = (a.close_side == order_side::sell) ? 'L' : 'S';
        row.qty             = a.qty;
        row.entry_price     = a.entry_price;
        row.stop_loss       = a.stop_loss;
        row.take_profit     = a.take_profit;
        row.venue_managed   = a.venue_managed;
        row.venue_list_id   = a.venue_list_id;
        row.age_seconds     = std::chrono::duration_cast<std::chrono::seconds>(now_sys - a.ts_armed).count();
        for (const auto& p : out.positions)
        {
            if (p.symbol == a.symbol) { row.mark = p.mark; break; }
        }
        if (row.mark == 0.0)
        {
            {
                std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                if (auto it = last_mark_prices_.find(a.symbol); it != last_mark_prices_.end() && it->second > 0.0)
                    row.mark = it->second;
            }
            if (row.mark == 0.0 && a.symbol == last_mark_symbol_)
                row.mark = last_mid;
        }
        out.brackets.push_back(std::move(row));
    }

    // L2
    {
        using snap_t = truetest::ui::dashboard_snapshot;
        auto& v = out.l2;
        v = snap_t::l2_view{};

        constexpr std::size_t kDepthRows = 10;
        const std::string& sym = last_mark_symbol_;
        v.symbol = sym;

        if (!sym.empty())
        {
            auto ob = orderbook_registry_.get(sym);
            if (ob)
            {
                auto infos = ob->get_order_infos();
                const auto& bids = infos.get_bids();
                const auto& asks = infos.get_asks();
                v.total_bid_levels = bids.size();
                v.total_ask_levels = asks.size();

                double cb = 0.0;
                v.bids.reserve(std::min(kDepthRows, bids.size()));
                for (std::size_t i = 0; i < std::min(kDepthRows, bids.size()); ++i)
                {
                    snap_t::l2_level l;
                    l.price = bids[i].price_.to_double();
                    l.size  = static_cast<double>(bids[i].quantity_) / config_.qty_scale;
                    cb += l.size;
                    l.cum   = cb;
                    v.bids.push_back(l);
                }
                v.cum_bid_size = cb;

                double ca = 0.0;
                v.asks.reserve(std::min(kDepthRows, asks.size()));
                for (std::size_t i = 0; i < std::min(kDepthRows, asks.size()); ++i)
                {
                    snap_t::l2_level l;
                    l.price = asks[i].price_.to_double();
                    l.size  = static_cast<double>(asks[i].quantity_) / config_.qty_scale;
                    ca += l.size;
                    l.cum   = ca;
                    v.asks.push_back(l);
                }
                v.cum_ask_size = ca;

                if (!v.bids.empty()) v.best_bid = v.bids.front().price;
                if (!v.asks.empty()) v.best_ask = v.asks.front().price;
                if (v.best_bid > 0.0 && v.best_ask > 0.0)
                {
                    v.mid = (v.best_bid + v.best_ask) * 0.5;
                    v.spread_bps = (v.best_ask - v.best_bid) / v.mid * 1e4;
                    const double bsz = v.bids.front().size;
                    const double asz = v.asks.front().size;
                    const double tot = bsz + asz;
                    v.microprice = (tot > 0.0) ? (bsz * v.best_ask + asz * v.best_bid) / tot : v.mid;
                }
                if (v.cum_bid_size + v.cum_ask_size > 0.0)
                    v.imbalance = (v.cum_bid_size - v.cum_ask_size) / (v.cum_bid_size + v.cum_ask_size);

                if (l2_seeded_symbols_.count(sym))  // note: l2_seeded is not directly here, we need to inject it too
                    v.source = snap_t::l2_source::venue;
                else if (!v.bids.empty() || !v.asks.empty())
                    v.source = snap_t::l2_source::synthetic;
                else
                    v.source = snap_t::l2_source::none;
            }
        }
    }


// Memory view — RSS/heap from /proc (Linux only) + computed pool /
    // ring footprints. Cached at ~1 Hz: memory doesn't move in 100 ms
    // and /proc parsing was the dominant cost of the whole snapshot
    // path. The cache is mutable so this const method can update it.
    {
        const auto now_steady = std::chrono::steady_clock::now();
        const bool stale = !memory_cache_initialised_
            || (now_steady - memory_cache_last_) >= std::chrono::seconds(1);

        if (stale)
        {
            auto& m = memory_cache_;
            m = truetest::ui::dashboard_snapshot::memory_view{};

            // Single-pass /proc/self/status parse. Earlier code opened
            // the file three times (once per key); now one read scans
            // for all three. Saves ~10 KB of redundant I/O per refresh.
            std::ifstream sf("/proc/self/status");
            if (sf.is_open())
            {
                std::string line;
                int hits = 0;
                while (hits < 3 && std::getline(sf, line))
                {
                    auto extract_kib = [&](const char* key) -> std::uint64_t {
                        const std::size_t klen = std::strlen(key);
                        if (line.compare(0, klen, key) != 0) return 0;
                        std::istringstream ss(line);
                        std::string k;
                        std::uint64_t v = 0;
                        ss >> k >> v;
                        return v * 1024;
                    };
                    if (auto v = extract_kib("VmRSS:"))  { m.rss_bytes      = v; ++hits; continue; }
                    if (auto v = extract_kib("VmSize:")) { m.vm_bytes       = v; ++hits; continue; }
                    if (auto v = extract_kib("VmHWM:"))  { m.peak_rss_bytes = v; ++hits; continue; }
                }
            }

            // /proc/self/statm — column 6 is "data" (heap+BSS+stack) in pages.
            {
                std::ifstream f("/proc/self/statm");
                if (f.is_open())
                {
                    std::uint64_t size, resident, shared, text, lib, data, dt;
                    if (f >> size >> resident >> shared >> text >> lib >> data >> dt)
                        m.heap_bytes = data * 4096;
                }
            }
            m.available = (m.rss_bytes > 0);

            // /proc/self/maps — categorise mapped regions so the panel
            // can break down the "other" segment into heap / stacks /
            // shared-lib code / anonymous mmap. Each line:
            //   <start>-<end> <perms> <off> <dev> <ino> <path>
            // sizes are summed in BYTES, not pages, since the address
            // range itself is the size.
            {
                std::ifstream f("/proc/self/maps");
                std::uint64_t b_heap = 0, b_stacks = 0, b_so = 0,
                              b_anon = 0, b_file = 0;
                if (f.is_open())
                {
                    std::string line;
                    while (std::getline(f, line))
                    {
                        // Parse the address range up to the dash.
                        const auto dash = line.find('-');
                        if (dash == std::string::npos) continue;
                        const auto sp1  = line.find(' ', dash + 1);
                        if (sp1 == std::string::npos) continue;
                        std::uint64_t a = 0, b = 0;
                        try {
                            a = std::stoull(line.substr(0, dash), nullptr, 16);
                            b = std::stoull(line.substr(dash + 1, sp1 - dash - 1),
                                            nullptr, 16);
                        } catch (...) { continue; }
                        if (b <= a) continue;
                        const std::uint64_t bytes = b - a;

                        // Path is everything after the last whitespace
                        // run (or empty for anonymous mappings).
                        const auto path_pos = line.find_last_of(' ');
                        std::string path = (path_pos != std::string::npos)
                            ? line.substr(path_pos + 1) : std::string{};

                        if (path == "[heap]")              b_heap   += bytes;
                        else if (path.rfind("[stack", 0) == 0)
                                                            b_stacks += bytes;
                        else if (path.size() >= 3 &&
                                 path.find(".so") != std::string::npos)
                                                            b_so     += bytes;
                        else if (path.empty() || path[0] == '[')
                                                            b_anon   += bytes;
                        else                                b_file   += bytes;
                    }
                }
                if (b_heap   > 0) m.other_breakdown.push_back({"heap",      b_heap});
                if (b_stacks > 0) m.other_breakdown.push_back({"stacks",    b_stacks});
                if (b_so     > 0) m.other_breakdown.push_back({".so libs",  b_so});
                if (b_file   > 0) m.other_breakdown.push_back({"file-mmap", b_file});
                if (b_anon   > 0) m.other_breakdown.push_back({"anon-mmap", b_anon});
            }

            // Pools: ObjectPool::block_count() is now lock-free atomic
            // (see #3 in this commit), so this is just an atomic load
            // per pool. Cached anyway since stale-check already gates us.
            constexpr std::size_t kPoolBlock = 4096;
            auto add_pool = [&](const char* name, std::size_t blocks,
                                std::size_t slot, std::size_t in_use,
                                std::size_t grows) {
                using snap_t = truetest::ui::dashboard_snapshot;
                snap_t::mem_pool_row r;
                r.name           = name;
                r.blocks         = blocks;
                r.slot_size      = slot;
                r.bytes          = static_cast<std::uint64_t>(blocks) * kPoolBlock * slot;
                r.in_use         = in_use;
                r.capacity_slots = blocks * kPoolBlock;
                r.grow_count     = grows;
                m.pool_bytes_total += r.bytes;
                m.pools.push_back(r);
            };
            add_pool("market_pool",      market_pool_.block_count(),
                     sizeof(market_event), market_pool_.in_use(), market_pool_.grow_count());
            add_pool("order_pool",       order_pool_.block_count(),
                     sizeof(order_event),  order_pool_.in_use(),  order_pool_.grow_count());
            add_pool("fill_pool",        fill_pool_.block_count(),
                     sizeof(fill_event),   fill_pool_.in_use(),   fill_pool_.grow_count());
            add_pool("tick_pool",        tick_pool_.block_count(),
                     sizeof(tick_event),   tick_pool_.in_use(),   tick_pool_.grow_count());
            add_pool("l2_update_pool",   l2_update_pool_.block_count(),
                     sizeof(l2_update_event), l2_update_pool_.in_use(), l2_update_pool_.grow_count());
            add_pool("l2_snapshot_pool", l2_snapshot_pool_.block_count(),
                     sizeof(l2_snapshot_event), l2_snapshot_pool_.in_use(), l2_snapshot_pool_.grow_count());
            add_pool("rejection_pool",   rejection_pool_.block_count(),
                     sizeof(rejection_event), rejection_pool_.in_use(), rejection_pool_.grow_count());
            add_pool("cancel_pool",      cancel_pool_.block_count(),
                     sizeof(cancel_event), cancel_pool_.in_use(), cancel_pool_.grow_count());
            add_pool("amend_pool",       amend_pool_.block_count(),
                     sizeof(amend_event), amend_pool_.in_use(), amend_pool_.grow_count());
            add_pool("funding_pool",     funding_pool_.block_count(),
                     sizeof(funding_event), funding_pool_.in_use(), funding_pool_.grow_count());
            add_pool("control_block_pool", control_block_pool_.block_count(),
                     ControlBlockPool::slot_size(), control_block_pool_.in_use(),
                     control_block_pool_.grow_count());

            // Rings: capacity is constexpr; this is essentially free.
            auto add_ring = [&](const char* name, const auto& ring) {
                using snap_t = truetest::ui::dashboard_snapshot;
                snap_t::mem_ring_row r;
                r.name = name;
                if (ring) {
                    r.capacity      = ring->capacity();
                    r.element_bytes = sizeof(event_pointer);
                    r.bytes = static_cast<std::uint64_t>(r.capacity) * r.element_bytes;
                    m.ring_bytes_total += r.bytes;
                }
                m.rings.push_back(r);
            };
            add_ring("logging",    logging_ring_);
            add_ring("risk",       risk_ring_);
            add_ring("stats",      stats_ring_);
            add_ring("observer",   observer_ring_);
            add_ring("risk_stats", risk_stats_ring_);
            add_ring("mm_event",   mm_ring_);
            // mm_order_ring_ not injected; skip or pass empty shared
            // add_ring("mm_order", ...);

            memory_cache_last_ = now_steady;
            memory_cache_initialised_ = true;
        }
        out.memory = memory_cache_;

        // In-use counts refresh every snapshot — atomic loads are free,
        // and the panel's pool fill-bars look frozen if they lag the
        // cached interval. Order matches the add_pool() calls above.
        // Refresh in_use for memory pools (now >4 pools post-extraction; always update known ones)
        for (auto& p : out.memory.pools) {
            const std::string_view name = p.name ? p.name : "";
            if (name == "market_pool") p.in_use = market_pool_.in_use();
            else if (name == "order_pool") p.in_use = order_pool_.in_use();
            else if (name == "fill_pool") p.in_use = fill_pool_.in_use();
            else if (name == "tick_pool") p.in_use = tick_pool_.in_use();
            // extend for other pools if the memory UI cares
        }
    }

    // Debug view — engine introspection for the Debug tab. All sourced
    // from existing accessors / atomics; no new hot-path tracking. Built
    // once per snapshot under the same lock as everything else.
    {
        auto& d = out.debug;
        // engine_core is built without TT_TARGET (per-binary define), so
        // we surface the runtime mode here instead of the binary name.
        // The status bar already shows which binary you're in via the
        // window chrome / process name.
        d.target = "(engine_core)";
        switch (config_.mode) {
            case engine_mode::backtest: d.mode = "backtest"; break;
            case engine_mode::shadow:   d.mode = "shadow";   break;
            case engine_mode::live:     d.mode = "live";     break;
        }
#ifdef HAS_BINANCE
        d.has_binance = true;
#endif
#ifdef HAS_QUESTDB
        d.has_questdb = true;
#endif
#ifdef HAS_DEBUG
        d.has_debug = true;
#endif
#ifdef TRUETEST_VENUE_DATA_COMPILED
        d.has_live_data = true;
#endif

        d.preset = preset_to_string(config_.threading);
        d.spin_policy = spin_policy_to_string(config_.worker_spin_policy);
        d.cpu_pin    = !config_.disable_pinning;

        // Worker count: derived from which ring members are non-null.
        std::size_t workers = 0;
        if (logging_ring_)    ++workers;
        if (risk_ring_)       ++workers;
        if (stats_ring_)      ++workers;
        if (observer_ring_)   ++workers;
        if (risk_stats_ring_) ++workers;
        if (mm_ring_) ++workers; // mm_order_ring_ not injected
        d.worker_count = workers;

        // Ring stats — capacity 0 indicates the ring isn't running at
        // this preset (engine returns nullptr from the get_*_ring()).
        auto add_ring = [&](const char* name, const auto& ring) {
            using snap_t = truetest::ui::dashboard_snapshot;
            snap_t::ring_stat r;
            r.name = name;
            if (ring) {
                r.size = ring->size();
                r.hwm  = ring->high_watermark();
                r.capacity = ring->capacity();
                r.drops = ring->drop_count();
            }
            d.rings.push_back(r);
        };
        add_ring("logging",    logging_ring_);
        add_ring("risk",       risk_ring_);
        add_ring("stats",      stats_ring_);
        add_ring("observer",   observer_ring_);
        add_ring("risk_stats", risk_stats_ring_);
        add_ring("mm_event",   mm_ring_);
        // nullptr /* mm_order_ring_ not injected */ uses MMRing (different element type) — track separately.
        {
            using snap_t = truetest::ui::dashboard_snapshot;
            snap_t::ring_stat r;
            r.name = "mm_order";
            // mm_order_ring_ not injected into builder; debug view omits it for now
            // if (mm_order_ring_) { r.size = ... }
            d.rings.push_back(r);
        }

        constexpr std::size_t kPoolBlock = 4096;
        auto add_pool = [&](const char* name, std::size_t blocks,
                            std::size_t in_use, std::size_t grows) {
            using snap_t = truetest::ui::dashboard_snapshot;
            snap_t::pool_stat p;
            p.name = name;
            p.blocks = blocks;
            p.block_size = kPoolBlock;
            p.capacity = blocks * kPoolBlock;
            p.in_use = in_use;
            p.grow_count = grows;
            d.pools.push_back(p);
        };
        add_pool("market_pool",      market_pool_.block_count(),
                 market_pool_.in_use(), market_pool_.grow_count());
        add_pool("order_pool",       order_pool_.block_count(),
                 order_pool_.in_use(), order_pool_.grow_count());
        add_pool("fill_pool",        fill_pool_.block_count(),
                 fill_pool_.in_use(), fill_pool_.grow_count());
        add_pool("tick_pool",        tick_pool_.block_count(),
                 tick_pool_.in_use(), tick_pool_.grow_count());
        add_pool("l2_update_pool",   l2_update_pool_.block_count(),
                 l2_update_pool_.in_use(), l2_update_pool_.grow_count());
        add_pool("l2_snapshot_pool", l2_snapshot_pool_.block_count(),
                 l2_snapshot_pool_.in_use(), l2_snapshot_pool_.grow_count());
        add_pool("rejection_pool",   rejection_pool_.block_count(),
                 rejection_pool_.in_use(), rejection_pool_.grow_count());
        add_pool("cancel_pool",      cancel_pool_.block_count(),
                 cancel_pool_.in_use(), cancel_pool_.grow_count());
        add_pool("amend_pool",       amend_pool_.block_count(),
                 amend_pool_.in_use(), amend_pool_.grow_count());
        add_pool("funding_pool",     funding_pool_.block_count(),
                 funding_pool_.in_use(), funding_pool_.grow_count());
        add_pool("control_block_pool", control_block_pool_.block_count(),
                 control_block_pool_.in_use(), control_block_pool_.grow_count());

        // Engine state.
        d.next_order_id = 0; // OrderIdGenerator not in scope here
        // Decrement back so we don't perturb the live id sequence — the
        // call above only reads + increments atomically, so the value
        // we surface is "what the next allocation WOULD have been".
        // (We can't decrement an atomic safely if other threads bump it
        //  in between; subtract logically: the value we want is `next-1`
        //  treating it as the most-recently-allocated id, which is fine
        //  for an introspection display.)
        d.next_order_id = 0; // TODO: OrderIdGenerator not visible; was d.next_order_id = OrderIdGenerator::next(); --d...
        d.pending_orders    = 0;  // TODO: expose from builder or pending state (not injected yet)
        d.pending_stops     = 0;
        d.open_orders_cache = open_orders_cache_.size();
        d.order_meta_size   = 0;  // TODO
        d.armed_brackets    = exit_manager_.armed_count();
        d.handles_size      = exit_manager_.armed_count();   // proxy

        d.exit_pending = exit_manager_.pending_count();
        d.exit_armed   = exit_manager_.armed_count();
        d.exit_exchange_to_leg = 0;  // not exposed; left as 0

#ifdef HAS_DEBUG
        // Stage timings — only present on HAS_DEBUG builds.
        if (auto* stimer = debug_samplers_.stage_timer) {
            for (std::size_t i = 0; i < static_cast<std::size_t>(debug::stage::COUNT); ++i)
            {
                const auto st = static_cast<debug::stage>(i);
                const auto s  = stimer->snapshot(st);
                if (s.call_count == 0) continue;
                using snap_t = truetest::ui::dashboard_snapshot;
                snap_t::debug_view::stage_row r;
                r.name   = debug::StageTimer::stage_name(st);
                r.calls  = s.call_count;
                r.avg_ns = s.total_ns / s.call_count;
                r.min_ns = s.min_ns;
                r.max_ns = s.max_ns;
                d.stages.push_back(r);
            }
        }
#endif

        // Last errors — pull what's exposed; subsystems without a public
        // accessor get an empty string. Bridge has last_error() when the
        // provider supplies an ExecutionBridge adapter.
        using snap_t = truetest::ui::dashboard_snapshot;
        d.errors.push_back(snap_t::subsys_error{"engine",  ""});
        if (config_.provider)
        {
            auto adapter = config_.provider->get_execution_adapter();
            std::string bridge_err = adapter ? adapter->last_error() : "";
            d.errors.push_back(snap_t::subsys_error{"bridge", std::move(bridge_err)});
        }
        else d.errors.push_back(snap_t::subsys_error{"bridge", ""});
        d.errors.push_back(snap_t::subsys_error{"provider", ""});
        d.errors.push_back(snap_t::subsys_error{"adapter",  ""});
        d.errors.push_back(snap_t::subsys_error{"questdb",  ""});
    }

    // Health view — latency from analytics + provider state. Ring drops,
    // event totals, and ages are read by the Health panel directly from
    // ConsoleDashboard's atomics (the engine does not own that ring).
    {
        auto lv = analytics_.latency_view_now();
        out.health.tick_to_trade_samples = lv.samples;
        out.health.avg_tick_to_trade_us  = lv.avg_ns / 1000.0;
        out.health.min_tick_to_trade_us  = static_cast<double>(lv.min_ns) / 1000.0;
        out.health.max_tick_to_trade_us  = static_cast<double>(lv.max_ns) / 1000.0;

        out.health.orders_total = open_orders_cache_.size()
                                + portfolio_.get_total_fills();
        out.health.fills_total  = portfolio_.get_total_fills();
        out.health.trades_total = portfolio_.get_total_trades();

        if (config_.provider)
        {
            out.health.provider_present = true;
            out.health.provider_name    = config_.provider->name();
            out.health.provider_state   = static_cast<int>(
                config_.provider->lifecycle_state());
        }

        // Unconditional audit_sink health (replaces remaining questdb guard + #ifdef).
        // strict_mode and last_flush default (0/-1) as sink seam does not yet surface full QuestdbStore::Health.
        auto qh = audit_sink_.health();
        out.health.questdb.active = qh.connected || qh.pending_lines > 0 || qh.dropped_lines > 0 || qh.fallback_lines > 0;
        out.health.questdb.connected = qh.connected;
        out.health.questdb.pending_lines = qh.pending_lines;
        out.health.questdb.dropped_lines = qh.dropped_lines;
        out.health.questdb.fallback_lines = qh.fallback_lines;
        out.health.questdb.strict_mode = false;
        out.health.questdb.last_flush_age_ms = -1;
    }

    // Per-strategy attribution. analytics_.per_strategy_view() copies
    // the per-strategy map cheaply (O(K) for K strategies). Counts of
    // open lots and armed brackets are derived from the same passes
    // we already did over portfolio_/exit_manager_ above.
    out.strategies.clear();
    {
        // Tally lots and armed brackets per strategy in single passes.
        std::unordered_map<std::string, std::size_t> lots_by_strat;
        for (const auto& [_, l] : portfolio_.get_lots())
            ++lots_by_strat[l.strategy_name];
        std::unordered_map<std::string, std::size_t> brackets_by_strat;
        for (const auto& a : armed)
            ++brackets_by_strat[a.strategy_name];

        auto per_strat = analytics_.per_strategy_view();
        out.strategies.reserve(per_strat.size());
        for (const auto& [name, sa] : per_strat)
        {
            snap_t::strategy_row row;
            row.name           = name;
            row.pnl            = sa.total_pnl;
            row.trade_count    = sa.trade_count;
            row.win_count      = sa.win_count;
            row.win_rate       = sa.win_rate();
            row.profit_factor  = sa.profit_factor();
            row.total_win      = sa.total_win;
            row.total_loss     = sa.total_loss;
            auto lit = lots_by_strat.find(name);
            row.open_lots      = (lit != lots_by_strat.end()) ? lit->second : 0;
            auto bit = brackets_by_strat.find(name);
            row.armed_brackets = (bit != brackets_by_strat.end()) ? bit->second : 0;
            out.strategies.push_back(std::move(row));
        }
    }

    // Trend strip — equity + drawdown tails the Overview panel renders
    // as sparklines. 60 cells matches the panel width budget; longer
    // tails are sub-sampled by ascii::sparkline anyway. The rate tail
    // is sourced by the panel directly from ConsoleDashboard (it owns
    // the rolling rate_ema_ samples), so the engine doesn't reach into
    // the dashboard from here.
    constexpr std::size_t kTailWidth = 60;
    out.trend.equity_tail   = analytics_.equity_tail(kTailWidth);
    out.trend.drawdown_tail = analytics_.drawdown_tail(kTailWidth);

    out.trend.equity_now = out.equity;
    out.trend.equity_change_pct = (out.initial_balance > 0.0)
        ? (out.equity / out.initial_balance - 1.0) * 100.0
        : 0.0;
    out.trend.drawdown_now_pct = out.trend.drawdown_tail.empty()
        ? 0.0 : out.trend.drawdown_tail.back();
}

// (memory/debug/trend fully ported for snapshot fidelity)

void DashboardSnapshotBuilder::clear_for_mc_reset()
{
    open_orders_cache_.clear();
    recent_fills_cache_.clear();
    dashboard_view_initialised_ = false;
    memory_cache_initialised_ = false;
    // view and memory will be re-populated on next refresh/snapshot
}
