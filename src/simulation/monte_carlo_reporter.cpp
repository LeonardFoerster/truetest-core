#include "monte_carlo_reporter.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {

void write_json_string(std::ostringstream& out, std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    out.put('"');
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c >= 0x80)
        {
            std::size_t width = 0;
            if (c >= 0xc2 && c <= 0xdf) width = 2;
            else if (c >= 0xe0 && c <= 0xef) width = 3;
            else if (c >= 0xf0 && c <= 0xf4) width = 4;
            else throw std::invalid_argument(
                "invalid UTF-8 in MonteCarlo JSON string");
            if (width > value.size() - i)
                throw std::invalid_argument(
                    "truncated UTF-8 in MonteCarlo JSON string");
            for (std::size_t offset = 1; offset < width; ++offset)
            {
                const auto next = static_cast<unsigned char>(value[i + offset]);
                if (next < 0x80 || next > 0xbf)
                    throw std::invalid_argument(
                        "invalid UTF-8 continuation in MonteCarlo JSON string");
            }
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((c == 0xe0 && second < 0xa0)
                || (c == 0xed && second > 0x9f)
                || (c == 0xf0 && second < 0x90)
                || (c == 0xf4 && second > 0x8f))
                throw std::invalid_argument(
                    "non-scalar UTF-8 in MonteCarlo JSON string");
            out.write(value.data() + i,
                      static_cast<std::streamsize>(width));
            i += width - 1;
            continue;
        }
        switch (c)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20)
                {
                    const char escaped[] = {
                        '\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0f]};
                    out.write(escaped, sizeof escaped);
                }
                else out.put(static_cast<char>(c));
        }
    }
    out.put('"');
}

} // namespace

namespace truetest::simulation {

std::string MonteCarloReporter::render_text_summary(const McAggregate& agg,
                                                    const McRunConfig& cfg) {
    std::ostringstream oss;

    oss << "\n=== Monte Carlo Summary (" << agg.n_trials << " trials) ===\n";
    oss << "Generator: " << cfg.generator_name << "\n";
    oss << "Strategy:  " << cfg.strategy_name << "\n\n";

    oss << std::fixed << std::setprecision(2);
    oss << "P&L (mean / median / 5% / 95%): "
        << agg.mean_pnl << " / " << agg.median_pnl << " / "
        << agg.p5_pnl << " / " << agg.p95_pnl << "\n";

    oss << "Sharpe (mean / median): " << agg.mean_sharpe << " / " << agg.median_sharpe << "\n";
    oss << "Sharpe trial coverage (valid / total): "
        << agg.valid_sharpe_trials << " / " << agg.n_trials << "\n";
    oss << "Max DD (mean / worst):  " << agg.mean_max_dd << " / " << agg.worst_max_dd << "\n";

    oss << "\n";
    oss << "Win Rate (mean / median): " << agg.win_rate_mean << " / " << agg.median_win_rate << "\n";
    oss << "Profit Factor (pooled / valid-trial mean / valid-trial median): "
        << agg.profit_factor_pooled << " / " << agg.profit_factor_mean_valid
        << " / " << agg.median_profit_factor_valid;
    if (agg.profit_factor_pooled_unbounded) oss << " (pooled unbounded)";
    oss << "\n";
    oss << "Profit Factor trial coverage (valid / unbounded / total): "
        << agg.valid_profit_factor_trials << " / "
        << agg.unbounded_profit_factor_trials << " / " << agg.n_trials << "\n";
    oss << "Trials with PF > 1: " << agg.trials_with_profit_factor_gt_1 << " / " << agg.n_trials << "\n";

    oss << "Profitable trials: " << agg.trials_with_positive_pnl << " / " << agg.n_trials << "\n";

    return oss.str();
}

std::string MonteCarloReporter::render_json(const McAggregate& agg,
                                            const McRunConfig& cfg) {
    const double values[] = {
        cfg.initial_balance, cfg.risk_fraction, agg.mean_pnl,
        agg.median_pnl, agg.p5_pnl, agg.p95_pnl, agg.mean_sharpe,
        agg.median_sharpe, agg.mean_max_dd, agg.worst_max_dd,
        agg.win_rate_mean, agg.median_win_rate,
        agg.profit_factor_pooled, agg.profit_factor_mean,
        agg.median_profit_factor, agg.profit_factor_mean_valid,
        agg.median_profit_factor_valid};
    for (const double value : values)
        if (!std::isfinite(value))
            throw std::invalid_argument(
                "MonteCarlo JSON contains a non-finite value");
    for (const auto& trial : agg.trials)
    {
        const double trial_values[] = {
            trial.total_pnl, trial.sharpe_ratio, trial.max_drawdown,
            trial.win_rate, trial.profit_factor, trial.total_win,
            trial.total_loss};
        for (const double value : trial_values)
            if (!std::isfinite(value))
                throw std::invalid_argument(
                    "MonteCarlo trial JSON contains a non-finite value");
    }

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "{\"n_trials\":" << agg.n_trials << ",\"generator\":";
    write_json_string(out, cfg.generator_name);
    out << ",\"strategy\":";
    write_json_string(out, cfg.strategy_name);
    out << ",\"initial_balance\":" << cfg.initial_balance
        << ",\"risk_fraction\":" << cfg.risk_fraction
        << ",\"mean_pnl\":" << agg.mean_pnl
        << ",\"median_pnl\":" << agg.median_pnl
        << ",\"p5_pnl\":" << agg.p5_pnl
        << ",\"p95_pnl\":" << agg.p95_pnl
        << ",\"mean_sharpe\":" << agg.mean_sharpe
        << ",\"median_sharpe\":" << agg.median_sharpe
        << ",\"valid_sharpe_trials\":" << agg.valid_sharpe_trials
        << ",\"mean_max_dd\":" << agg.mean_max_dd
        << ",\"worst_max_dd\":" << agg.worst_max_dd
        << ",\"win_rate_mean\":" << agg.win_rate_mean
        << ",\"median_win_rate\":" << agg.median_win_rate
        << ",\"profit_factor_pooled\":" << agg.profit_factor_pooled
        << ",\"profit_factor_pooled_unbounded\":"
        << (agg.profit_factor_pooled_unbounded ? "true" : "false")
        << ",\"profit_factor_mean\":" << agg.profit_factor_mean
        << ",\"median_profit_factor\":" << agg.median_profit_factor
        << ",\"profit_factor_mean_valid\":"
        << agg.profit_factor_mean_valid
        << ",\"median_profit_factor_valid\":"
        << agg.median_profit_factor_valid
        << ",\"valid_profit_factor_trials\":"
        << agg.valid_profit_factor_trials
        << ",\"unbounded_profit_factor_trials\":"
        << agg.unbounded_profit_factor_trials
        << ",\"trials_with_pf_gt_1\":"
        << agg.trials_with_profit_factor_gt_1
        << ",\"profitable_trials\":" << agg.trials_with_positive_pnl
        << ",\"trials\":[";
    for (std::size_t i = 0; i < agg.trials.size(); ++i)
    {
        if (i != 0) out.put(',');
        const auto& trial = agg.trials[i];
        out << "{\"trial_id\":" << trial.trial_id
            << ",\"seed_used\":" << trial.seed_used
            << ",\"total_pnl\":" << trial.total_pnl
            << ",\"sharpe_ratio\":" << trial.sharpe_ratio
            << ",\"sharpe_ratio_valid\":"
            << (trial.sharpe_ratio_valid ? "true" : "false")
            << ",\"accounting_reconciled\":"
            << (trial.accounting_reconciled ? "true" : "false")
            << ",\"max_drawdown\":" << trial.max_drawdown
            << ",\"win_rate\":" << trial.win_rate
            << ",\"profit_factor\":" << trial.profit_factor
            << ",\"profit_factor_valid\":"
            << (trial.profit_factor_valid ? "true" : "false")
            << ",\"profit_factor_unbounded\":"
            << (trial.profit_factor_unbounded ? "true" : "false")
            << ",\"total_win\":" << trial.total_win
            << ",\"total_loss\":" << trial.total_loss
            << ",\"total_trades\":" << trial.total_trades << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace truetest::simulation
