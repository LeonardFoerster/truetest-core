#include <gtest/gtest.h>

#include "analytics/analytics.h"
#include "execution/execution_adapter.h"
#include "execution/fee_model.h"
#include "execution/portfolio.h"
#include "exits/exit_manager.h"
#include "providers/local/csv_parser.h"
#include "reference/ema_rsi_atr_pullback_reference.h"
#include "reference/observability_evidence_writer.h"
#include "reference/observability_semantic_report.h"
#include "risk/risk_manager.h"
#include "strategy/ema_rsi_atr_pullback/ema_rsi_atr_pullback_strategy.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using observability_evidence::boolean;
using observability_evidence::integer;
using observability_evidence::number;
using observability_evidence::row;
using observability_evidence::trace_buffer;
using namespace truetest::exits;

constexpr std::string_view kStrategy = "ema-rsi-atr-pullback";
constexpr double kInitialCash = 10000.0;
constexpr double kQuantityScale = 1e6;
constexpr std::size_t kEmaPeriod = 3;
constexpr std::size_t kRsiPeriod = 2;
constexpr std::size_t kAtrPeriod = 2;
constexpr double kRiskFraction = 0.005;
constexpr double kAtrStopMultiplier = 2.0;
constexpr double kRsiLongThreshold = 40.0;
constexpr double kRsiShortThreshold = 60.0;
constexpr double kFeePerFill = 1.0;
constexpr unsigned kExecutionSeed = 424242;
constexpr double kMarketAggression = 1.1;
constexpr std::size_t kRollingWindow = 252;
constexpr double kRiskFreeRate = 0.0;
constexpr std::size_t kPeriodsPerYear = 525600;
constexpr std::size_t kMaxEquityPoints = 100000;

std::string maybe_number(const std::optional<double>& value)
{
    return value ? number(*value) : "NOT_READY";
}

std::string side_name(order_side side)
{
    return side == order_side::buy ? "buy" : "sell";
}

std::string order_type_name(order_type type)
{
    switch (type) {
    case order_type::market:
        return "market";
    case order_type::limit:
        return "limit";
    case order_type::stop:
        return "stop";
    case order_type::stop_limit:
        return "stop_limit";
    }
    return "UNVERIFIED";
}

std::string state_name(ema_rsi_atr_pullback_strategy::trade_state state)
{
    switch (state) {
    case ema_rsi_atr_pullback_strategy::trade_state::flat:
        return "flat";
    case ema_rsi_atr_pullback_strategy::trade_state::entry_pending_long:
        return "entry_pending_long";
    case ema_rsi_atr_pullback_strategy::trade_state::entry_pending_short:
        return "entry_pending_short";
    case ema_rsi_atr_pullback_strategy::trade_state::long_open:
        return "long_open";
    case ema_rsi_atr_pullback_strategy::trade_state::short_open:
        return "short_open";
    case ema_rsi_atr_pullback_strategy::trade_state::exit_pending_long:
        return "exit_pending_long";
    case ema_rsi_atr_pullback_strategy::trade_state::exit_pending_short:
        return "exit_pending_short";
    }
    return "UNVERIFIED";
}

std::string risk_action_name(risk_action action)
{
    switch (action) {
    case risk_action::pass:
        return "pass";
    case risk_action::reject:
        return "reject";
    case risk_action::halt:
        return "halt";
    case risk_action::unwind:
        return "unwind";
    }
    return "UNVERIFIED";
}

std::int64_t timestamp_ms(std::chrono::system_clock::time_point timestamp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch())
        .count();
}

std::unordered_map<std::string, double>
indicator_snapshot(const ema_rsi_atr_pullback_strategy& strategy, const std::string& symbol)
{
    std::unordered_map<std::string, double> result;
    for (const auto& [name, value] : strategy.get_indicator_values(symbol))
        result.emplace(name, value);
    return result;
}

std::string indicator_value(const std::unordered_map<std::string, double>& values,
                            const std::string& name)
{
    const auto found = values.find(name);
    return found == values.end() ? "NOT_READY" : number(found->second);
}

void require_near(double actual, double expected, std::string_view field)
{
    if (!std::isfinite(actual) || !std::isfinite(expected))
        throw std::runtime_error(std::string(field) + " is non-finite");
    const double tolerance = 64.0 * std::numeric_limits<double>::epsilon() *
                             std::max({1.0, std::abs(actual), std::abs(expected)});
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(std::string(field) + " disagrees with independent evidence");
}

void require_indicator(const std::unordered_map<std::string, double>& production,
                       const std::string& name, const std::optional<double>& expected)
{
    const auto found = production.find(name);
    if (found == production.end() || !expected) {
        if (found != production.end() || expected)
            throw std::runtime_error(name + " readiness disagrees with independent oracle");
        return;
    }
    require_near(found->second, *expected, name);
}

std::vector<bar_record> read_fixture(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open observability fixture: " + path.string());

    CsvBarParser parser;
    std::string line;
    if (!std::getline(input, line) || !parser.parse_header(line))
        throw std::runtime_error("cannot parse observability fixture header");

    std::vector<bar_record> result;
    while (std::getline(input, line)) {
        auto parsed = parser.parse_record(line);
        if (!parsed) throw std::runtime_error("observability fixture row was rejected");
        result.push_back(std::move(*parsed));
    }
    if (!parser.rejections().empty())
        throw std::runtime_error("observability fixture contains rejected rows");
    return result;
}

risk_snapshot make_risk_snapshot(const portfolio& account, double mark)
{
    risk_snapshot snapshot;
    snapshot.equity = account.get_equity(mark);
    snapshot.ledger_authoritative = true;
    snapshot.instrument.mark_price = mark;
    snapshot.instrument.mark_state = mark_quality::valid;
    snapshot.instrument.mark_age_ms = 0;
    snapshot.instrument.exposure_tracked = true;
    const auto found = account.get_positions().find("OBS");
    if (found != account.get_positions().end()) {
        snapshot.instrument.position_qty = found->second.qty;
        snapshot.instrument.position_notional = std::abs(found->second.qty) * mark;
        snapshot.portfolio.gross_exposure = snapshot.instrument.position_notional;
        snapshot.portfolio.net_exposure = found->second.qty * mark;
    }
    snapshot.portfolio.equity = snapshot.equity;
    snapshot.portfolio.worst_case_gross_exposure = snapshot.portfolio.gross_exposure;
    return snapshot;
}

struct position_snapshot
{
    double cash = 0.0;
    double quantity = 0.0;
    double cost_basis = 0.0;
};

position_snapshot observe_position(const portfolio& account)
{
    position_snapshot result;
    result.cash = account.get_cash();
    const auto found = account.get_positions().find("OBS");
    if (found != account.get_positions().end()) {
        result.quantity = found->second.qty;
        result.cost_basis = found->second.cost_basis;
    }
    return result;
}

fill_event execute_against_controlled_depth(LocalBookAdapter& adapter,
                                            const std::shared_ptr<orderbook>& book,
                                            const order_event& candidate, double mark,
                                            double counter_price, trace_buffer& trace)
{
    const side counter_side = candidate.get_side() == order_side::buy ? side::sell : side::buy;
    const auto book_quantity =
        static_cast<quantity>(std::ceil(candidate.get_quantity() * kQuantityScale) + 100.0);
    const std::uint64_t counter_id = 9000 + candidate.get_order_id();
    book->add_order(std::make_shared<order>(ob_order_type::good_till_cancel, counter_id,
                                            counter_side, Price::from_double(counter_price),
                                            book_quantity));

    const auto depth = book->get_order_infos();
    const auto& levels = counter_side == side::buy ? depth.get_bids() : depth.get_asks();
    if (levels.empty()) throw std::runtime_error("controlled depth was not stored in orderbook");
    const double stored_price = levels.front().price_.to_double();
    const double stored_quantity =
        static_cast<double>(levels.front().quantity_) / kQuantityScale;
    require_near(stored_price, Price::from_double(counter_price).to_double(),
                 "controlled depth price readback");
    require_near(stored_quantity, static_cast<double>(book_quantity) / kQuantityScale,
                 "controlled depth quantity readback");

    adapter.set_mid_price(mark);
    adapter.submit_order(candidate);
    std::vector<fill_event> fills;
    if (!adapter.poll_fills(fills) || fills.size() != 1)
        throw std::runtime_error("controlled book did not produce exactly one fill");
    auto fill = std::move(fills.front());
    const auto& provenance = fill.get_provenance();
    trace.append(trace.execution, [&] {
        return row{integer(candidate.get_order_id()),
                   std::string(fill_execution_model_name(provenance.model)),
                   counter_side == side::buy ? "bid" : "ask",
                   number(stored_price),
                   number(stored_quantity),
                   number(provenance.intended_price),
                   number(provenance.reference_price),
                   number(fill.get_fill_price()),
                   number(fill.get_fill_price() - provenance.intended_price),
                   number(provenance.modeled_spread_bps),
                   number(provenance.modeled_impact_bps),
                   "CONTROLLED_BOOK_PRE_SUBMIT_READBACK",
                   "HARNESS_ASSIGNED",
                   "PRODUCTION_STATE_SNAPSHOT",
                   "PRODUCTION_RETURN_VALUE",
                   "HARNESS_DERIVED"};
    });
    return fill;
}

void trace_order(trace_buffer& trace, const order_event& candidate)
{
    trace.append(trace.orders, [&] {
        return row{integer(candidate.get_order_id()),
                   integer(candidate.get_signal_id()),
                   integer(candidate.get_opener_order_id()),
                   side_name(candidate.get_side()),
                   order_type_name(candidate.get_order_type()),
                   number(candidate.get_price()),
                   number(candidate.get_quantity()),
                   integer(timestamp_ms(candidate.get_decision_ts())),
                   integer(timestamp_ms(candidate.get_submit_ts())),
                   integer(timestamp_ms(candidate.get_earliest_eligible_ts())),
                   "HARNESS_ASSIGNED",
                   "PRODUCTION_RETURN_VALUE",
                   "HARNESS_ASSIGNED"};
    });
}

void trace_risk(trace_buffer& trace, RiskManager& risk, const portfolio& account,
                const order_event& candidate, double mark)
{
    const auto snapshot = make_risk_snapshot(account, mark);
    risk_rule rule = risk_rule::none;
    const auto action = risk.check_order(candidate, account, snapshot, 0, &rule);
    trace.append(trace.risk, [&] {
        return row{integer(candidate.get_order_id()),
                   "aggregate_pretrade",
                   number(snapshot.equity),
                   number(mark),
                   number(snapshot.instrument.position_qty),
                   number(snapshot.portfolio.gross_exposure),
                   risk_action_name(action),
                   to_string(rule),
                   "HARNESS_ASSIGNED",
                   "HARNESS_ASSIGNED",
                   "PRODUCTION_RETURN_VALUE",
                   "UNVERIFIED_PER_RULE_PASS_SEQUENCE"};
    });
    if (action != risk_action::pass || rule != risk_rule::none)
        throw std::runtime_error("controlled evidence order was not accepted by risk");
}

void trace_fill_and_position(trace_buffer& trace, const fill_event& fill,
                             const position_snapshot& before, const position_snapshot& after)
{
    const auto& provenance = fill.get_provenance();
    trace.append(trace.fills, [&] {
        return row{integer(fill.get_fill_id()),
                   integer(fill.get_order_id()),
                   integer(fill.get_opener_order_id()),
                   side_name(fill.get_side()),
                   number(fill.get_filled_quantity()),
                   number(fill.get_commission()),
                   integer(timestamp_ms(fill.get_timestamp())),
                   number(fill.get_remaining_qty()),
                   number(provenance.intended_price),
                   number(provenance.reference_price),
                   number(fill.get_fill_price()),
                   number(provenance.modeled_spread_bps),
                   number(provenance.modeled_impact_bps),
                   std::string(fill_execution_model_name(provenance.model)),
                   std::string(fill_execution_reason_name(provenance.reason)),
                   "PRODUCTION_RETURN_VALUE",
                   "HARNESS_ASSIGNED"};
    });
    trace.append(trace.positions, [&] {
        return row{integer(fill.get_fill_id()), number(before.cash),
                   number(after.cash),          number(before.quantity),
                   number(after.quantity),      number(before.cost_basis),
                   number(after.cost_basis),    "HARNESS_JOIN",
                   "PRODUCTION_STATE_SNAPSHOT"};
    });
}

struct run_result
{
    AnalyticsReport report;
    double portfolio_cash = 0.0;
    std::uint64_t entry_order_id = 0;
    std::uint64_t exit_order_id = 0;
    std::size_t physical_entry_row = 0;
};

run_result run_one_trade(const std::vector<bar_record>& bars, trace_buffer& trace)
{
    if (bars.size() != 5)
        throw std::runtime_error("observability fixture must contain exactly five bars");

    ema_rsi_atr_pullback_strategy strategy(kEmaPeriod, kRsiPeriod, kAtrPeriod, kRiskFraction,
                                           kAtrStopMultiplier, kInitialCash, kRsiLongThreshold,
                                           kRsiShortThreshold);
    ema_rsi_atr_reference::evaluator reference(kEmaPeriod, kRsiPeriod, kAtrPeriod,
                                                kRsiLongThreshold, kRsiShortThreshold);
    ExitManager exits;
    RiskManager risk;
    portfolio account(kInitialCash);
    Analytics analytics(kInitialCash, kRollingWindow, kRiskFreeRate, kPeriodsPerYear,
                        kMaxEquityPoints);
    auto book = std::make_shared<orderbook>();
    LocalBookAdapter adapter(book, std::make_shared<FixedFeeModel>(kFeePerFill), nullptr,
                             kExecutionSeed, kMarketAggression, kQuantityScale);

    std::optional<order_event> entry;
    std::uint64_t entry_order_id = 0;
    std::size_t entry_physical_row = 0;

    for (std::size_t i = 0; i < 4; ++i) {
        const auto& input = bars[i];
        if (!input.source) throw std::runtime_error("fixture row has no physical provenance");
        const auto timestamp =
            std::chrono::system_clock::time_point{std::chrono::milliseconds(input.open_time_ms)};
        market_event market(timestamp, input.symbol, input.open, input.high, input.low, input.close,
                            input.volume, input.quantity_scale);
        analytics.on_event(std::make_shared<market_event>(market));

        trace.append(trace.bars, [&] {
            return row{integer(input.source->physical_row),
                       integer(input.source->accepted_index),
                       integer(i),
                       integer(input.open_time_ms),
                       input.symbol,
                       number(input.open),
                       number(input.high),
                       number(input.low),
                       number(input.close),
                       "PRODUCTION_RETURN_VALUE",
                       "HARNESS_ASSIGNED",
                       "PRODUCTION_RETURN_VALUE"};
        });

        const auto before = indicator_snapshot(strategy, input.symbol);
        const auto state_before = strategy.get_trade_state(input.symbol);
        const auto oracle = reference.on_bar({input.open, input.high, input.low, input.close});
        auto candidate = strategy.on_market(market);
        const auto state_after = strategy.get_trade_state(input.symbol);
        const auto after = indicator_snapshot(strategy, input.symbol);

        require_indicator(after, "ema_3", oracle.ema);
        require_indicator(after, "rsi_2", oracle.rsi_current);
        require_indicator(after, "atr_2", oracle.atr);

        for (const auto& name :
             {std::string("ema_3"), std::string("rsi_2"), std::string("atr_2")}) {
            trace.append(trace.indicators, [&] {
                return row{integer(i), name, indicator_value(before, name),
                           indicator_value(after, name), "HARNESS_ASSIGNED",
                           "PRODUCTION_STATE_SNAPSHOT"};
            });
        }

        const std::string decision = candidate ? side_name(candidate->get_side()) : "none";
        trace.append(trace.signals, [&] {
            return row{candidate ? "5001" : "0",
                       integer(i),
                       integer(input.source->physical_row),
                       integer(input.open_time_ms),
                       state_name(state_before),
                       state_name(state_after),
                       maybe_number(oracle.ema),
                       maybe_number(oracle.rsi_previous),
                       maybe_number(oracle.rsi_current),
                       maybe_number(oracle.atr),
                       boolean(oracle.close_above_ema),
                       boolean(oracle.close_below_ema),
                       boolean(oracle.previous_at_or_below_long),
                       boolean(oracle.current_above_long),
                       boolean(oracle.previous_at_or_above_short),
                       boolean(oracle.current_below_short),
                       decision,
                       "HARNESS_ASSIGNED",
                       "HARNESS_JOIN",
                       "PRODUCTION_STATE_SNAPSHOT",
                       "INDEPENDENT_ORACLE",
                       "PRODUCTION_RETURN_VALUE"};
        });

        const bool oracle_order = oracle.expected != ema_rsi_atr_reference::signal::none;
        if (candidate.has_value() != oracle_order)
            throw std::runtime_error("production strategy disagrees with independent oracle");
        if (candidate && ((oracle.expected == ema_rsi_atr_reference::signal::buy &&
                           candidate->get_side() != order_side::buy) ||
                          (oracle.expected == ema_rsi_atr_reference::signal::sell &&
                           candidate->get_side() != order_side::sell))) {
            throw std::runtime_error("production order side disagrees with independent oracle");
        }
        if (!candidate) continue;
        if (entry) throw std::runtime_error("fixture emitted more than one entry");

        candidate->set_order_id(1001);
        candidate->set_signal_id(5001);
        candidate->set_strategy_name(std::string(kStrategy));
        candidate->set_decision_ts(timestamp + std::chrono::minutes(1));
        candidate->set_submit_ts(candidate->get_decision_ts());
        candidate->set_earliest_eligible_ts(candidate->get_decision_ts());
        entry_order_id = candidate->get_order_id();
        entry_physical_row = input.source->physical_row;

        auto intents = strategy.take_pending_exit_intents();
        if (intents.size() != 1)
            throw std::runtime_error("entry did not produce exactly one exit intent");
        intents.front().opener_order_id = entry_order_id;
        trace.append(trace.exits, [&] {
            return row{integer(entry_order_id),
                       "registered",
                       maybe_number(intents.front().stop_loss),
                       "0",
                       "none",
                       "0",
                       "0",
                       "HARNESS_ASSIGNED",
                       "HARNESS_JOIN",
                       "PRODUCTION_RETURN_VALUE",
                       "HARNESS_ASSIGNED",
                       "HARNESS_ASSIGNED"};
        });
        exits.register_pending(std::move(intents.front()));
        entry = std::move(*candidate);
    }

    if (!entry || entry_order_id == 0) throw std::runtime_error("fixture produced no entry");

    trace_order(trace, *entry);
    trace_risk(trace, risk, account, *entry, entry->get_price());
    analytics.on_event(std::make_shared<order_event>(*entry));
    auto entry_fill =
        execute_against_controlled_depth(adapter, book, *entry, entry->get_price(), 110.5, trace);
    entry_fill.set_strategy_name(std::string(kStrategy));
    entry_fill.set_opener_order_id(entry_order_id);
    const auto entry_before = observe_position(account);
    account.on_fill(entry_fill, entry_order_id, std::string(kStrategy));
    analytics.on_event(std::make_shared<fill_event>(entry_fill));
    strategy.on_fill(entry_fill, entry_order_id);
    exits.on_fill(entry_fill, entry_order_id);
    const auto entry_after = observe_position(account);
    trace_fill_and_position(trace, entry_fill, entry_before, entry_after);

    const auto armed = exits.snapshot_armed();
    if (armed.size() != 1 || !armed.front().stop_loss)
        throw std::runtime_error("exit intent did not arm from opener fill");
    trace.append(trace.exits, [&] {
        return row{integer(entry_order_id),
                   "armed",
                   number(*armed.front().stop_loss),
                   integer(exits.armed_count()),
                   "none",
                   "0",
                   "0",
                   "HARNESS_ASSIGNED",
                   "HARNESS_JOIN",
                   "PRODUCTION_STATE_SNAPSHOT",
                   "PRODUCTION_STATE_SNAPSHOT",
                   "HARNESS_ASSIGNED"};
    });

    const auto& trigger = bars.back();
    const auto trigger_ts =
        std::chrono::system_clock::time_point{std::chrono::milliseconds(trigger.open_time_ms)};
    market_event trigger_market(trigger_ts, trigger.symbol, trigger.open, trigger.high, trigger.low,
                                trigger.close, trigger.volume, trigger.quantity_scale);
    analytics.on_event(std::make_shared<market_event>(trigger_market));
    trace.append(trace.bars, [&] {
        return row{integer(trigger.source->physical_row),
                   integer(trigger.source->accepted_index),
                   "4",
                   integer(trigger.open_time_ms),
                   trigger.symbol,
                   number(trigger.open),
                   number(trigger.high),
                   number(trigger.low),
                   number(trigger.close),
                   "PRODUCTION_RETURN_VALUE",
                   "HARNESS_ASSIGNED",
                   "PRODUCTION_RETURN_VALUE"};
    });

    // Protective exits take precedence on this bar, so the production strategy is deliberately
    // not evaluated. Preserve that distinction instead of implying a production signal decision.
    const auto trigger_indicators = indicator_snapshot(strategy, trigger.symbol);
    const auto trigger_state = strategy.get_trade_state(trigger.symbol);
    const auto trigger_oracle =
        reference.on_bar({trigger.open, trigger.high, trigger.low, trigger.close});
    for (const auto& name : {std::string("ema_3"), std::string("rsi_2"), std::string("atr_2")}) {
        trace.append(trace.indicators, [&] {
            return row{"4", name, indicator_value(trigger_indicators, name),
                       indicator_value(trigger_indicators, name), "HARNESS_ASSIGNED",
                       "PRODUCTION_STATE_SNAPSHOT"};
        });
    }
    trace.append(trace.signals, [&] {
        return row{"0",
                   "4",
                   integer(trigger.source->physical_row),
                   integer(trigger.open_time_ms),
                   state_name(trigger_state),
                   state_name(trigger_state),
                   maybe_number(trigger_oracle.ema),
                   maybe_number(trigger_oracle.rsi_previous),
                   maybe_number(trigger_oracle.rsi_current),
                   maybe_number(trigger_oracle.atr),
                   boolean(trigger_oracle.close_above_ema),
                   boolean(trigger_oracle.close_below_ema),
                   boolean(trigger_oracle.previous_at_or_below_long),
                   boolean(trigger_oracle.current_above_long),
                   boolean(trigger_oracle.previous_at_or_above_short),
                   boolean(trigger_oracle.current_below_short),
                   "protective_exit_precedence",
                   "HARNESS_ASSIGNED",
                   "HARNESS_JOIN",
                   "PRODUCTION_STATE_SNAPSHOT",
                   "INDEPENDENT_ORACLE",
                   "HARNESS_JOIN"};
    });

    exits.begin_evaluation_window();
    auto close_orders = exits.on_bar(trigger.symbol, trigger.open, trigger.low, trigger.high,
                                     trigger.close, trigger_ts);
    if (close_orders.size() != 1)
        throw std::runtime_error("trigger bar did not produce exactly one protective close");
    auto close = std::move(close_orders.front());
    close.set_order_id(1002);
    close.set_strategy_name(std::string(kStrategy));
    close.set_decision_ts(trigger_ts + std::chrono::minutes(1));
    close.set_submit_ts(close.get_decision_ts());
    close.set_earliest_eligible_ts(close.get_decision_ts());
    if (close.get_opener_order_id() != entry_order_id ||
        close.get_exit_reason() != order_exit_reason::stop_loss) {
        throw std::runtime_error("protective close lost opener/reason attribution");
    }
    trace.append(trace.exits, [&] {
        return row{
            integer(entry_order_id),       "triggered",        number(*armed.front().stop_loss),
            integer(exits.armed_count()),  "stop_loss",        number(close.get_price()),
            integer(close.get_order_id()), "HARNESS_ASSIGNED", "HARNESS_JOIN",
            "PRODUCTION_STATE_SNAPSHOT",   "PRODUCTION_STATE_SNAPSHOT",
            "PRODUCTION_RETURN_VALUE"};
    });

    trace_order(trace, close);
    trace_risk(trace, risk, account, close, trigger.close);
    analytics.on_event(std::make_shared<order_event>(close));
    auto close_fill = execute_against_controlled_depth(adapter, book, close, trigger.close,
                                                       close.get_price(), trace);
    close_fill.set_strategy_name(std::string(kStrategy));
    close_fill.set_opener_order_id(entry_order_id);
    const auto close_before = observe_position(account);
    account.on_fill(close_fill, entry_order_id, std::string(kStrategy));
    analytics.on_event(std::make_shared<fill_event>(close_fill));
    strategy.on_fill(close_fill, entry_order_id);
    exits.on_fill(close_fill, entry_order_id);
    const auto close_after = observe_position(account);
    trace_fill_and_position(trace, close_fill, close_before, close_after);

    Analytics::exit_lifecycle_counts lifecycle;
    lifecycle.registered = exits.counters().pending_registered;
    lifecycle.armed = exits.counters().armed;
    analytics.set_exit_lifecycle_counts(lifecycle);
    auto report = analytics.generate_report();

    if (report.trades.size() != 2)
        throw std::runtime_error("report did not preserve both fill records");
    const auto& entry_record = report.trades[0];
    const auto& close_record = report.trades[1];
    if (entry_record.fill_id != entry_fill.get_fill_id() ||
        entry_record.order_id != entry_fill.get_order_id() ||
        close_record.fill_id != close_fill.get_fill_id() ||
        close_record.order_id != close_fill.get_order_id() ||
        entry_record.side != entry_fill.get_side() || close_record.side != close_fill.get_side()) {
        throw std::runtime_error("report fill/order identity disagrees with emitted fills");
    }
    require_near(entry_record.quantity, entry_fill.get_filled_quantity(), "entry report quantity");
    require_near(close_record.quantity, close_fill.get_filled_quantity(), "close report quantity");
    require_near(close_fill.get_filled_quantity(), entry_fill.get_filled_quantity(),
                 "round-trip fill quantity");
    require_near(entry_record.fill_price, entry_fill.get_fill_price(), "entry report fill price");
    require_near(close_record.fill_price, close_fill.get_fill_price(), "close report fill price");
    require_near(entry_record.commission, entry_fill.get_commission(), "entry report commission");
    require_near(close_record.commission, close_fill.get_commission(), "close report commission");

    const double expected_gross = (close_fill.get_fill_price() - entry_fill.get_fill_price()) *
                                  entry_fill.get_filled_quantity();
    const double expected_commission = entry_fill.get_commission() + close_fill.get_commission();
    const double expected_net = expected_gross - expected_commission;
    require_near(entry_record.pnl, 0.0, "entry report pnl");
    require_near(close_record.pnl, expected_net, "close report pnl");
    require_near(report.gross_realized_pnl, expected_gross, "report gross realized pnl");
    require_near(report.total_commission, expected_commission, "report total commission");
    require_near(report.realized_pnl, expected_net, "report realized pnl");
    require_near(report.final_equity, kInitialCash + expected_net, "report final equity");
    require_near(account.get_cash(), kInitialCash + expected_net, "portfolio final cash");

    for (std::size_t i = 0; i < report.trades.size(); ++i) {
        const auto& trade = report.trades[i];
        trace.append(trace.reconciliation, [&] {
            return row{integer(i),
                       integer(trade.fill_id),
                       integer(trade.order_id),
                       integer(entry_order_id),
                       number(trade.pnl),
                       number(trade.commission),
                       "VERIFIED_FILL_FIELDS_AND_INDEPENDENT_ACCOUNTING",
                       "PRODUCTION_RETURN_VALUE",
                       "HARNESS_JOIN"};
        });
    }

    trace.append(trace.completeness, [&] {
        return row{"physical_row_to_strategy_input", "UNVERIFIED",
                   "production data bridge drops parser source metadata", "HARNESS_JOIN"};
    });
    trace.append(trace.completeness, [&] {
        return row{"strategy_conditions", "VERIFIED_INDEPENDENT_ORACLE",
                   "oracle matched order existence, side, and production indicator snapshots",
                   "INDEPENDENT_ORACLE"};
    });
    trace.append(trace.completeness, [&] {
        return row{"production_strategy_predicate_emission", "UNVERIFIED",
                   "production strategy exposes decision and indicator state but not each predicate",
                   "HARNESS_JOIN"};
    });
    trace.append(trace.completeness, [&] {
        return row{"risk_aggregate_decisions", "VERIFIED", "actual RiskManager return",
                   "PRODUCTION_RETURN_VALUE"};
    });
    trace.append(trace.completeness, [&] {
        return row{"risk_snapshot_inputs", "VERIFIED_HARNESS_CONSTRUCTION",
                   "RiskManager input snapshot is constructed by the component harness",
                   "HARNESS_ASSIGNED"};
    });
    trace.append(trace.completeness, [&] {
        return row{"risk_per_rule_pass_sequence", "UNVERIFIED",
                   "RiskManager exposes only aggregate action and first failing rule",
                   "HARNESS_JOIN"};
    });
    trace.append(trace.completeness, [&] {
        return row{"execution_depth", "VERIFIED_CONTROLLED_BOOK",
                   "production orderbook readback of harness-injected depth joined by order id",
                   "HARNESS_JOIN"};
    });
    trace.append(trace.completeness, [&] {
        return row{"engine_orchestration", "UNVERIFIED",
                   "component harness does not instrument frozen engine routing", "HARNESS_JOIN"};
    });
    trace.append(trace.completeness, [&] {
        return row{"fill_to_report", "VERIFIED",
                   "fill fields matched report rows and pnl/fees were independently recomputed",
                   "HARNESS_JOIN"};
    });
    trace.append(trace.completeness, [&] {
        return row{
            "report_final_equity_to_portfolio_cash", "VERIFIED",
            "report and portfolio independently matched initial cash plus recomputed net pnl",
            "HARNESS_JOIN"};
    });
    trace.append(trace.completeness, [&] {
        return row{"trace_latency_allocation_effects", "UNVERIFIED",
                   "test-only trace invariance does not establish latency or allocation neutrality",
                   "HARNESS_JOIN"};
    });

    return {std::move(report), account.get_cash(), entry_order_id, close.get_order_id(),
            entry_physical_row};
}

std::string metrics_json(const run_result& result)
{
    const auto& report = result.report;
    return "{\n"
           "  \"schema\": \"truetest-observability-evidence-v1\",\n"
           "  \"initial_equity\": " +
           number(report.initial_equity) +
           ",\n"
           "  \"final_equity\": " +
           number(report.final_equity) +
           ",\n"
           "  \"portfolio_cash\": " +
           number(result.portfolio_cash) +
           ",\n"
           "  \"gross_realized_pnl\": " +
           number(report.gross_realized_pnl) +
           ",\n"
           "  \"realized_pnl\": " +
           number(report.realized_pnl) +
           ",\n"
           "  \"total_commission\": " +
           number(report.total_commission) +
           ",\n"
           "  \"reconciliation_residual\": " +
           number(report.reconciliation_residual) +
           ",\n"
           "  \"total_orders\": " +
           integer(report.total_orders) +
           ",\n"
           "  \"total_fills\": " +
           integer(report.total_fills) +
           ",\n"
           "  \"total_trades\": " +
           integer(report.total_trades) +
           ",\n"
           "  \"entry_order_id\": " +
           integer(result.entry_order_id) +
           ",\n"
           "  \"exit_order_id\": " +
           integer(result.exit_order_id) +
           ",\n"
           "  \"entry_physical_row\": " +
           integer(result.physical_entry_row) +
           "\n"
           "}\n";
}

std::string effective_config_json()
{
    const risk_limits limits{};
    return "{\n"
           "  \"schema\": \"truetest-observability-harness-config-v1\",\n"
           "  \"scope\": \"TEST_COMPONENT_HARNESS\",\n"
           "  \"strategy\": \"ema-rsi-atr-pullback\",\n"
           "  \"ema_period\": " +
           integer(kEmaPeriod) +
           ",\n"
           "  \"rsi_period\": " +
           integer(kRsiPeriod) +
           ",\n"
           "  \"atr_period\": " +
           integer(kAtrPeriod) +
           ",\n"
           "  \"risk_fraction\": " +
           number(kRiskFraction) +
           ",\n"
           "  \"atr_stop_multiplier\": " +
           number(kAtrStopMultiplier) +
           ",\n"
           "  \"initial_cash\": " +
           number(kInitialCash) +
           ",\n"
           "  \"rsi_long_threshold\": " +
           number(kRsiLongThreshold) +
           ",\n"
           "  \"rsi_short_threshold\": " +
           number(kRsiShortThreshold) +
           ",\n"
           "  \"execution_adapter\": \"LocalBookAdapter\",\n"
           "  \"fee_model\": \"fixed\",\n"
           "  \"fee_per_fill\": " +
           number(kFeePerFill) +
           ",\n"
           "  \"fill_model\": \"none\",\n"
           "  \"latency_model\": \"none\",\n"
           "  \"impact_model\": \"none\",\n"
           "  \"walked_book_impact\": false,\n"
           "  \"execution_seed\": " +
           integer(kExecutionSeed) +
           ",\n"
           "  \"market_aggression\": " +
           number(kMarketAggression) +
           ",\n"
           "  \"quantity_scale\": " +
           number(kQuantityScale) +
           ",\n"
           "  \"analytics_rolling_window\": " +
           integer(kRollingWindow) +
           ",\n"
           "  \"analytics_risk_free_rate\": " +
           number(kRiskFreeRate) +
           ",\n"
           "  \"analytics_periods_per_year\": " +
           integer(kPeriodsPerYear) +
           ",\n"
           "  \"analytics_max_equity_points\": " +
           integer(kMaxEquityPoints) +
           ",\n"
           "  \"risk_max_position_value\": " +
           number(limits.max_position_value) +
           ",\n"
           "  \"risk_max_drawdown\": " +
           number(limits.max_drawdown) +
           ",\n"
           "  \"risk_max_loss_per_trade\": " +
           number(limits.max_loss_per_trade) +
           ",\n"
           "  \"risk_max_open_orders\": " +
           integer(limits.max_open_orders) +
           ",\n"
           "  \"risk_max_portfolio_exposure\": " +
           number(limits.max_portfolio_exposure) +
           ",\n"
           "  \"risk_max_daily_loss\": " +
           number(limits.max_daily_loss) +
           ",\n"
           "  \"risk_daily_reset_hour\": " +
           integer(limits.daily_reset_hour) +
           ",\n"
           "  \"risk_max_trades_per_hour\": " +
           integer(limits.max_trades_per_hour) +
           ",\n"
           "  \"risk_max_orders_per_minute\": " +
           integer(limits.max_orders_per_minute) +
           ",\n"
           "  \"risk_max_position_pct_of_equity\": " +
           number(limits.max_position_pct_of_equity) +
           ",\n"
           "  \"risk_max_spread_bps\": " +
           number(limits.max_spread_bps) +
           ",\n"
           "  \"risk_max_funding_8h_rate\": " +
           number(limits.max_funding_8h_rate) +
           ",\n"
           "  \"risk_max_gross_leverage\": " +
           number(limits.max_gross_leverage) +
           ",\n"
           "  \"risk_max_symbol_inventory_qty\": " +
           number(limits.max_symbol_inventory_qty) +
           ",\n"
           "  \"risk_max_mark_age_ms\": " +
           integer(limits.max_mark_age_ms) +
           ",\n"
           "  \"risk_require_fresh_mark\": " +
           boolean(limits.require_fresh_mark) +
           ",\n"
           "  \"threading\": \"single_process_component_sequence\"\n"
           "}\n";
}

}  // namespace

TEST(ObservabilityEvidence, DisabledCollectorDoesNotConstructRows)
{
    trace_buffer trace(false);
    bool factory_called = false;
    trace.append(trace.orders, [&] {
        factory_called = true;
        return row{};
    });

    EXPECT_FALSE(factory_called);
    EXPECT_EQ(trace.orders.size(), 0u);
}

TEST(ObservabilityEvidence, OneTradeLinksPhysicalInputToReconciledReport)
{
    const char* configured_input = std::getenv("TT_OBSERVABILITY_INPUT");
    const std::filesystem::path input =
        configured_input && *configured_input
            ? std::filesystem::path(configured_input)
            : std::filesystem::path(TEST_FIXTURES_DIR) / "observability_one_trade.csv";
    const bool trace_enabled = [] {
        const char* value = std::getenv("TT_OBSERVABILITY_TRACE");
        return value && std::string_view(value) == "1";
    }();
    trace_buffer trace(trace_enabled);
    auto result = run_one_trade(read_fixture(input), trace);

    ASSERT_EQ(result.report.total_orders, 2u);
    ASSERT_EQ(result.report.total_fills, 2u);
    ASSERT_EQ(result.report.total_trades, 1u);
    ASSERT_EQ(result.report.trades.size(), 2u);
    EXPECT_EQ(result.report.trades.front().order_id, result.entry_order_id);
    EXPECT_EQ(result.report.trades.back().order_id, result.exit_order_id);
    EXPECT_EQ(result.physical_entry_row, 5u);
    EXPECT_NEAR(result.report.final_equity, result.portfolio_cash, 1e-12);
    const double reconciliation_tolerance =
        64.0 * std::numeric_limits<double>::epsilon() *
        std::max(result.report.initial_equity, std::abs(result.report.final_equity));
    EXPECT_NEAR(result.report.reconciliation_residual, 0.0, reconciliation_tolerance);

    const char* configured_output = std::getenv("TT_OBSERVABILITY_EVIDENCE_DIR");
    if (configured_output && *configured_output) {
        const std::filesystem::path output(configured_output);
        std::filesystem::create_directories(output);
        trace.write(output);
        observability_evidence::write_text(output / "metrics.json", metrics_json(result));
        observability_evidence::write_text(
            output / "semantic-result.csv",
            observability_evidence::canonical_report(result.report, result.portfolio_cash,
                                                     result.entry_order_id, result.exit_order_id,
                                                     result.physical_entry_row));
        observability_evidence::write_text(
            output / "effective-config.json",
            effective_config_json());
    }
}
