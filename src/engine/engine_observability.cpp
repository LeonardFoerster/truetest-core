// Engine observability: dashboard snapshot delegation, console summary
// report, and small read-only accessors used by tests/TUI/web.
// Extracted mechanically from engine.cpp (Phase 1 TU split); behavior unchanged.
#include "engine.h"
#include "ui/console_dashboard.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

bool engine::snapshot_dashboard(truetest::ui::dashboard_snapshot& out) const
{
    if (dashboard_builder_) return dashboard_builder_->snapshot_dashboard(out);
    return false;
}

void engine::request_dashboard_refresh()
{
    if (dashboard_builder_) dashboard_builder_->request_dashboard_refresh();
}

void engine::print_summary()
{
    switch (config_.threading)
    {
    case thread_preset::inline_mode:
    case thread_preset::logging_only:
        analytics_.print_report();
        return;
    case thread_preset::light:
        if (observer_worker_)
            observer_worker_->analytics().print_report();
        return;
    case thread_preset::standard:
        if (risk_stats_worker_)
            risk_stats_worker_->analytics().print_report();
        return;
    case thread_preset::full:
    case thread_preset::extended:
        if (stats_worker_)
            stats_worker_->analytics().print_report();
        break;
    }

    if (shadow_tracker_)
        shadow_tracker_->print_report();

    // Queue-position telemetry (shadow + --queue-model l2-snapshot only).
    // Use the IExecutionAdapter virtuals (implemented by TradeTapeShadowAdapter
    // and others); no concrete cast required.
    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exec = config_.provider->get_execution_adapter();
        if (exec)
        {
            const auto submitted = exec->queue_submitted_with_queue();
            if (submitted > 0)
            {
                std::cout << "  Queue model (shadow):\n"
                          << "    Submitted with queue ahead: " << submitted << "\n"
                          << "    Filled after queue drained: " << exec->queue_filled_after_drain() << "\n"
                          << "    Still queue-blocked at EOS: " << exec->queue_blocked_at_eos() << "\n";
            }
        }
    }

    // Maker queue telemetry (paper/backtest + --maker-queue-model).
    {
        std::size_t total_live = 0;
        double      total_qpos = 0.0;
        int         n = 0;

        for (auto& [_, ad] : execution_adapters_)
        {
            if (ad)
            {
                auto c = ad->live_quote_count();
                if (c > 0)
                {
                    total_live += c;
                    total_qpos += ad->avg_queue_position_bps();
                    ++n;
                }
            }
        }
        if (config_.provider)
        {
            auto pa = config_.provider->get_execution_adapter();
            if (pa)
            {
                auto c = pa->live_quote_count();
                if (c > 0)
                {
                    total_live += c;
                    total_qpos += pa->avg_queue_position_bps();
                    ++n;
                }
            }
        }

        if (total_live > 0)
        {
            uint32_t avg_bps = static_cast<uint32_t>(total_qpos / n);
            std::cout << "  Maker queue model:\n"
                      << "    Live passive limits: " << total_live << "\n"
                      << "    Avg queue position:  " << (avg_bps / 100) << "%\n";
            // Phase 2 richer stats (mainly for shadow TradeTape)
            auto pa = config_.provider ? config_.provider->get_execution_adapter() : nullptr;
            if (pa) {
                auto sub = pa->queue_submitted_with_queue();
                auto fil = pa->queue_filled_after_drain();
                auto blk = pa->queue_blocked_at_eos();
                if (sub > 0 || fil > 0 || blk > 0) {
                    std::cout << "    Queue detailed (shadow): submitted=" << sub
                              << " filled_after_drain=" << fil
                              << " blocked_at_eos=" << blk << "\n";
                }
            }
        }
    }

    // Dual Portfolio Shadow Report (Phase 2 - text version for non-TUI runs)
    if (config_.mode == engine_mode::shadow)
    {
        const portfolio* exch = get_exchange_portfolio();
        const Analytics* exch_analytics = get_exchange_analytics();

        if (exch && exch_analytics)
        {
            double last_price = (last_mid_price_.load(std::memory_order_relaxed) > 0.0) ? last_mid_price_.load(std::memory_order_relaxed) : 0.0;

            double sim_equity, exch_equity;
            {
                std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                sim_equity  = portfolio_.get_equity(last_mark_prices_, last_price);
                exch_equity = exch->get_equity(last_mark_prices_, last_price);
            }
            double delta        = exch_equity - sim_equity;
            double delta_pct    = (sim_equity > 0.0) ? (delta / sim_equity * 100.0) : 0.0;

            std::cout << "\n";
            std::cout << "  ============================================\n";
            std::cout << "    Dual Portfolio Shadow Report (Text)\n";
            std::cout << "  ============================================\n";
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "    Sim Equity:      $" << sim_equity << "\n";
            std::cout << "    Exchange Equity: $" << exch_equity << "\n";
            std::cout << "    Delta:           $" << delta 
                      << " (" << (delta >= 0 ? "+" : "") << delta_pct << "%)\n";
            std::cout << "    Sim Cash:        $" << portfolio_.get_cash() << "\n";
            std::cout << "    Exchange Cash:   $" << exch->get_cash() << "\n";
        }
    }
}

const Analytics& engine::get_analytics() const
{
    switch (config_.threading)
    {
    case thread_preset::light:
        if (observer_worker_) return observer_worker_->analytics();
        break;
    case thread_preset::standard:
        if (risk_stats_worker_) return risk_stats_worker_->analytics();
        break;
    case thread_preset::full:
    case thread_preset::extended:
        if (stats_worker_) return stats_worker_->analytics();
        break;
    default:
        break;
    }
    return analytics_;
}

void engine::set_data_rows_rejected(std::size_t n)
{
    // Stored engine-local and on analytics_; folded into worker export analytics
    // at stop_workers (after join) so threaded get_analytics() is honest.
    data_rows_rejected_ = n;
    analytics_.set_data_rows_rejected(n);
}

const portfolio* engine::get_exchange_portfolio() const
{
    if (config_.mode != engine_mode::shadow || !exchange_portfolio_.has_value())
        return nullptr;
    return &exchange_portfolio_.value();
}

const Analytics* engine::get_exchange_analytics() const
{
    if (config_.mode != engine_mode::shadow || !exchange_analytics_.has_value())
        return nullptr;
    return &exchange_analytics_.value();
}

void engine::write_adapter_diagnostics(truetest::ui::streaming_stats& st)
{
    std::uint32_t live = 0;
    std::uint64_t queue_sum = 0;
    std::uint32_t queue_n   = 0;
    auto collect = [&](IExecutionAdapter* a) {
        if (!a) return;
        const auto c = a->live_quote_count();
        if (c == 0) return;
        live      += static_cast<std::uint32_t>(c);
        queue_sum += a->avg_queue_position_bps();
        ++queue_n;
    };
    for (auto& [_, ad] : execution_adapters_)
        collect(ad.get());
    if (config_.provider)
        collect(config_.provider->get_execution_adapter().get());

    st.live_quotes.store(live, std::memory_order_relaxed);
    st.avg_queue_pos_bps.store(
        queue_n > 0 ? static_cast<std::uint32_t>(queue_sum / queue_n) : 0u,
        std::memory_order_relaxed);
}
