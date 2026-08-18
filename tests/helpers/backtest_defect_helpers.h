// Shared fixtures for backtest defect regression suites.
#pragma once

#include <gtest/gtest.h>

#include "analytics/analytics.h"
#include "data/data_handler.h"
#include "data/data_wrapper.h"
#include "data/date_parse.h"
#include "data/market_source.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "execution/execution_adapter.h"
#include "execution/hybrid_paper_adapter.h"
#include "execution/latency_model.h"
#include "execution/portfolio.h"
#include "execution/queue_aware_book_adapter.h"
#include "execution/queue_model.h"
#include "helpers/mock_transport.h"
#include "orderbook/fill_model.h"
#include "providers/data_bridge.h"
#include "providers/local/csv_parser.h"
#include "simulation/monte_carlo_controller.h"
#include "simulation/monte_carlo_types.h"
#include "simulation/generators/gbm_generator.h"
#include "strategy/ma_crossover_strategy.h"
#include "strategy/sma_strategy.h"
#include "strategy/strategy_interface.h"
#include "strategy/strategy_registry.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;
using namespace truetest::simulation;

namespace bt_defect {

inline auto t0()
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(0));
}
inline auto t_at(int ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

struct SilenceOutput {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceOutput() : orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceOutput() { std::cout.rdbuf(orig); }
};

class DispatchCountStrategy : public IStrategy {
public:
    int market_calls = 0;
    int tick_calls = 0;
    std::optional<order_event> on_market(const market_event&) override
    {
        ++market_calls;
        return std::nullopt;
    }
    std::optional<order_event> on_tick(const tick_event&) override
    {
        ++tick_calls;
        return std::nullopt;
    }
};

class OneShotMarketBuy : public IStrategy {
    bool done_ = false;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (done_) return std::nullopt;
        done_ = true;
        return order_event(m.get_timestamp(), m.get_symbol(),
                           order_type::market, order_side::buy, 1.0, m.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

inline void load_bars(data_handler& dh, int n, double px0 = 100.0)
{
    for (int i = 0; i < n; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        const double px = px0 + 0.1 * i;
        dh.load_into_queue(std::to_string(ts), "BTCUSDT",
                           px, px + 0.5, px - 0.5, px, 1000);
    }
}

// Instance-local fee probe (no process-wide statics).
struct FeeProbeState {
    double last_entry_fee = -1.0;
    double last_exit_fee = -1.0;
    double last_fixed_fee = -1.0;
    double last_risk_fraction = -1.0;
};

struct FeeProbeStrategy : IStrategy {
    FeeProbeState* state = nullptr;
    explicit FeeProbeStrategy(FeeProbeState* s = nullptr) : state(s) {}
    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }
    std::vector<param_def> get_param_schema() const override
    {
        return {
            {"entry_fee_rate", 0.0, 0.0, 0.05, ""},
            {"exit_fee_rate", 0.0, 0.0, 0.05, ""},
            {"fixed_fee_per_leg", 0.0, 0.0, 1e6, ""},
            {"risk_fraction", 0.02, 0.0, 1.0, ""},
        };
    }
    void set_param(const std::string& key, double value) override
    {
        if (!state) throw std::runtime_error("FeeProbeStrategy has no state");
        if (key == "entry_fee_rate") state->last_entry_fee = value;
        else if (key == "exit_fee_rate") state->last_exit_fee = value;
        else if (key == "fixed_fee_per_leg") state->last_fixed_fee = value;
        else if (key == "risk_fraction") state->last_risk_fraction = value;
        else throw std::runtime_error("Unknown parameter: " + key);
    }
};

class DayLimitPoster : public IStrategy {
    bool posted_ = false;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (posted_) return std::nullopt;
        posted_ = true;
        return order_event(m.get_timestamp(), m.get_symbol(),
                           order_type::limit, order_side::buy, 1.0,
                           m.get_close() * 0.5, time_in_force::day);
    }
    void set_position_open(const std::string&, bool) override {}
};

class OneShotLimitAtClose : public IStrategy {
    bool done_ = false;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (done_) return std::nullopt;
        done_ = true;
        return order_event(m.get_timestamp(), m.get_symbol(),
                           order_type::limit, order_side::buy, 1.0, m.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

class OneShotBuyLimitAt995 : public IStrategy {
    bool done_ = false;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (done_) return std::nullopt;
        done_ = true;
        return order_event(m.get_timestamp(), m.get_symbol(),
                           order_type::limit, order_side::buy, 1.0, 99.5);
    }
    void set_position_open(const std::string&, bool) override {}
};

class OneShotLimitAtCloseFillCount : public IStrategy {
    bool done_ = false;
public:
    int fill_callbacks = 0;
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (done_) return std::nullopt;
        done_ = true;
        return order_event(m.get_timestamp(), m.get_symbol(),
                           order_type::limit, order_side::buy, 1.0, m.get_close());
    }
    void on_fill(const fill_event&, uint64_t) override { ++fill_callbacks; }
    void set_position_open(const std::string&, bool) override {}
};

} // namespace bt_defect

using bt_defect::t0;
using bt_defect::t_at;
using bt_defect::SilenceOutput;
using bt_defect::DispatchCountStrategy;
using bt_defect::OneShotMarketBuy;
using bt_defect::load_bars;
using bt_defect::FeeProbeState;
using bt_defect::FeeProbeStrategy;
using bt_defect::DayLimitPoster;
using bt_defect::OneShotLimitAtClose;
using bt_defect::OneShotBuyLimitAt995;
using bt_defect::OneShotLimitAtCloseFillCount;
