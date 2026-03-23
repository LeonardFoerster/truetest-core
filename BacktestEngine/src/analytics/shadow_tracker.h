#pragma once

#include "../core/event.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <string>
#include <unordered_map>
#include <vector>

// ShadowTracker: compares simulated fills (from local orderbook) against
// exchange fills (from live execution adapter) to measure slippage,
// fill rate differences, and latency.
//
// Used in shadow mode: the engine submits orders to both the local book
// and the exchange adapter, then feeds both sets of fills to this tracker.
struct shadow_fill
{
    uint64_t order_id;
    std::string symbol;
    order_side side;

    // Simulated fill (from local orderbook)
    double sim_price = 0.0;
    double sim_quantity = 0.0;
    std::chrono::system_clock::time_point sim_timestamp;
    bool sim_filled = false;

    // Exchange fill (from live/paper executor)
    double exchange_price = 0.0;
    double exchange_quantity = 0.0;
    std::chrono::system_clock::time_point exchange_timestamp;
    bool exchange_filled = false;

    // Computed metrics
    double slippage() const
    {
        if (!sim_filled || !exchange_filled) return 0.0;
        return exchange_price - sim_price;
    }

    double slippage_bps() const
    {
        if (!sim_filled || !exchange_filled || sim_price == 0.0) return 0.0;
        return (slippage() / sim_price) * 10000.0;
    }

    int64_t latency_ms() const
    {
        if (!sim_filled || !exchange_filled) return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            exchange_timestamp - sim_timestamp).count();
    }
};

class ShadowTracker
{
public:
    void on_simulated_fill(const fill_event& f)
    {
        auto& entry = get_or_create(f.get_order_id());
        entry.sim_price = f.get_fill_price();
        entry.sim_quantity = f.get_filled_quantity();
        entry.sim_timestamp = f.get_timestamp();
        entry.sim_filled = true;
        entry.symbol = f.get_symbol();
        entry.side = f.get_side();
    }

    void on_exchange_fill(const fill_event& f)
    {
        auto& entry = get_or_create(f.get_order_id());
        entry.exchange_price = f.get_fill_price();
        entry.exchange_quantity = f.get_filled_quantity();
        entry.exchange_timestamp = f.get_timestamp();
        entry.exchange_filled = true;
        entry.symbol = f.get_symbol();
        entry.side = f.get_side();
    }

    void print_report() const
    {
        std::cout << "\n";
        std::cout << "  ============================================\n";
        std::cout << "    Shadow Mode Report\n";
        std::cout << "  ============================================\n";

        size_t total = fills_.size();
        size_t both_filled = 0;
        size_t sim_only = 0;
        size_t exchange_only = 0;
        double total_slippage = 0.0;
        double total_abs_slippage = 0.0;
        int64_t total_latency = 0;

        for (const auto& [id, f] : fills_)
        {
            if (f.sim_filled && f.exchange_filled)
            {
                both_filled++;
                total_slippage += f.slippage();
                total_abs_slippage += std::abs(f.slippage());
                total_latency += f.latency_ms();
            }
            else if (f.sim_filled && !f.exchange_filled)
            {
                sim_only++;
            }
            else if (!f.sim_filled && f.exchange_filled)
            {
                exchange_only++;
            }
        }

        std::cout << "    Total orders tracked:    " << total << "\n";
        std::cout << "    Both filled:             " << both_filled << "\n";
        std::cout << "    Simulated only:          " << sim_only << "\n";
        std::cout << "    Exchange only:            " << exchange_only << "\n";

        if (both_filled > 0)
        {
            double avg_slippage = total_slippage / both_filled;
            double avg_abs_slippage = total_abs_slippage / both_filled;
            double avg_latency = static_cast<double>(total_latency) / both_filled;

            std::cout << std::fixed << std::setprecision(6);
            std::cout << "    Avg slippage:            " << avg_slippage << "\n";
            std::cout << "    Avg |slippage|:          " << avg_abs_slippage << "\n";
            std::cout << "    Avg latency (ms):        " << avg_latency << "\n";

            // Fill rate comparison
            double sim_fill_rate = static_cast<double>(both_filled + sim_only) / total * 100.0;
            double exch_fill_rate = static_cast<double>(both_filled + exchange_only) / total * 100.0;
            std::cout << std::fixed << std::setprecision(1);
            std::cout << "    Sim fill rate:           " << sim_fill_rate << "%\n";
            std::cout << "    Exchange fill rate:       " << exch_fill_rate << "%\n";
        }

        std::cout << "  ============================================\n\n";
    }

    const std::unordered_map<uint64_t, shadow_fill>& fills() const { return fills_; }

private:
    std::unordered_map<uint64_t, shadow_fill> fills_;

    shadow_fill& get_or_create(uint64_t order_id)
    {
        auto it = fills_.find(order_id);
        if (it != fills_.end())
            return it->second;
        fills_[order_id] = shadow_fill{};
        fills_[order_id].order_id = order_id;
        return fills_[order_id];
    }
};
