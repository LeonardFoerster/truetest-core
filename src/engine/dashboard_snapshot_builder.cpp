#include "dashboard_snapshot_builder.h"

#include "analytics/adverse_selection_tracker.h"
#include "analytics/analytics.h"
#include "engine_config.h"
#include "execution/execution_adapter.h"
#include "execution/portfolio.h"
#include "exits/exit_manager.h"
#include "orderbook/orderbook_registry.h"
#include "providers/provider.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#ifdef HAS_DEBUG
#include "debug/stage_timer.h"
#endif

namespace dash = truetest::dashboard;

DashboardSnapshotBuilder::DashboardSnapshotBuilder(
    const portfolio& port,
    const Analytics& analytics,
    const AdverseSelectionTracker& adverse,
    const truetest::exits::ExitManager& exits,
    const std::atomic<bool>& halt_flag,
    const std::atomic<std::size_t>& active_order_count,
    const engine_config& config,
    const std::atomic<double>& last_mid_price,
    const std::string& last_mark_symbol,
    const std::unordered_map<std::string, double>& last_mark_prices,
    OrderbookRegistry& orderbook_registry,
    const std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters,
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
    DashboardEngineDebugSampler engine_debug_counts,
    DebugSamplers debug_samplers)
    : portfolio_(port)
    , analytics_(analytics)
    , adverse_selection_(adverse)
    , exit_manager_(exits)
    , halt_flag_(halt_flag)
    , active_order_count_(active_order_count)
    , config_(config)
    , last_mid_price_(last_mid_price)
    , last_mark_symbol_(last_mark_symbol)
    , last_mark_prices_(last_mark_prices)
    , orderbook_registry_(orderbook_registry)
    , execution_adapters_(execution_adapters)
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
    , engine_debug_counts_(engine_debug_counts)
    , debug_samplers_(debug_samplers)
{
    constexpr std::size_t kTextReserve = dash::kTextCapacity;
    const auto configured_orders = config_.risk.max_open_orders > 0
        ? static_cast<std::size_t>(config_.risk.max_open_orders)
        : dash::kMaxOpenOrders;
    const auto bounded_orders = std::min(configured_orders,
                                         dash::kMaxOpenOrders);
    const auto required_slots = std::max<std::size_t>(8, bounded_orders * 2U);
    std::size_t table_size = 8;
    while (table_size < required_slots) table_size *= 2U;
    open_orders_cache_.resize(table_size);
    for (auto& entry : open_orders_cache_)
    {
        entry.row.symbol.reserve(kTextReserve);
        entry.row.strategy_name.reserve(kTextReserve);
    }
    for (auto& fill : recent_fills_cache_)
        fill.symbol.reserve(kTextReserve);
    if (config_.provider)
    {
        try { provider_name_static_ = config_.provider->name(); }
        catch (...) { provider_name_static_.clear(); }
    }
}

DashboardSnapshotBuilder::~DashboardSnapshotBuilder() = default;

bool DashboardSnapshotBuilder::capture_projection(
    dash::DashboardProjection& out,
    std::uint64_t request_epoch,
    bool analytics_quiescent) noexcept
{
    out.generation = snapshot_generation_ + 1U;
    out.fulfilled_request_epoch = request_epoch;
    out.generated_at = std::chrono::steady_clock::now();
    out.captured_wall_time = std::chrono::system_clock::now();
    out.complete = true;

    out.marks_state = {};
    out.positions_state = {};
    out.lots_state = {};
    out.open_orders_state = {};
    out.recent_fills_state = {};
    out.brackets_state = {};
    out.strategies_state = {};
    out.books_state = {};
    out.market_symbols_state = {};
    out.stages_state = {};

    const auto assign_text = [&out](dash::text& target,
                                    std::string_view source) noexcept {
        if (!target.assign(source)) out.complete = false;
    };
    const auto add_market_symbol = [&out, &assign_text](
                                       std::string_view source) noexcept {
        if (source.empty()) return;
        for (std::size_t i = 0; i < out.market_symbols_state.count; ++i)
            if (out.market_symbols[i].view() == source) return;
        if (!out.market_symbols_state.appendable(out.market_symbols.size()))
            return;
        assign_text(out.market_symbols[out.market_symbols_state.count++],
                    source);
    };

    assign_text(out.active_symbol, last_mark_symbol_);
    add_market_symbol(last_mark_symbol_);
    out.last_mid = last_mid_price_.load(std::memory_order_relaxed);
    out.cash = portfolio_.get_cash();
    out.initial_balance = portfolio_.get_initial_balance();
    out.total_fills = portfolio_.get_total_fills();
    out.total_trades = portfolio_.get_total_trades();

    // Only the event producer mutates this map. Cross-thread consumers no
    // longer read it; therefore no mutex is needed (or permitted) here.
    for (const auto& [symbol, mark] : last_mark_prices_)
    {
        if (!out.marks_state.appendable(out.marks.size())) continue;
        auto& row = out.marks[out.marks_state.count++];
        assign_text(row.symbol, symbol);
        add_market_symbol(symbol);
        row.value = mark;
    }

    for (const auto& [symbol, position] : portfolio_.get_positions())
    {
        if (std::abs(position.qty) < 1e-12) continue;
        if (!out.positions_state.appendable(out.positions.size())) continue;
        auto& row = out.positions[out.positions_state.count++];
        assign_text(row.symbol, symbol);
        add_market_symbol(symbol);
        row.qty = position.qty;
        row.cost_basis = position.cost_basis;
    }

    for (const auto& [opener, lot] : portfolio_.get_lots())
    {
        if (!out.lots_state.appendable(out.lots.size())) continue;
        auto& row = out.lots[out.lots_state.count++];
        row.opener_order_id = opener;
        assign_text(row.symbol, lot.symbol);
        add_market_symbol(lot.symbol);
        assign_text(row.strategy_name, lot.strategy_name);
        row.side = lot.side;
        row.qty_open = lot.qty_open;
        row.entry_price = lot.entry_price;
        row.ts_open = lot.ts_open;
    }

    for (const auto& entry : open_orders_cache_)
    {
        if (entry.state != open_order_cache_entry::slot_state::occupied)
            continue;
        if (!out.open_orders_state.appendable(out.open_orders.size()))
            continue;
        auto& row = out.open_orders[out.open_orders_state.count++];
        row.order_id = entry.row.order_id;
        assign_text(row.symbol, entry.row.symbol);
        add_market_symbol(entry.row.symbol);
        assign_text(row.strategy_name, entry.row.strategy_name);
        row.side = entry.row.side;
        row.type = entry.row.type;
        row.qty = entry.row.qty;
        row.price = entry.row.price;
        row.trigger_price = entry.row.trigger_price;
        row.trigger_price_available = entry.row.trigger_price_available;
        row.timestamp = entry.ts;
        row.status = entry.row.status;
    }
    out.open_orders_state.total_count += open_orders_cache_overflow_count_;
    if (open_orders_cache_overflow_count_ != 0U)
        out.open_orders_state.complete = false;

    out.recent_fills_state.total_count = recent_fills_count_;
    out.recent_fills_state.count = recent_fills_count_;
    for (std::size_t i = 0; i < recent_fills_count_; ++i)
    {
        const auto index =
            (recent_fills_head_ + kRecentFillsCap - 1U - i) %
            kRecentFillsCap;
        const auto& source = recent_fills_cache_[index];
        auto& row = out.recent_fills[i];
        row.timestamp = source.ts;
        assign_text(row.symbol, source.symbol);
        row.side = source.side;
        row.qty = source.qty;
        row.price = source.price;
        row.fee = source.fee;
        row.source = source.source;
    }

    exit_manager_.for_each_armed_event_thread(
        [&out, &assign_text](std::uint64_t opener,
                            const truetest::exits::exit_intent& intent,
                            double entry_price,
                            std::chrono::system_clock::time_point ts) noexcept {
            if (!out.brackets_state.appendable(out.brackets.size())) return;
            auto& row = out.brackets[out.brackets_state.count++];
            row.opener_order_id = opener;
            assign_text(row.strategy_name, intent.strategy_name);
            assign_text(row.symbol, intent.symbol);
            row.close_side = intent.close_side;
            row.qty = intent.qty;
            row.entry_price = entry_price;
            row.stop_loss = intent.stop_loss;
            row.take_profit = intent.take_profit;
            row.ts_armed = ts;
        });

    const double qty_scale = config_.qty_scale > 0.0 ? config_.qty_scale : 1.0;
    orderbook_registry_.for_each_book(
        [&out, &assign_text, &add_market_symbol, this, qty_scale](
            const std::string& symbol, const orderbook& book) noexcept {
            if (!out.books_state.appendable(out.books.size())) return;
            auto& row = out.books[out.books_state.count++];
            assign_text(row.symbol, symbol);
            add_market_symbol(symbol);
            row.venue_seeded = l2_seeded_symbols_.count(symbol) != 0U;
            std::array<lvl_info, dash::kDepthLevels> bids{};
            std::array<lvl_info, dash::kDepthLevels> asks{};
            const auto depth = book.copy_external_depth(bids, asks);
            row.quantity_valid = !depth.quantity_overflow;
            row.total_bid_levels = depth.total_bid_levels;
            row.total_ask_levels = depth.total_ask_levels;
            row.bid_count = depth.bid_count;
            row.ask_count = depth.ask_count;
            for (std::size_t i = 0; i < row.bid_count; ++i)
            {
                row.bids[i].price = bids[i].price_.to_double();
                row.bids[i].size = static_cast<double>(bids[i].quantity_) /
                                   qty_scale;
            }
            for (std::size_t i = 0; i < row.ask_count; ++i)
            {
                row.asks[i].price = asks[i].price_.to_double();
                row.asks[i].size = static_cast<double>(asks[i].quantity_) /
                                   qty_scale;
            }
        });

    out.queue_diagnostics_available = false;
    out.queue_available = false;
    out.queue_diagnostics_complete = true;
    out.queue_avg_bps = 0;
    out.queue_submitted_with_queue = 0;
    out.queue_filled_after_drain = 0;
    out.queue_blocked_at_eos = 0;
    out.provider_present = static_cast<bool>(config_.provider);
    out.provider_state_available = false;
    out.provider_state = -1;
    assign_text(out.provider_name, provider_name_static_);

    // Adapter/provider diagnostics are virtual and several implementations
    // expose event-thread-owned containers. Invoke them only for explicitly
    // quiescent initial/terminal captures; runtime projections mark the data
    // unavailable and never retain a raw adapter pointer for UI threads.
    if (analytics_quiescent)
    {
        try
        {
            std::array<IExecutionAdapter*, dash::kMaxSymbols + 1U> adapters{};
            std::size_t adapter_count = 0;
            const auto add_adapter = [&](IExecutionAdapter* adapter) noexcept {
                if (!adapter) return;
                for (std::size_t i = 0; i < adapter_count; ++i)
                    if (adapters[i] == adapter) return;
                if (adapter_count == adapters.size())
                {
                    out.queue_diagnostics_complete = false;
                    return;
                }
                adapters[adapter_count++] = adapter;
            };
            for (const auto& [_, adapter] : execution_adapters_)
                add_adapter(adapter.get());
            std::shared_ptr<IExecutionAdapter> provider_adapter;
            if (config_.provider)
            {
                out.provider_state = static_cast<int>(
                    config_.provider->lifecycle_state());
                out.provider_state_available = true;
                provider_adapter = config_.provider->get_execution_adapter();
                add_adapter(provider_adapter.get());
            }

            std::uint64_t queue_sum = 0;
            std::uint32_t queue_sources = 0;
            for (std::size_t i = 0; i < adapter_count; ++i)
            {
                auto* adapter = adapters[i];
                if (adapter->live_quote_count() == 0U) continue;
                queue_sum += adapter->avg_queue_position_bps();
                ++queue_sources;
                out.queue_submitted_with_queue +=
                    adapter->queue_submitted_with_queue();
                out.queue_filled_after_drain +=
                    adapter->queue_filled_after_drain();
                out.queue_blocked_at_eos +=
                    adapter->queue_blocked_at_eos();
            }
            out.queue_available = queue_sources != 0U;
            out.queue_diagnostics_available = true;
            out.queue_avg_bps = queue_sources != 0U
                ? static_cast<std::uint32_t>(queue_sum / queue_sources) : 0U;
        }
        catch (...)
        {
            out.queue_available = false;
            out.queue_diagnostics_available = false;
            out.queue_diagnostics_complete = false;
            out.provider_state_available = false;
            out.provider_state = -1;
        }
    }

#ifdef HAS_DEBUG
    if (auto* timer = debug_samplers_.stage_timer)
    {
        for (std::size_t i = 0;
             i < static_cast<std::size_t>(debug::stage::COUNT); ++i)
        {
            const auto stage = static_cast<debug::stage>(i);
            const auto stats = timer->snapshot(stage);
            if (stats.call_count == 0U) continue;
            if (!out.stages_state.appendable(out.stages.size())) continue;
            auto& row = out.stages[out.stages_state.count++];
            row.index = static_cast<std::uint8_t>(i);
            row.calls = stats.call_count;
            row.total_ns = stats.total_ns;
            row.min_ns = stats.min_ns;
            row.max_ns = stats.max_ns;
        }
    }
#endif

    out.analytics_available = analytics_quiescent || !config_.is_threaded();
    out.strategies_state = {};
    out.equity_tail_count = 0;
    out.drawdown_tail_count = 0;
    out.avg_markout_bps = 0.0;
    out.markout_samples = 0;
    out.total_orders = 0;
    out.win_rate = 0.0;
    out.sharpe = 0.0;
    out.sortino = 0.0;
    out.profit_factor = 0.0;
    out.max_drawdown_pct = 0.0;
    out.latency = {};
    if (out.analytics_available)
    {
        out.avg_markout_bps = adverse_selection_.mean_bps();
        out.markout_samples = adverse_selection_.sample_count();
        out.total_orders = analytics_.total_orders();
        out.win_rate = analytics_.win_rate_pct();
        out.sharpe = analytics_.rolling_sharpe();
        out.sortino = analytics_.sortino_now();
        out.profit_factor = analytics_.profit_factor_now();
        out.max_drawdown_pct = analytics_.max_drawdown_pct();
        const auto latency = analytics_.latency_view_now();
        out.latency = {latency.avg_ns, latency.min_ns,
                       latency.max_ns, latency.samples};
        out.equity_tail_count = analytics_.copy_equity_tail(
            std::span<double>{out.equity_tail});
        out.drawdown_tail_count = analytics_.copy_drawdown_tail(
            std::span<double>{out.drawdown_tail});

        for (const auto& [name, stats] : analytics_.per_strategy_view_ref())
        {
            if (!out.strategies_state.appendable(out.strategies.size()))
                continue;
            auto& row = out.strategies[out.strategies_state.count++];
            assign_text(row.name, name);
            row.pnl = stats.total_pnl;
            row.trade_count = stats.trade_count;
            row.win_count = stats.win_count;
            row.total_win = stats.total_win;
            row.total_loss = stats.total_loss;
        }
    }

    out.halted = halt_flag_.load(std::memory_order_acquire);
    out.active_order_count =
        active_order_count_.load(std::memory_order_acquire);
    out.engine_counts_available = static_cast<bool>(engine_debug_counts_);
    out.engine_counts = engine_debug_counts_();
    out.open_orders_cache_size = open_orders_cache_size_;
    out.exit_pending = exit_manager_.pending_count();
    out.exit_armed = exit_manager_.armed_count();
    out.cache_complete = dashboard_cache_complete_;

    const auto collection_complete =
        out.marks_state.complete && out.positions_state.complete &&
        out.lots_state.complete && out.open_orders_state.complete &&
        out.recent_fills_state.complete && out.brackets_state.complete &&
        out.strategies_state.complete && out.books_state.complete &&
        out.market_symbols_state.complete && out.stages_state.complete;
    out.complete = out.complete && collection_complete &&
                   dashboard_cache_complete_;
    return true;
}

void DashboardSnapshotBuilder::materialize_dashboard_view(
    const dash::DashboardProjection& projection,
    truetest::ui::dashboard_snapshot& out) const
{
    using snapshot_t = truetest::ui::dashboard_snapshot;
    out = snapshot_t{};

    const auto mark_for = [&projection](std::string_view symbol) noexcept {
        for (std::size_t i = 0; i < projection.marks_state.count; ++i)
            if (projection.marks[i].symbol.view() == symbol &&
                std::isfinite(projection.marks[i].value) &&
                projection.marks[i].value > 0.0)
                return projection.marks[i].value;
        return projection.active_symbol.view() == symbol &&
               projection.last_mid > 0.0
            ? projection.last_mid : 0.0;
    };

    out.cash = projection.cash;
    out.initial_balance = projection.initial_balance;
    out.equity_available = true;
    out.realized_pnl_available = true;
    out.unrealized_pnl_available = true;
    double marked_position_value = 0.0;
    double settled_pnl = out.cash - out.initial_balance;
    out.positions.reserve(projection.positions_state.count);
    for (std::size_t i = 0; i < projection.positions_state.count; ++i)
    {
        const auto& source = projection.positions[i];
        snapshot_t::position_row row;
        row.symbol = source.symbol.view();
        row.qty = source.qty;
        row.avg_entry = std::abs(source.qty) > 0.0
            ? source.cost_basis / source.qty : 0.0;
        row.mark = mark_for(source.symbol.view());
        row.mark_available = row.mark > 0.0;
        row.unrealized_available = row.mark_available;
        if (row.mark_available)
        {
            row.unrealized = row.mark * row.qty - source.cost_basis;
            out.unrealized_pnl += row.unrealized;
            marked_position_value += row.mark * row.qty;
        }
        else
        {
            out.equity_available = false;
            out.unrealized_pnl_available = false;
        }
        settled_pnl += source.cost_basis;
        out.positions.push_back(std::move(row));
    }
    if (out.equity_available)
    {
        out.equity = out.cash + marked_position_value;
        out.total_pnl = out.equity - out.initial_balance;
        out.total_pnl_available = true;
    }
    else
        out.unrealized_pnl = 0.0;
    out.realized_pnl = settled_pnl;

    const auto capture_time = projection.captured_wall_time;
    out.lots.reserve(projection.lots_state.count);
    for (std::size_t i = 0; i < projection.lots_state.count; ++i)
    {
        const auto& source = projection.lots[i];
        snapshot_t::lot_row row;
        row.opener_order_id = source.opener_order_id;
        row.symbol = source.symbol.view();
        row.strategy_name = source.strategy_name.view();
        row.side = source.side == order_side::buy ? 'L' : 'S';
        row.qty_open = source.qty_open;
        row.entry_price = source.entry_price;
        row.age_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            capture_time - source.ts_open).count();
        out.lots.push_back(std::move(row));
    }

    out.open_orders.reserve(projection.open_orders_state.count);
    for (std::size_t i = 0; i < projection.open_orders_state.count; ++i)
    {
        const auto& source = projection.open_orders[i];
        snapshot_t::open_order_row row;
        row.order_id = source.order_id;
        row.symbol = source.symbol.view();
        row.strategy_name = source.strategy_name.view();
        row.side = source.side;
        row.type = source.type;
        row.qty = source.qty;
        row.price = source.price;
        row.trigger_price = source.trigger_price;
        row.trigger_price_available = source.trigger_price_available;
        row.age_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            capture_time - source.timestamp).count();
        row.status = source.status;
        out.open_orders.push_back(std::move(row));
    }
    std::sort(out.open_orders.begin(), out.open_orders.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.order_id < rhs.order_id;
              });

    out.recent_fills.reserve(projection.recent_fills_state.count);
    for (std::size_t i = 0; i < projection.recent_fills_state.count; ++i)
    {
        const auto& source = projection.recent_fills[i];
        snapshot_t::fill_row row;
        row.ts = source.timestamp;
        row.symbol = source.symbol.view();
        row.side = source.side;
        row.qty = source.qty;
        row.price = source.price;
        row.fee = source.fee;
        row.source = source.source;
        out.recent_fills.push_back(std::move(row));
    }

    out.perf.avg_markout_bps = projection.avg_markout_bps;
    out.perf.markout_samples = projection.markout_samples;
    out.perf.total_fills = projection.total_fills;
    out.perf.total_trades = projection.total_trades;
    out.perf.total_orders = projection.total_orders;
    out.perf.win_rate = projection.win_rate;
    out.perf.sharpe = projection.sharpe;
    out.perf.sortino = projection.sortino;
    out.perf.profit_factor = projection.profit_factor;

    out.risk.halted = projection.halted;
    out.risk.daily_loss_limit = config_.risk.max_daily_loss;
    out.risk.max_drawdown_pct = projection.max_drawdown_pct;
    out.risk.max_drawdown_limit = config_.risk.max_drawdown * 100.0;
    out.risk.max_drawdown_available = projection.analytics_available &&
        projection.drawdown_tail_count != 0U;
    out.risk.open_orders = projection.active_order_count;
    out.risk.open_orders_limit = config_.risk.max_open_orders > 0
        ? static_cast<std::size_t>(config_.risk.max_open_orders) : 0U;
    bool exposure_available = true;
    double exposure = 0.0;
    for (std::size_t i = 0; i < projection.positions_state.count; ++i)
    {
        const auto& position = projection.positions[i];
        const auto mark = mark_for(position.symbol.view());
        if (mark > 0.0) exposure += std::abs(position.qty) * mark;
        else exposure_available = false;
    }
    out.risk.exposure_available = exposure_available;
    out.risk.exposure = exposure_available ? exposure : 0.0;
    out.risk.exposure_limit = config_.risk.max_portfolio_exposure;

    out.queue.available = projection.queue_available;
    out.queue.avg_bps = projection.queue_avg_bps;
    out.queue.submitted_with_queue =
        projection.queue_submitted_with_queue;
    out.queue.filled_after_drain = projection.queue_filled_after_drain;
    out.queue.blocked_at_eos = projection.queue_blocked_at_eos;

    std::vector<truetest::exits::ExitManager::fixed_venue_handle_view>
        venue_rows(projection.brackets_state.count);
    const auto venue_result = exit_manager_.snapshot_venue_handles_into(
        std::span{venue_rows});
    out.brackets.reserve(projection.brackets_state.count);
    for (std::size_t i = 0; i < projection.brackets_state.count; ++i)
    {
        const auto& source = projection.brackets[i];
        snapshot_t::bracket_row row;
        row.opener_order_id = source.opener_order_id;
        row.strategy_name = source.strategy_name.view();
        row.symbol = source.symbol.view();
        row.side = source.close_side == order_side::sell ? 'L' : 'S';
        row.qty = source.qty;
        row.entry_price = source.entry_price;
        row.stop_loss = source.stop_loss;
        row.take_profit = source.take_profit;
        row.mark = mark_for(source.symbol.view());
        row.age_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            capture_time - source.ts_armed).count();
        for (std::size_t j = 0; j < venue_result.count; ++j)
        {
            if (venue_rows[j].opener_order_id != row.opener_order_id) continue;
            row.venue_managed = venue_rows[j].venue_managed;
            row.venue_list_id.assign(venue_rows[j].list_id.data(),
                                     venue_rows[j].list_id_size);
            break;
        }
        out.brackets.push_back(std::move(row));
    }

    std::vector<std::string> symbols;
    symbols.reserve(projection.market_symbols_state.count);
    for (std::size_t i = 0;
         i < projection.market_symbols_state.count; ++i)
        symbols.emplace_back(projection.market_symbols[i].view());
    std::sort(symbols.begin(), symbols.end());

    out.l2.symbol = projection.active_symbol.view();
    out.market_rows.reserve(symbols.size());
    for (const auto& symbol : symbols)
    {
        snapshot_t::market_row row;
        row.symbol = symbol;
        row.mark = mark_for(symbol);
        row.mark_available = row.mark > 0.0;
        for (std::size_t i = 0; i < projection.positions_state.count; ++i)
            if (projection.positions[i].symbol.view() == symbol)
                row.position_qty = projection.positions[i].qty;
        for (std::size_t i = 0; i < projection.open_orders_state.count; ++i)
        {
            const auto& order = projection.open_orders[i];
            if (order.symbol.view() != symbol) continue;
            if (order.side == 'B') ++row.working_buy_orders;
            else if (order.side == 'S') ++row.working_sell_orders;
        }

        const dash::book_row* book = nullptr;
        for (std::size_t i = 0; i < projection.books_state.count; ++i)
            if (projection.books[i].symbol.view() == symbol)
            {
                book = &projection.books[i];
                break;
            }
        if (book && book->quantity_valid)
        {
            if (book->bid_count != 0U)
            {
                row.best_bid = book->bids[0].price;
                row.best_bid_available = row.best_bid > 0.0;
            }
            if (book->ask_count != 0U)
            {
                row.best_ask = book->asks[0].price;
                row.best_ask_available = row.best_ask > 0.0;
            }
            double bid_depth = 0.0;
            double ask_depth = 0.0;
            for (std::size_t i = 0; i < book->bid_count; ++i)
                bid_depth += book->bids[i].size;
            for (std::size_t i = 0; i < book->ask_count; ++i)
                ask_depth += book->asks[i].size;
            if (row.best_bid_available && row.best_ask_available)
            {
                row.bbo_available = true;
                row.mid = (row.best_bid + row.best_ask) * 0.5;
                row.spread = row.best_ask - row.best_bid;
                row.spread_bps = row.mid > 0.0
                    ? row.spread / row.mid * 1e4 : 0.0;
                const auto top_size = book->bids[0].size + book->asks[0].size;
                if (top_size > 0.0)
                {
                    row.microprice =
                        (book->bids[0].size * row.best_ask +
                         book->asks[0].size * row.best_bid) / top_size;
                    row.microprice_available = true;
                }
            }
            if (bid_depth + ask_depth > 0.0)
            {
                row.imbalance = (bid_depth - ask_depth) /
                                (bid_depth + ask_depth);
                row.imbalance_available = true;
            }

            if (symbol == out.l2.symbol)
            {
                out.l2.total_bid_levels = book->total_bid_levels;
                out.l2.total_ask_levels = book->total_ask_levels;
                double cumulative = 0.0;
                for (std::size_t i = 0; i < book->bid_count; ++i)
                {
                    cumulative += book->bids[i].size;
                    out.l2.bids.push_back(
                        {book->bids[i].price, book->bids[i].size, cumulative});
                }
                out.l2.cum_bid_size = cumulative;
                cumulative = 0.0;
                for (std::size_t i = 0; i < book->ask_count; ++i)
                {
                    cumulative += book->asks[i].size;
                    out.l2.asks.push_back(
                        {book->asks[i].price, book->asks[i].size, cumulative});
                }
                out.l2.cum_ask_size = cumulative;
                out.l2.best_bid = row.best_bid;
                out.l2.best_ask = row.best_ask;
                out.l2.mid = row.mid;
                out.l2.spread_bps = row.spread_bps;
                out.l2.microprice = row.microprice;
                out.l2.imbalance = row.imbalance;
                out.l2.source = book->venue_seeded
                    ? snapshot_t::l2_source::venue
                    : ((book->bid_count != 0U || book->ask_count != 0U)
                        ? snapshot_t::l2_source::synthetic
                        : snapshot_t::l2_source::none);
            }
        }
        out.market_rows.push_back(std::move(row));
    }

    auto& debug_view = out.debug;
    debug_view.target = "(engine_core)";
    switch (config_.mode)
    {
        case engine_mode::backtest: debug_view.mode = "backtest"; break;
        case engine_mode::shadow: debug_view.mode = "shadow"; break;
        case engine_mode::live: debug_view.mode = "live"; break;
    }
#ifdef HAS_BINANCE
    debug_view.has_binance = true;
#endif
#ifdef HAS_QUESTDB
    debug_view.has_questdb = true;
#endif
#ifdef HAS_DEBUG
    debug_view.has_debug = true;
#endif
#ifdef TRUETEST_VENUE_DATA_COMPILED
    debug_view.has_live_data = true;
#endif
    debug_view.preset = preset_to_string(config_.threading);
    debug_view.spin_policy = spin_policy_to_string(config_.worker_spin_policy);
    debug_view.cpu_pin = !config_.disable_pinning;

    const auto add_ring = [&debug_view](const char* name, const auto& ring) {
        snapshot_t::ring_stat row;
        row.name = name;
        if (ring)
        {
            row.size = ring->size();
            row.hwm = ring->high_watermark();
            row.capacity = ring->capacity();
            row.drops = ring->drop_count();
            ++debug_view.worker_count;
        }
        debug_view.rings.push_back(row);
    };
    add_ring("logging", logging_ring_);
    add_ring("risk", risk_ring_);
    add_ring("stats", stats_ring_);
    add_ring("observer", observer_ring_);
    add_ring("risk_stats", risk_stats_ring_);
    add_ring("mm_event", mm_ring_);
    debug_view.rings.push_back(snapshot_t::ring_stat{"mm_order"});

    constexpr std::size_t kPoolBlock = 4096;
    const auto add_pool = [&debug_view](const char* name, const auto& pool) {
        debug_view.pools.push_back(snapshot_t::pool_stat{
            name, pool.block_count(), kPoolBlock,
            pool.block_count() * kPoolBlock, pool.in_use(),
            pool.grow_count()});
    };
    add_pool("market_pool", market_pool_);
    add_pool("order_pool", order_pool_);
    add_pool("fill_pool", fill_pool_);
    add_pool("tick_pool", tick_pool_);
    add_pool("l2_update_pool", l2_update_pool_);
    add_pool("l2_snapshot_pool", l2_snapshot_pool_);
    add_pool("rejection_pool", rejection_pool_);
    add_pool("cancel_pool", cancel_pool_);
    add_pool("amend_pool", amend_pool_);
    add_pool("funding_pool", funding_pool_);
    add_pool("control_block_pool", control_block_pool_);

    if (projection.engine_counts_available)
    {
        debug_view.pending_orders = projection.engine_counts.pending_orders;
        debug_view.pending_orders_available = true;
        debug_view.pending_stops = projection.engine_counts.pending_stops;
        debug_view.pending_stops_available = true;
        debug_view.order_meta_size = projection.engine_counts.order_meta_size;
        debug_view.order_meta_size_available = true;
    }
    debug_view.open_orders_cache = projection.open_orders_cache_size;
    debug_view.armed_brackets = projection.exit_armed;
    debug_view.exit_pending = projection.exit_pending;
    debug_view.exit_armed = projection.exit_armed;
    debug_view.handles_size = venue_result.total_count;
    debug_view.exit_exchange_to_leg = exit_manager_.exchange_leg_count();

#ifdef HAS_DEBUG
    debug_view.stages.reserve(projection.stages_state.count);
    for (std::size_t i = 0; i < projection.stages_state.count; ++i)
    {
        const auto& row = projection.stages[i];
        const auto stage = static_cast<debug::stage>(row.index);
        debug_view.stages.push_back({
            debug::StageTimer::stage_name(stage), row.calls,
            row.calls != 0U ? row.total_ns / row.calls : 0U,
            row.min_ns, row.max_ns});
    }
#endif

    debug_view.errors.push_back({"engine", ""});
    debug_view.errors.push_back({
        "bridge",
        "unavailable: no synchronized immutable adapter error source"});
    debug_view.errors.push_back({
        "provider",
        projection.provider_present && !projection.provider_state_available
            ? "unavailable: provider lifecycle is not safe to read concurrently"
            : ""});
    debug_view.errors.push_back({
        "adapter",
        projection.queue_diagnostics_available
            ? ""
            : "unavailable: adapter diagnostics require a quiescent projection"});
#ifdef HAS_QUESTDB
    debug_view.errors.push_back({
        "questdb",
        config_.persist_enabled
            ? "unavailable: no synchronized immutable persistence health source"
            : ""});
#else
    debug_view.errors.push_back({"questdb", ""});
#endif
    debug_view.errors.push_back({
        "dashboard_projection",
        projection.complete && venue_result.complete
            ? ""
            : "bounded dashboard projection truncated; collection counts are incomplete"});
    debug_view.errors.push_back({
        "dashboard_analytics",
        projection.analytics_available
            ? ""
            : "threaded analytics not quiescent; metrics await terminal projection"});

    out.health.tick_to_trade_samples = projection.latency.samples;
    out.health.avg_tick_to_trade_us = projection.latency.avg_ns / 1000.0;
    out.health.min_tick_to_trade_us =
        static_cast<double>(projection.latency.min_ns) / 1000.0;
    out.health.max_tick_to_trade_us =
        static_cast<double>(projection.latency.max_ns) / 1000.0;
    out.health.orders_total = projection.total_orders;
    out.health.fills_total = projection.total_fills;
    out.health.trades_total = projection.total_trades;
    out.health.provider_present = projection.provider_present;
    out.health.provider_name = projection.provider_name.view();
    out.health.provider_state = projection.provider_state_available
        ? projection.provider_state : -1;

    // The current audit sink's active bit is not synchronized with UI/web
    // readers.  Do not read or fabricate live counters here; the debug row
    // above makes unavailability explicit when persistence was requested.
    out.health.questdb.active = false;
    out.health.questdb.connected = false;
    out.health.questdb.pending_lines = 0;
    out.health.questdb.dropped_lines = 0;
    out.health.questdb.fallback_lines = 0;
#ifdef HAS_QUESTDB
    out.health.questdb.strict_mode = config_.questdb_strict;
#else
    out.health.questdb.strict_mode = false;
#endif
    out.health.questdb.last_flush_age_ms = -1;

    out.strategies.reserve(projection.strategies_state.count);
    for (std::size_t i = 0; i < projection.strategies_state.count; ++i)
    {
        const auto& source = projection.strategies[i];
        snapshot_t::strategy_row row;
        row.name = source.name.view();
        row.pnl = source.pnl;
        row.trade_count = source.trade_count;
        row.win_count = source.win_count;
        row.win_rate = source.trade_count != 0U
            ? static_cast<double>(source.win_count) /
              static_cast<double>(source.trade_count) * 100.0 : 0.0;
        row.profit_factor = source.total_loss > 0.0
            ? source.total_win / source.total_loss
            : (source.total_win > 0.0 ? 1e9 : 0.0);
        row.total_win = source.total_win;
        row.total_loss = source.total_loss;
        for (const auto& lot : out.lots)
            if (lot.strategy_name == row.name) ++row.open_lots;
        for (const auto& bracket : out.brackets)
            if (bracket.strategy_name == row.name) ++row.armed_brackets;
        out.strategies.push_back(std::move(row));
    }

    out.trend.equity_tail.assign(
        projection.equity_tail.begin(),
        projection.equity_tail.begin() +
            static_cast<std::ptrdiff_t>(projection.equity_tail_count));
    out.trend.drawdown_tail.assign(
        projection.drawdown_tail.begin(),
        projection.drawdown_tail.begin() +
            static_cast<std::ptrdiff_t>(projection.drawdown_tail_count));
    out.trend.equity_now = out.equity;
    out.trend.equity_available = out.equity_available;
    out.trend.equity_change_pct =
        out.equity_available && out.initial_balance > 0.0
        ? (out.equity / out.initial_balance - 1.0) * 100.0 : 0.0;
    out.trend.drawdown_now_pct = out.trend.drawdown_tail.empty()
        ? 0.0 : out.trend.drawdown_tail.back();
    out.trend.drawdown_now_available = !out.trend.drawdown_tail.empty();
    out.generated_at = projection.generated_at;
    out.generated_at_available = true;
}
