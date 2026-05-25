#pragma once

#include "providers/provider.h"
#include "providers/provider_registry.h"   // for provider_config
#include "providers/synthetic/synthetic_transport.h"
#include "simulation/generators/gbm_generator.h"

#include <memory>
#include <string>

namespace truetest::simulation {

/**
 * Synthetic / Monte-Carlo provider for TrueTest.
 *
 * Generates stochastic price paths on demand and exposes them
 * through the standard IProvider + IDataTransport interface.
 *
 * In Phase 1 this provider only supports bar-mode backtesting via
 * a generated CSV-like in-memory transport (compatible with CsvBarParser).
 *
 * Usage example:
 *   --provider synthetic --mc-model gbm --mc-params "mu=0.05,sigma=0.65,n_steps=8000"
 */
class SyntheticProvider : public IProvider {
public:
    explicit SyntheticProvider(const provider_config& cfg);

    std::string name() const override { return "synthetic"; }

    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return false; }

    bool open() override;
    void close() override;

    std::shared_ptr<IDataTransport> get_transport() override;

    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override {
        return nullptr;
    }

    void configure(const engine_config& ecfg) override;

private:
    provider_config config_;
    McGeneratorConfig gen_config_;
    std::unique_ptr<IMonteCarloGenerator> generator_;
    SyntheticPath generated_path_;
    std::shared_ptr<SyntheticTransport> transport_;
    bool opened_ = false;

    void parse_config();
    void ensure_generator();
};

} // namespace truetest::simulation
