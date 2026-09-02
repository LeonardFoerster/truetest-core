#include "simulation/monte_carlo_aggregate.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace truetest::simulation {

namespace {

class compensated_sum
{
public:
    void add(double value) noexcept
    {
        const long double operand = static_cast<long double>(value);
        const long double next = sum_ + operand;
        if (std::abs(sum_) >= std::abs(operand))
            correction_ += (sum_ - next) + operand;
        else
            correction_ += (operand - next) + sum_;
        sum_ = next;
    }

    long double value() const noexcept { return sum_ + correction_; }

private:
    long double sum_ = 0.0L;
    long double correction_ = 0.0L;
};

double checked_double(long double value, const char* field)
{
    constexpr long double limit =
        static_cast<long double>(std::numeric_limits<double>::max());
    if (!std::isfinite(value) || std::abs(value) > limit)
        throw std::overflow_error(
            std::string("MonteCarlo aggregate is non-representable: ")
            + field);
    return static_cast<double>(value);
}

void validate_trial(const TrialResult& trial)
{
    const double values[] = {
        trial.initial_equity, trial.final_equity, trial.total_pnl,
        trial.max_drawdown, trial.sharpe_ratio, trial.win_rate,
        trial.profit_factor, trial.total_win, trial.total_loss};
    if (!std::all_of(std::begin(values), std::end(values),
                     [](double value) { return std::isfinite(value); }))
        throw std::invalid_argument(
            "MonteCarlo trial contains a non-finite economic value");
    if (!(trial.initial_equity > 0.0) || trial.max_drawdown < 0.0
        || trial.win_rate < 0.0
        || trial.win_rate > 100.0 || trial.profit_factor < 0.0
        || trial.total_win < 0.0 || trial.total_loss < 0.0
        || trial.winning_trades > trial.total_trades)
        throw std::invalid_argument(
            "MonteCarlo trial contains an out-of-domain metric");
    if (!trial.accounting_reconciled)
        throw std::invalid_argument(
            "MonteCarlo trial contains unreconciled accounting");
    if (!trial.sharpe_ratio_valid && trial.sharpe_ratio != 0.0)
        throw std::invalid_argument(
            "MonteCarlo invalid Sharpe trial has a non-zero value");

    const double reconciled_pnl =
        trial.final_equity - trial.initial_equity;
    if (!std::isfinite(reconciled_pnl)
        || trial.total_pnl != reconciled_pnl)
        throw std::invalid_argument(
            "MonteCarlo trial PnL does not reconcile with equity");

    const double expected_win_rate = trial.total_trades == 0U
        ? 0.0
        : static_cast<double>(trial.winning_trades)
            / static_cast<double>(trial.total_trades) * 100.0;
    if (!std::isfinite(expected_win_rate)
        || trial.win_rate != expected_win_rate)
        throw std::invalid_argument(
            "MonteCarlo trial win rate does not reconcile with trade counts");

    if (trial.total_loss > 0.0)
    {
        const double expected = trial.total_win / trial.total_loss;
        if (!std::isfinite(expected) || trial.profit_factor != expected
            || !trial.profit_factor_valid
            || trial.profit_factor_unbounded)
            throw std::invalid_argument(
                "MonteCarlo trial profit factor does not reconcile");
    }
    else if (trial.total_win > 0.0)
    {
        if (trial.profit_factor != 0.0
            || trial.profit_factor_valid
            || !trial.profit_factor_unbounded)
            throw std::invalid_argument(
                "MonteCarlo unbounded profit factor lacks its status");
    }
    else if (trial.profit_factor != 0.0
             || trial.profit_factor_valid
             || trial.profit_factor_unbounded)
        throw std::invalid_argument(
            "MonteCarlo zero-ledger trial has a non-zero profit factor");
}

double stable_mean(const std::vector<double>& values)
{
    if (values.empty()) return 0.0;
    compensated_sum sum;
    for (const double value : values) sum.add(value);
    return checked_double(
        sum.value() / static_cast<long double>(values.size()), "mean");
}

double median_sorted(const std::vector<double>& values) noexcept
{
    if (values.empty()) return 0.0;
    const std::size_t upper = values.size() / 2U;
    if ((values.size() & 1U) != 0U)
        return values[upper];
    return std::midpoint(values[upper - 1U], values[upper]);
}

std::size_t percentile_index(std::size_t size,
                             std::size_t percentile) noexcept
{
    return (size / 100U) * percentile
        + ((size % 100U) * percentile) / 100U;
}

} // namespace

void summarize_monte_carlo_trials(McAggregate& aggregate)
{
    // Reduce into a separate value so validation/overflow/allocation failures
    // cannot leave a previously valid aggregate partially overwritten.
    McAggregate output;
    output.trials = aggregate.trials;
    std::sort(output.trials.begin(), output.trials.end(),
              [](const TrialResult& lhs, const TrialResult& rhs) {
                  return lhs.trial_id < rhs.trial_id;
              });
    output.wall_time_ms = aggregate.wall_time_ms;
    output.n_trials = output.trials.size();

    if (output.trials.empty()) {
        aggregate = std::move(output);
        return;
    }

    const std::size_t count = output.trials.size();
    std::vector<double> pnls;
    std::vector<double> sharpes;
    std::vector<double> maxdds;
    std::vector<double> win_rates;
    std::vector<double> valid_profit_factors;
    pnls.reserve(count);
    sharpes.reserve(count);
    maxdds.reserve(count);
    win_rates.reserve(count);
    valid_profit_factors.reserve(count);

    std::unordered_set<std::uint64_t> trial_ids;
    trial_ids.reserve(count);
    compensated_sum pooled_win;
    compensated_sum pooled_loss;
    for (const auto& trial : output.trials)
    {
        validate_trial(trial);
        if (!trial_ids.insert(trial.trial_id).second)
            throw std::invalid_argument(
                "MonteCarlo campaign contains a duplicate trial_id");
        pnls.push_back(trial.total_pnl);
        if (trial.sharpe_ratio_valid)
        {
            sharpes.push_back(trial.sharpe_ratio);
            ++output.valid_sharpe_trials;
        }
        maxdds.push_back(trial.max_drawdown);
        win_rates.push_back(trial.win_rate);
        pooled_win.add(trial.total_win);
        pooled_loss.add(trial.total_loss);

        if (trial.total_loss > 0.0)
        {
            valid_profit_factors.push_back(checked_double(
                static_cast<long double>(trial.total_win)
                    / static_cast<long double>(trial.total_loss),
                "trial profit factor"));
            ++output.valid_profit_factor_trials;
        }
        else if (trial.total_win > 0.0)
            ++output.unbounded_profit_factor_trials;
        if (trial.total_pnl > 0.0)
            ++output.trials_with_positive_pnl;
        if (trial.profit_factor_unbounded
            || (trial.profit_factor_valid && trial.profit_factor > 1.0))
            ++output.trials_with_profit_factor_gt_1;
    }

    // Means are deliberately reduced in the already canonical trial-index
    // order. Separate value-order sorts below are only for order statistics.
    output.mean_pnl = stable_mean(pnls);
    output.mean_sharpe = stable_mean(sharpes);
    output.mean_max_dd = stable_mean(maxdds);
    output.win_rate_mean = stable_mean(win_rates);
    output.profit_factor_mean_valid = stable_mean(valid_profit_factors);

    std::sort(pnls.begin(), pnls.end());
    std::sort(sharpes.begin(), sharpes.end());
    std::sort(maxdds.begin(), maxdds.end());
    std::sort(win_rates.begin(), win_rates.end());
    std::sort(valid_profit_factors.begin(), valid_profit_factors.end());

    output.median_pnl = median_sorted(pnls);
    output.p5_pnl = pnls[percentile_index(count, 5U)];
    output.p95_pnl = pnls[percentile_index(count, 95U)];
    output.median_sharpe = median_sorted(sharpes);
    output.worst_max_dd = maxdds.back();
    output.median_win_rate = median_sorted(win_rates);

    const long double total_win = pooled_win.value();
    const long double total_loss = pooled_loss.value();
    if (total_loss > 0.0L)
        output.profit_factor_pooled = checked_double(
            total_win / total_loss, "pooled profit factor");
    else if (total_win > 0.0L)
    {
        output.profit_factor_pooled = 0.0;
        output.profit_factor_pooled_unbounded = true;
    }
    if (!valid_profit_factors.empty())
        output.median_profit_factor_valid =
            median_sorted(valid_profit_factors);
    output.profit_factor_mean = output.profit_factor_mean_valid;
    output.median_profit_factor = output.median_profit_factor_valid;

    aggregate = std::move(output);
}

} // namespace truetest::simulation
