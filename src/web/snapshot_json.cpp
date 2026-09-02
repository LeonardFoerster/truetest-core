#include "snapshot_json.h"

#include "json_emit.h"
#include "../ui/dashboard_snapshot.h"

#include <chrono>
#include <cmath>
#include <string_view>

namespace truetest::web {

using truetest::ui::dashboard_snapshot;
using jx::Json;

namespace {

// A dashboard_snapshot timestamps itself with steady_clock so in-process
// consumers cannot be fooled by wall-clock adjustments.  The web boundary
// needs a Unix timestamp, however, so project the captured steady-clock age
// onto system_clock at serialization time. A future monotonic timestamp is
// internally inconsistent and is therefore published as unavailable.

long long ts_ms(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

struct generated_at_wire
{
    long long epoch_ms = 0;
    bool available = false;
};

generated_at_wire project_generated_at(const dashboard_snapshot& snapshot)
{
    if (!snapshot.generated_at_available) return {};

    const auto steady_now = std::chrono::steady_clock::now();
    if (snapshot.generated_at > steady_now) return {};

    const auto age = snapshot.generated_at < steady_now
        ? steady_now - snapshot.generated_at
        : std::chrono::steady_clock::duration::zero();
    const auto generated_system =
        std::chrono::system_clock::now() -
        std::chrono::duration_cast<std::chrono::system_clock::duration>(age);
    return {.epoch_ms = ts_ms(generated_system), .available = true};
}

const char* l2_source_name(dashboard_snapshot::l2_source s)
{
    switch (s)
    {
        case dashboard_snapshot::l2_source::synthetic: return "synthetic";
        case dashboard_snapshot::l2_source::venue:     return "venue";
        case dashboard_snapshot::l2_source::none:      default: return "none";
    }
}

void write_levels(Json& j, const std::vector<dashboard_snapshot::l2_level>& lv)
{
    j.arr();
    for (const auto& l : lv)
    {
        j.obj().kv("price", l.price).kv("size", l.size).kv("cum", l.cum).endobj();
    }
    j.endarr();
}

bool metric_available(bool declared_available, double value) noexcept
{
    return declared_available && std::isfinite(value);
}

void write_metric(Json& j,
                  const char* value_key,
                  double value,
                  const char* availability_key,
                  bool declared_available)
{
    const bool available = metric_available(declared_available, value);
    j.key(value_key);
    if (available) j.num(value);
    else j.null();
    j.kv(availability_key, available);
}

const char* fill_source_name(const char* source) noexcept
{
    const std::string_view value = source ? std::string_view{source} : std::string_view{};
    if (value == "exchange") return "exchange";
    if (value == "simulated") return "simulated";
    return "unknown";
}

} // namespace

std::string snapshot_to_json(const dashboard_snapshot& s)
{
    std::string out;
    out.reserve(8192);
    Json j(out);

    j.obj();
    j.kv("schema_version", static_cast<long long>(snapshot_schema_version));
    const auto generated_at = project_generated_at(s);
    j.key("generated_at_ms");
    if (generated_at.available) j.inum(generated_at.epoch_ms);
    else j.null();
    j.kv("generated_at_available", generated_at.available);

    // ---- account ----
    j.key("account").obj()
        .kv("cash", s.cash)
        .kv("initial_balance", s.initial_balance);
    write_metric(j, "equity", s.equity, "equity_available", s.equity_available);
    write_metric(j, "total_pnl", s.total_pnl, "total_pnl_available", s.total_pnl_available);
    write_metric(j, "realized_pnl", s.realized_pnl,
                 "realized_pnl_available", s.realized_pnl_available);
    write_metric(j, "unrealized_pnl", s.unrealized_pnl,
                 "unrealized_pnl_available", s.unrealized_pnl_available);
    j.endobj();

    // ---- positions ----
    j.key("positions").arr();
    for (const auto& p : s.positions)
    {
        j.obj()
            .kv("symbol", p.symbol)
            .kv("qty", p.qty)
            .kv("avg_entry", p.avg_entry);
        write_metric(j, "mark", p.mark, "mark_available", p.mark_available);
        write_metric(j, "unrealized", p.unrealized,
                     "unrealized_available", p.unrealized_available);
        j.endobj();
    }
    j.endarr();

    // ---- lots ----
    j.key("lots").arr();
    for (const auto& l : s.lots)
    {
        j.obj()
            .kv("opener_order_id", static_cast<unsigned long long>(l.opener_order_id))
            .kv("symbol", l.symbol)
            .kv("strategy_name", l.strategy_name)
            .kv_char("side", l.side)
            .kv("qty_open", l.qty_open)
            .kv("entry_price", l.entry_price)
            .kv("age_seconds", static_cast<long long>(l.age_seconds))
            .endobj();
    }
    j.endarr();

    // ---- open orders ----
    j.key("open_orders").arr();
    for (const auto& o : s.open_orders)
    {
        j.obj()
            .kv("order_id", static_cast<unsigned long long>(o.order_id))
            .kv("symbol", o.symbol)
            .kv("strategy_name", o.strategy_name)
            .kv_char("side", o.side)
            .kv_char("type", o.type)
            .kv("qty", o.qty)
            .kv("price", o.price);
        write_metric(j, "trigger_price", o.trigger_price,
                     "trigger_price_available", o.trigger_price_available);
        j.kv("age_seconds", static_cast<long long>(o.age_seconds))
            .kv("status", o.status ? o.status : "")
            .endobj();
    }
    j.endarr();

    // ---- recent fills (newest first) ----
    j.key("recent_fills").arr();
    for (const auto& f : s.recent_fills)
    {
        j.obj()
            .kv("ts_ms", ts_ms(f.ts))
            .kv("symbol", f.symbol)
            .kv_char("side", f.side)
            .kv("qty", f.qty)
            .kv("price", f.price)
            .kv("fee", f.fee)
            .kv("source", fill_source_name(f.source))
            .endobj();
    }
    j.endarr();

    // ---- brackets (armed SL/TP) ----
    j.key("brackets").arr();
    for (const auto& b : s.brackets)
    {
        j.obj()
            .kv("opener_order_id", static_cast<unsigned long long>(b.opener_order_id))
            .kv("strategy_name", b.strategy_name)
            .kv("symbol", b.symbol)
            .kv_char("side", b.side)
            .kv("qty", b.qty)
            .kv("entry_price", b.entry_price);
        if (b.stop_loss)   j.kv("stop_loss", *b.stop_loss);   else j.key("stop_loss").null();
        if (b.take_profit) j.kv("take_profit", *b.take_profit); else j.key("take_profit").null();
        j.kv("mark", b.mark)
            .kv("venue_managed", b.venue_managed)
            .kv("venue_list_id", b.venue_list_id)
            .kv("age_seconds", static_cast<long long>(b.age_seconds))
            .endobj();
    }
    j.endarr();

    // ---- strategies ----
    j.key("strategies").arr();
    for (const auto& st : s.strategies)
    {
        j.obj()
            .kv("name", st.name)
            .kv("pnl", st.pnl)
            .kv("trade_count", st.trade_count)
            .kv("win_count", st.win_count)
            .kv("win_rate", st.win_rate)
            .kv("profit_factor", st.profit_factor)
            .kv("total_win", st.total_win)
            .kv("total_loss", st.total_loss)
            .kv("open_lots", st.open_lots)
            .kv("armed_brackets", st.armed_brackets)
            .endobj();
    }
    j.endarr();

    // ---- risk ----
    j.key("risk").obj()
        .kv("halted", s.risk.halted);
    write_metric(j, "daily_loss", s.risk.daily_loss,
                 "daily_loss_available", s.risk.daily_loss_available);
    j.kv("daily_loss_limit", s.risk.daily_loss_limit);
    write_metric(j, "max_drawdown_pct", s.risk.max_drawdown_pct,
                 "max_drawdown_available", s.risk.max_drawdown_available);
    j.kv("max_drawdown_limit", s.risk.max_drawdown_limit);
    write_metric(j, "exposure", s.risk.exposure,
                 "exposure_available", s.risk.exposure_available);
    j.kv("exposure_limit", s.risk.exposure_limit)
        .kv("open_orders", s.risk.open_orders)
        .kv("open_orders_limit", s.risk.open_orders_limit)
        .endobj();

    // ---- perf ----
    j.key("perf").obj()
        .kv("total_orders", s.perf.total_orders)
        .kv("total_fills", s.perf.total_fills)
        .kv("total_trades", s.perf.total_trades)
        .kv("win_rate", s.perf.win_rate)
        .kv("sharpe", s.perf.sharpe)
        .kv("sortino", s.perf.sortino)
        .kv("profit_factor", s.perf.profit_factor)
        .kv("avg_markout_bps", s.perf.avg_markout_bps)
        .kv("markout_samples", s.perf.markout_samples)
        .endobj();

    // ---- L2 ladder ----
    j.key("l2").obj()
        .kv("symbol", s.l2.symbol)
        .kv("source", l2_source_name(s.l2.source))
        .kv("total_bid_levels", s.l2.total_bid_levels)
        .kv("total_ask_levels", s.l2.total_ask_levels);
    j.key("bids"); write_levels(j, s.l2.bids);
    j.key("asks"); write_levels(j, s.l2.asks);
    j.kv("best_bid", s.l2.best_bid)
        .kv("best_ask", s.l2.best_ask)
        .kv("mid", s.l2.mid)
        .kv("spread_bps", s.l2.spread_bps)
        .kv("microprice", s.l2.microprice)
        .kv("imbalance", s.l2.imbalance)
        .kv("cum_bid_size", s.l2.cum_bid_size)
        .kv("cum_ask_size", s.l2.cum_ask_size)
        .endobj();

    // ---- health ----
    const auto& h = s.health;
    j.key("health").obj()
        .kv("avg_tick_to_trade_us", h.avg_tick_to_trade_us)
        .kv("min_tick_to_trade_us", h.min_tick_to_trade_us)
        .kv("max_tick_to_trade_us", h.max_tick_to_trade_us)
        .kv("tick_to_trade_samples", h.tick_to_trade_samples)
        .kv("events_total", h.events_total)
        .kv("fills_total", h.fills_total)
        .kv("orders_total", h.orders_total)
        .kv("trades_total", h.trades_total)
        .kv("ring_drops_logging", h.ring_drops_logging)
        .kv("ring_drops_risk", h.ring_drops_risk)
        .kv("ring_drops_stats", h.ring_drops_stats)
        .kv("ring_drops_observer", h.ring_drops_observer)
        .kv("ring_drops_risk_stats", h.ring_drops_risk_stats)
        .kv("ring_drops_mm", h.ring_drops_mm)
        .kv("provider_present", h.provider_present)
        .kv("provider_name", h.provider_name)
        .kv("provider_state", h.provider_state)
        .kv("rate_ev_per_sec", h.rate_ev_per_sec);
    j.key("questdb").obj()
        .kv("active", h.questdb.active)
        .kv("connected", h.questdb.connected)
        .kv("pending_lines", h.questdb.pending_lines)
        .kv("dropped_lines", h.questdb.dropped_lines)
        .kv("fallback_lines", h.questdb.fallback_lines)
        .kv("last_flush_age_ms", static_cast<long long>(h.questdb.last_flush_age_ms))
        .kv("strict_mode", h.questdb.strict_mode)
        .endobj();
    j.endobj();

    // ---- trend (sparkline tails) ----
    auto write_tail = [&](const char* k, const std::vector<double>& v) {
        j.key(k).arr();
        for (double x : v) j.num(x);
        j.endarr();
    };
    j.key("trend").obj();
    write_tail("equity_tail", s.trend.equity_tail);
    write_tail("drawdown_tail", s.trend.drawdown_tail);
    write_tail("rate_tail", s.trend.rate_tail);
    write_metric(j, "equity_now", s.trend.equity_now,
                 "equity_available", s.trend.equity_available);
    write_metric(j, "equity_change_pct", s.trend.equity_change_pct,
                 "equity_change_available",
                 s.trend.equity_available &&
                     std::isfinite(s.initial_balance) &&
                     s.initial_balance > 0.0);
    write_metric(j, "drawdown_now_pct", s.trend.drawdown_now_pct,
                 "drawdown_now_available", s.trend.drawdown_now_available);
    j.kv("rate_now", s.trend.rate_now)
        .endobj();

    // ---- queue position ----
    j.key("queue").obj();
    j.key("avg_bps");
    if (s.queue.available) j.unum(s.queue.avg_bps);
    else j.null();
    j.kv("available", s.queue.available)
        .kv("submitted_with_queue", s.queue.submitted_with_queue)
        .kv("filled_after_drain", s.queue.filled_after_drain)
        .kv("blocked_at_eos", s.queue.blocked_at_eos)
        .endobj();

    // ---- memory (v2+) — /proc + in-process pool/ring footprints ----
    {
        const auto& m = s.memory;
        j.key("memory").obj()
            .kv("available", m.available)
            .kv("rss_bytes", static_cast<unsigned long long>(m.rss_bytes))
            .kv("vm_bytes", static_cast<unsigned long long>(m.vm_bytes))
            .kv("peak_rss_bytes", static_cast<unsigned long long>(m.peak_rss_bytes))
            .kv("heap_bytes", static_cast<unsigned long long>(m.heap_bytes))
            .kv("pool_bytes_total", static_cast<unsigned long long>(m.pool_bytes_total))
            .kv("ring_bytes_total", static_cast<unsigned long long>(m.ring_bytes_total));
        j.key("pools").arr();
        for (const auto& p : m.pools)
        {
            j.obj()
                .kv("name", p.name ? p.name : "")
                .kv("blocks", p.blocks)
                .kv("slot_size", p.slot_size)
                .kv("bytes", static_cast<unsigned long long>(p.bytes))
                .kv("in_use", p.in_use)
                .kv("capacity_slots", p.capacity_slots)
                .kv("grow_count", p.grow_count)
                .endobj();
        }
        j.endarr();
        j.key("rings").arr();
        for (const auto& r : m.rings)
        {
            j.obj()
                .kv("name", r.name ? r.name : "")
                .kv("capacity", r.capacity)
                .kv("element_bytes", r.element_bytes)
                .kv("bytes", static_cast<unsigned long long>(r.bytes))
                .endobj();
        }
        j.endarr();
        j.endobj();
    }

    // ---- debug (v2+) — workers, rings, pools, mode (cold-path only) ----
    {
        const auto& d = s.debug;
        j.key("debug").obj()
            .kv("target", d.target)
            .kv("mode", d.mode)
            .kv("has_binance", d.has_binance)
            .kv("has_questdb", d.has_questdb)
            .kv("has_debug", d.has_debug)
            .kv("has_live_data", d.has_live_data)
            .kv("preset", d.preset)
            .kv("worker_count", d.worker_count)
            .kv("cpu_pin", d.cpu_pin)
            .kv("spin_policy", d.spin_policy)
            .kv("event_count", static_cast<unsigned long long>(d.event_count))
            .kv("pending_orders", d.pending_orders)
            .kv("open_orders_cache", d.open_orders_cache)
            .kv("armed_brackets", d.armed_brackets);
        j.key("rings").arr();
        for (const auto& r : d.rings)
        {
            j.obj()
                .kv("name", r.name ? r.name : "")
                .kv("size", r.size)
                .kv("hwm", r.hwm)
                .kv("capacity", r.capacity)
                .kv("drops", static_cast<unsigned long long>(r.drops))
                .endobj();
        }
        j.endarr();
        j.key("pools").arr();
        for (const auto& p : d.pools)
        {
            j.obj()
                .kv("name", p.name ? p.name : "")
                .kv("blocks", p.blocks)
                .kv("block_size", p.block_size)
                .kv("capacity", p.capacity)
                .kv("in_use", p.in_use)
                .kv("grow_count", p.grow_count)
                .endobj();
        }
        j.endarr();
        j.key("errors").arr();
        for (const auto& e : d.errors)
        {
            j.obj()
                .kv("name", e.name ? e.name : "")
                .kv("msg", e.msg)
                .endobj();
        }
        j.endarr();
        // Stage timings only when present (HAS_DEBUG builds); keep poll payloads small.
        if (!d.stages.empty())
        {
            j.key("stages").arr();
            for (const auto& st : d.stages)
            {
                j.obj()
                    .kv("name", st.name ? st.name : "")
                    .kv("calls", static_cast<unsigned long long>(st.calls))
                    .kv("avg_ns", static_cast<unsigned long long>(st.avg_ns))
                    .kv("min_ns", static_cast<unsigned long long>(st.min_ns))
                    .kv("max_ns", static_cast<unsigned long long>(st.max_ns))
                    .endobj();
            }
            j.endarr();
        }
        j.endobj();
    }

    j.endobj();
    return out;
}

} // namespace truetest::web
