#include "simulation/monte_carlo_aggregate.h"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

using namespace truetest::simulation;

namespace {

TrialResult trial(std::uint64_t id, double pnl, double wins, double losses)
{
    TrialResult result;
    result.trial_id = id;
    result.initial_equity = 1'000.0;
    result.final_equity = result.initial_equity + pnl;
    result.total_pnl = result.final_equity - result.initial_equity;
    result.accounting_reconciled = true;
    result.total_win = wins;
    result.total_loss = losses;
    result.profit_factor = losses > 0.0 ? wins / losses : 0.0;
    result.profit_factor_valid = losses > 0.0;
    result.profit_factor_unbounded = losses == 0.0 && wins > 0.0;
    return result;
}

} // namespace

TEST(MonteCarloAggregate, StableReductionReconcilesFiniteTrials)
{
    McAggregate aggregate;
    aggregate.trials = {
        trial(0, 1.0e15, 1.0e15, 1.0),
        trial(1, -1.0e15, 0.0, 1.0e15),
        trial(2, 1.0, 2.0, 1.0)};

    ASSERT_NO_THROW(summarize_monte_carlo_trials(aggregate));
    EXPECT_EQ(aggregate.n_trials, 3u);
    EXPECT_DOUBLE_EQ(aggregate.mean_pnl, 1.0 / 3.0);
    EXPECT_DOUBLE_EQ(aggregate.profit_factor_pooled, 1.0);
}

TEST(MonteCarloAggregate, EvenCardinalityMediansUseStableMidpoints)
{
    auto low = trial(0, -10.0, 2.0, 1.0);
    low.sharpe_ratio = -std::numeric_limits<double>::max();
    low.sharpe_ratio_valid = true;
    low.total_trades = 5;
    low.winning_trades = 1;
    low.win_rate = 20.0;
    auto high = trial(1, 10.0, 4.0, 1.0);
    high.sharpe_ratio = std::numeric_limits<double>::max();
    high.sharpe_ratio_valid = true;
    high.total_trades = 5;
    high.winning_trades = 4;
    high.win_rate = 80.0;

    McAggregate aggregate;
    aggregate.trials = {high, low};
    summarize_monte_carlo_trials(aggregate);

    EXPECT_DOUBLE_EQ(aggregate.median_pnl, 0.0);
    EXPECT_DOUBLE_EQ(aggregate.median_sharpe, 0.0);
    EXPECT_DOUBLE_EQ(aggregate.median_win_rate, 50.0);
    EXPECT_DOUBLE_EQ(aggregate.median_profit_factor, 3.0);
    EXPECT_DOUBLE_EQ(aggregate.median_profit_factor_valid, 3.0);
}

TEST(MonteCarloAggregate, CanonicalizesCompletionOrderBeforeReduction)
{
    McAggregate forward;
    forward.trials = {
        trial(1, 0.25, 1.0, 2.0),
        trial(2, 1.0e15, 1.0e15, 1.0),
        trial(3, -1.0e15, 0.0, 1.0e15)};
    McAggregate reverse;
    reverse.trials = {forward.trials[2], forward.trials[0], forward.trials[1]};

    summarize_monte_carlo_trials(forward);
    summarize_monte_carlo_trials(reverse);

    ASSERT_EQ(forward.trials.size(), reverse.trials.size());
    for (std::size_t i = 0; i < forward.trials.size(); ++i)
    {
        EXPECT_EQ(forward.trials[i].trial_id, i + 1);
        EXPECT_EQ(forward.trials[i].trial_id, reverse.trials[i].trial_id);
        EXPECT_DOUBLE_EQ(forward.trials[i].total_pnl,
                         reverse.trials[i].total_pnl);
    }
    EXPECT_DOUBLE_EQ(forward.mean_pnl, reverse.mean_pnl);
    EXPECT_DOUBLE_EQ(forward.profit_factor_pooled,
                     reverse.profit_factor_pooled);
}

TEST(MonteCarloAggregate, RejectsNonFiniteAndContradictoryTrials)
{
    const auto rejects = [](TrialResult value) {
        McAggregate aggregate;
        aggregate.trials.push_back(value);
        EXPECT_THROW(summarize_monte_carlo_trials(aggregate),
                     std::invalid_argument);
    };

    auto value = trial(0, 1.0, 1.0, 0.0);
    value.total_pnl = std::numeric_limits<double>::quiet_NaN();
    rejects(value);

    value = trial(0, 1.0, 1.0, 0.0);
    value.final_equity += 1.0;
    rejects(value);

    value = trial(0, 1.0, 2.0, 1.0);
    value.profit_factor = 3.0;
    rejects(value);

    value = trial(0, 0.0, 1.0e-16, 1.0);
    value.profit_factor = 0.0;
    rejects(value);

    value = trial(0, 1.0, 2.0, 1.0);
    value.profit_factor_valid = false;
    rejects(value);

    value = trial(0, 1.0, 1.0, 0.0);
    value.profit_factor_unbounded = false;
    rejects(value);

    value = trial(0, 0.0, 0.0, 0.0);
    value.profit_factor_unbounded = true;
    rejects(value);

    value = trial(0, 0.0, 0.0, 0.0);
    value.accounting_reconciled = false;
    rejects(value);

    value = trial(0, 0.0, 0.0, 0.0);
    value.sharpe_ratio = 1.0;
    rejects(value);

    value = trial(0, 0.0, 0.0, 0.0);
    value.win_rate = 101.0;
    rejects(value);

    value = trial(0, 0.0, 0.0, 0.0);
    value.total_trades = 1;
    value.winning_trades = 1;
    value.win_rate = 0.0;
    rejects(value);

    // The parallel controller's former exception fallback was a default
    // TrialResult. It must abort the campaign, never count as a flat trial.
    rejects(TrialResult{});
}

TEST(MonteCarloAggregate, RejectsDuplicateTrialIdentity)
{
    McAggregate aggregate;
    aggregate.trials = {
        trial(7, 1.0, 1.0, 0.0),
        trial(7, 2.0, 2.0, 0.0)};
    EXPECT_THROW(summarize_monte_carlo_trials(aggregate),
                 std::invalid_argument);
}

TEST(MonteCarloAggregate, RejectionDoesNotPartiallyMutatePriorSummary)
{
    McAggregate aggregate;
    aggregate.trials = {trial(0, 10.0, 10.0, 0.0)};
    summarize_monte_carlo_trials(aggregate);
    ASSERT_DOUBLE_EQ(aggregate.mean_pnl, 10.0);

    aggregate.trials.push_back(TrialResult{});
    EXPECT_THROW(summarize_monte_carlo_trials(aggregate),
                 std::invalid_argument);
    EXPECT_EQ(aggregate.n_trials, 1u);
    EXPECT_DOUBLE_EQ(aggregate.mean_pnl, 10.0);
    ASSERT_EQ(aggregate.trials.size(), 2u);
}
