// R3 — property suite over the authoritative ledger and the risk views.
//
// The generator is seeded explicitly and never touches std::random_device or
// a system clock, so a failure reproduces from the printed case index alone
// (same convention as tests/test_mm_strategy_property.cpp).

#include <gtest/gtest.h>

#include "risk/risk_accounting.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using truetest::risk::build_risk_view;

namespace {

constexpr std::uint64_t property_seed = 0x5EED30003ULL;
constexpr int property_cases = 300;

auto epoch_ms(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

const std::string* symbol_universe()
{
    static const std::string syms[] = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    return syms;
}
constexpr std::size_t symbol_count = 3;

struct marks
{
    std::unordered_map<std::string, mark_point> m;
    mark_point operator()(const std::string& sym) const
    {
        auto it = m.find(sym);
        return (it != m.end()) ? it->second : mark_point{};
    }
};

// One randomly generated lifecycle step applied to the ledger + portfolio.
struct world
{
    OrderTracker ledger;
    portfolio port{1'000'000.0};
    marks mk;
    std::vector<std::uint64_t> live;
    std::uint64_t next_id = 1;
};

class driver
{
public:
    explicit driver(std::uint64_t seed) : rng_(seed) {}

    double uniform(double lo, double hi)
    {
        return std::uniform_real_distribution<double>(lo, hi)(rng_);
    }
    int int_uniform(int lo, int hi)
    {
        return std::uniform_int_distribution<int>(lo, hi)(rng_);
    }

    void step(world& w)
    {
        const std::string& sym = symbol_universe()[int_uniform(0, symbol_count - 1)];
        w.mk.m[sym] = mark_point{uniform(1.0, 500.0), epoch_ms(int_uniform(0, 1000))};

        const int action = int_uniform(0, 6);
        if (action <= 2 || w.live.empty())
        {
            // Submit a new order.
            const auto id = w.next_id++;
            const auto side = (int_uniform(0, 1) == 0) ? order_side::buy : order_side::sell;
            order_event o(epoch_ms(0), sym, order_type::limit, side,
                          uniform(0.01, 20.0), uniform(1.0, 500.0));
            o.set_order_id(id);
            w.ledger.register_order(o);
            w.ledger.set_status(id, order_status::pending);
            if (int_uniform(0, 1) == 0)
                w.ledger.set_status(id, order_status::open);
            w.live.push_back(id);
            return;
        }

        const std::size_t idx =
            static_cast<std::size_t>(int_uniform(0, static_cast<int>(w.live.size()) - 1));
        const auto id = w.live[idx];
        const auto* entry = w.ledger.find(id);
        if (!entry)
            return;
        const std::string& order_sym = w.ledger.symbol_of(*entry);
        const auto side = entry->side;

        switch (action)
        {
        case 3: case 4: {   // fill (partial or full)
            const double remaining = entry->remaining_qty();
            if (remaining <= 0.0) { retire(w, idx); return; }
            const double qty = (action == 3)
                ? std::min(remaining, uniform(0.0, remaining))
                : remaining;
            if (qty <= 0.0) return;
            fill_event f(epoch_ms(0), order_sym, id, side, qty,
                         uniform(1.0, 500.0), 0.0, remaining - qty,
                         /*fill_id=*/(int_uniform(0, 3) == 0) ? id * 1000 + 7 : 0);
            if (w.ledger.on_fill(f))
                w.port.on_fill(f);
            if (!w.ledger.is_active(id)) retire(w, idx);
            return;
        }
        case 5: {           // duplicate delivery of the previous fill id
            fill_event dup(epoch_ms(0), order_sym, id, side, 1.0,
                           uniform(1.0, 500.0), 0.0, 0.0, id * 1000 + 7);
            if (w.ledger.on_fill(dup))
                w.port.on_fill(dup);
            if (!w.ledger.is_active(id)) retire(w, idx);
            return;
        }
        default: {          // terminate
            const order_status terminal[] = {order_status::cancelled,
                                             order_status::rejected,
                                             order_status::expired};
            w.ledger.set_status(id, terminal[int_uniform(0, 2)]);
            retire(w, idx);
            return;
        }
        }
    }

private:
    static void retire(world& w, std::size_t idx)
    {
        w.live[idx] = w.live.back();
        w.live.pop_back();
    }

    std::mt19937_64 rng_;
};

// Invariants that must hold after every single step.
void check_ledger_invariants(const world& w, int case_index)
{
    std::size_t open_entries = 0;
    double per_symbol_buy[symbol_count] = {0.0, 0.0, 0.0};
    double per_symbol_sell[symbol_count] = {0.0, 0.0, 0.0};
    std::size_t per_symbol_count[symbol_count] = {0, 0, 0};

    w.ledger.for_each_open([&](const order_ledger_entry& e) {
        ++open_entries;
        EXPECT_GE(e.filled_qty, 0.0) << "case " << case_index;
        EXPECT_LE(e.filled_qty, e.original_qty + 1e-9) << "case " << case_index;
        EXPECT_GE(e.remaining_qty(), 0.0) << "case " << case_index;
        EXPECT_NEAR(e.remaining_qty(), e.original_qty - e.filled_qty, 1e-9)
            << "case " << case_index;
        EXPECT_FALSE(order_status_is_terminal(e.status)) << "case " << case_index;

        const std::string& sym = w.ledger.symbol_of(e);
        for (std::size_t i = 0; i < symbol_count; ++i)
        {
            if (symbol_universe()[i] != sym) continue;
            (e.side == order_side::buy ? per_symbol_buy[i] : per_symbol_sell[i])
                += e.remaining_qty();
            ++per_symbol_count[i];
        }
    });

    // P: open order count == number of non-terminal ledger entries, exactly.
    EXPECT_EQ(w.ledger.active_count(), open_entries) << "case " << case_index;
    EXPECT_EQ(w.ledger.get_open_orders().size(), open_entries) << "case " << case_index;

    // P: the incrementally maintained aggregates equal a full recount.
    for (std::size_t i = 0; i < symbol_count; ++i)
    {
        const auto agg = w.ledger.open_exposure(symbol_universe()[i]);
        EXPECT_NEAR(agg.open_buy_qty, per_symbol_buy[i], 1e-9)
            << "case " << case_index << " sym " << symbol_universe()[i];
        EXPECT_NEAR(agg.open_sell_qty, per_symbol_sell[i], 1e-9)
            << "case " << case_index << " sym " << symbol_universe()[i];
        EXPECT_EQ(agg.open_order_count, per_symbol_count[i])
            << "case " << case_index << " sym " << symbol_universe()[i];
        EXPECT_GE(agg.open_buy_qty, 0.0);
        EXPECT_GE(agg.open_sell_qty, 0.0);
    }
}

} // namespace

TEST(RiskProperty, LedgerInvariantsHoldUnderRandomLifecycles)
{
    driver d(property_seed);
    world w;
    for (int i = 0; i < property_cases; ++i)
    {
        d.step(w);
        check_ledger_invariants(w, i);
    }
    EXPECT_GT(w.ledger.orders_seen(), 0u);
}

TEST(RiskProperty, TerminalOrdersCarryNoPendingExposure)
{
    driver d(property_seed + 1);
    world w;
    for (int i = 0; i < property_cases; ++i)
    {
        d.step(w);
        for (std::uint64_t id = 1; id < w.next_id; ++id)
        {
            const auto* e = w.ledger.find(id);
            if (!e || !order_status_is_terminal(e->status))
                continue;
            EXPECT_DOUBLE_EQ(w.ledger.pending_qty(id), 0.0) << "case " << i;
            EXPECT_FALSE(w.ledger.is_active(id)) << "case " << i;
        }
    }
}

TEST(RiskProperty, CancelNeverIncreasesWorstCaseExposure)
{
    driver d(property_seed + 2);
    world w;
    for (int i = 0; i < property_cases; ++i)
    {
        d.step(w);
        if (w.live.empty())
            continue;

        const auto id = w.live.front();
        const auto* entry = w.ledger.find(id);
        if (!entry) continue;
        const std::string sym = w.ledger.symbol_of(*entry);
        if (sym.empty()) continue;

        risk_snapshot before;
        build_risk_view(before, sym, w.port, w.ledger, epoch_ms(1000), 0, w.mk);

        // Cancel for real (the ledger is deliberately non-copyable — it owns
        // an atomic count workers read) and let the world carry on.
        w.ledger.set_status(id, order_status::cancelled);
        w.live[0] = w.live.back();
        w.live.pop_back();
        risk_snapshot after;
        build_risk_view(after, sym, w.port, w.ledger, epoch_ms(1000), 0, w.mk);

        EXPECT_LE(std::abs(after.instrument.worst_case_long_qty),
                  std::abs(before.instrument.worst_case_long_qty) + 1e-9)
            << "case " << i;
        EXPECT_LE(std::abs(after.instrument.worst_case_short_qty),
                  std::abs(before.instrument.worst_case_short_qty) + 1e-9)
            << "case " << i;
        EXPECT_LE(after.portfolio.worst_case_gross_exposure,
                  before.portfolio.worst_case_gross_exposure + 1e-6)
            << "case " << i;
    }
}

// A risk-reducing candidate must never push the worst case further out in the
// direction it is supposed to reduce.
TEST(RiskProperty, RiskReducingOrdersNeverGrowInventoryInTheirDirection)
{
    driver d(property_seed + 3);
    world w;
    for (int i = 0; i < property_cases; ++i)
    {
        d.step(w);
        for (std::size_t s = 0; s < symbol_count; ++s)
        {
            const std::string& sym = symbol_universe()[s];
            risk_snapshot snap;
            build_risk_view(snap, sym, w.port, w.ledger, epoch_ms(1000), 0, w.mk);
            const double pos = snap.instrument.position_qty;
            if (std::abs(pos) <= 1e-9)
                continue;

            const auto side = (pos > 0.0) ? order_side::sell : order_side::buy;
            const double qty = std::abs(pos) * 0.5;
            ASSERT_EQ(classify_inventory_effect(side, qty, pos),
                      inventory_effect::reducing) << "case " << i;

            const double after = pos + ((side == order_side::buy) ? qty : -qty);
            EXPECT_LE(std::abs(after), std::abs(pos) + 1e-9) << "case " << i;
            // And it never crosses zero (that would be a flip, i.e. increasing).
            EXPECT_GE(pos * after, -1e-9) << "case " << i;
        }
    }
}

// Exposure is a function of (quantity, mark) only. Rebuilding the same world
// with a different cost basis must not move a single exposure number.
TEST(RiskProperty, MarkToMarketExposureIsIndependentOfCostBasis)
{
    driver d(property_seed + 4);
    std::mt19937_64 rng(property_seed + 40);

    for (int i = 0; i < 60; ++i)
    {
        OrderTracker ledger;
        marks mk;
        const std::string& sym = symbol_universe()[i % symbol_count];
        const double qty = std::uniform_real_distribution<double>(0.1, 50.0)(rng);
        const double mark = std::uniform_real_distribution<double>(1.0, 500.0)(rng);
        const double basis_a = std::uniform_real_distribution<double>(1.0, 500.0)(rng);
        const double basis_b = std::uniform_real_distribution<double>(1.0, 500.0)(rng);
        mk.m[sym] = mark_point{mark, epoch_ms(0)};

        portfolio a(1'000'000.0);
        a.on_fill(fill_event(epoch_ms(0), sym, 1, order_side::buy, qty, basis_a, 0.0));
        portfolio b(1'000'000.0);
        b.on_fill(fill_event(epoch_ms(0), sym, 1, order_side::buy, qty, basis_b, 0.0));

        risk_snapshot sa, sb;
        build_risk_view(sa, sym, a, ledger, epoch_ms(0), 0, mk);
        build_risk_view(sb, sym, b, ledger, epoch_ms(0), 0, mk);

        EXPECT_DOUBLE_EQ(sa.instrument.position_notional,
                         sb.instrument.position_notional) << "case " << i;
        EXPECT_DOUBLE_EQ(sa.portfolio.gross_exposure,
                         sb.portfolio.gross_exposure) << "case " << i;
        EXPECT_DOUBLE_EQ(sa.portfolio.worst_case_gross_exposure,
                         sb.portfolio.worst_case_gross_exposure) << "case " << i;
        EXPECT_DOUBLE_EQ(sa.instrument.position_notional, qty * mark) << "case " << i;
        if (std::abs(basis_a - basis_b) > 1e-9)
            EXPECT_NE(sa.instrument.unrealized_pnl, sb.instrument.unrealized_pnl)
                << "case " << i;
    }
    (void)d;
}

// Worst-case quantities always bracket the current position: the long branch
// can only be >= position, the short branch only <= position.
TEST(RiskProperty, WorstCaseBracketsTheCurrentPosition)
{
    driver d(property_seed + 5);
    world w;
    for (int i = 0; i < property_cases; ++i)
    {
        d.step(w);
        for (std::size_t s = 0; s < symbol_count; ++s)
        {
            const std::string& sym = symbol_universe()[s];
            risk_snapshot snap;
            build_risk_view(snap, sym, w.port, w.ledger, epoch_ms(1000), 0, w.mk);
            const auto& v = snap.instrument;
            EXPECT_GE(v.worst_case_long_qty, v.position_qty - 1e-9) << "case " << i;
            EXPECT_LE(v.worst_case_short_qty, v.position_qty + 1e-9) << "case " << i;
            EXPECT_GE(v.open_buy_qty, 0.0) << "case " << i;
            EXPECT_GE(v.open_sell_qty, 0.0) << "case " << i;
            EXPECT_GE(v.position_notional, 0.0) << "case " << i;
        }
    }
}
