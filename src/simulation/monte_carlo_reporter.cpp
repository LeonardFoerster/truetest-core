#include "monte_carlo_reporter.h"

#include <iomanip>
#include <sstream>

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
    oss << "Max DD (mean / worst):  " << agg.mean_max_dd << " / " << agg.worst_max_dd << "\n";

    oss << "Profitable trials: " << agg.trials_with_positive_pnl << " / " << agg.n_trials << "\n";

    return oss.str();
}

std::string MonteCarloReporter::render_json(const McAggregate& agg,
                                            const McRunConfig& cfg) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"n_trials\": " << agg.n_trials << ",\n";
    oss << "  \"mean_pnl\": " << agg.mean_pnl << ",\n";
    oss << "  \"median_pnl\": " << agg.median_pnl << ",\n";
    oss << "  \"mean_sharpe\": " << agg.mean_sharpe << ",\n";
    oss << "  \"worst_max_dd\": " << agg.worst_max_dd << ",\n";
    oss << "  \"profitable_trials\": " << agg.trials_with_positive_pnl << "\n";
    oss << "}\n";
    return oss.str();
}

} // namespace truetest::simulation
