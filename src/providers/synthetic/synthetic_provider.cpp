#include "synthetic_provider.h"

#include "simulation/monte_carlo_types.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace truetest::simulation {

namespace {

// Very small key=value parser for --mc-params "mu=0.05,sigma=0.8,n_steps=10000"
void apply_params_string(McGeneratorConfig& out, const std::string& params) {
    std::istringstream iss(params);
    std::string token;
    while (std::getline(iss, token, ',')) {
        auto eq = token.find('=');
        if (eq == std::string::npos) continue;

        std::string key = token.substr(0, eq);
        std::string val = token.substr(eq + 1);

        // trim
        key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
        val.erase(std::remove_if(val.begin(), val.end(), ::isspace), val.end());

        if (key.empty()) continue;

        try {
            if (key == "symbol")               out.symbol = val;
            else if (key == "initial_price")   out.initial_price = std::stod(val);
            else if (key == "n_steps" || key == "n_bars") out.n_steps = std::stoll(val);
            else if (key == "dt")              out.dt = std::stod(val);
            else if (key == "mu")              out.mu = std::stod(val);
            else if (key == "sigma")           out.sigma = std::stod(val);
            else if (key == "seed")            out.seed = std::stoull(val);
            else if (key == "emit_l2")         out.emit_synthetic_l2 = (val == "1" || val == "true");
            else if (key == "spread_bps")      out.base_spread_bps = std::stod(val);
        } catch (...) {
            // ignore bad values for robustness in Phase 1
        }
    }
}

} // anonymous namespace

SyntheticProvider::SyntheticProvider(const provider_config& cfg)
    : config_(cfg)
{
    parse_config();
    ensure_generator();
}

void SyntheticProvider::parse_config() {
    gen_config_ = McGeneratorConfig{}; // start with sensible defaults

    // Pull common keys from provider_config (populated by main.inc)
    if (auto it = config_.find("symbol"); it != config_.end() && !it->second.empty()) {
        gen_config_.symbol = it->second;
    }
    if (auto it = config_.find("initial_price"); it != config_.end()) {
        try { gen_config_.initial_price = std::stod(it->second); } catch (...) {}
    }
    if (auto it = config_.find("n_steps"); it != config_.end() || (it = config_.find("n_bars")) != config_.end()) {
        try { gen_config_.n_steps = std::stoll(it->second); } catch (...) {}
    }
    if (auto it = config_.find("mu"); it != config_.end()) {
        try { gen_config_.mu = std::stod(it->second); } catch (...) {}
    }
    if (auto it = config_.find("sigma"); it != config_.end()) {
        try { gen_config_.sigma = std::stod(it->second); } catch (...) {}
    }
    if (auto it = config_.find("seed"); it != config_.end()) {
        try { gen_config_.seed = std::stoull(it->second); } catch (...) {}
    }
    if (auto it = config_.find("mc_params"); it != config_.end() && !it->second.empty()) {
        apply_params_string(gen_config_, it->second);
    }
    // Also support "params" as alias (and future --param style)
    if (auto it = config_.find("params"); it != config_.end() && !it->second.empty()) {
        apply_params_string(gen_config_, it->second);
    }

    // Phase 1 convenience: if user did almost nothing, still produce a usable short path
    if (gen_config_.n_steps < 10) {
        gen_config_.n_steps = 500; // short but non-trivial demo
    }
    if (gen_config_.sigma < 0.001) {
        gen_config_.sigma = 0.4;   // reasonable default vol
    }
}

void SyntheticProvider::ensure_generator() {
    // Phase 1: only GBM supported
    if (!generator_) {
        generator_ = std::make_unique<GBMGenerator>();
    }
}

bool SyntheticProvider::open() {
    if (opened_) return true;

    ensure_generator();

    uint64_t use_seed = gen_config_.seed;
    if (use_seed == 0) {
        // Derive a reasonable default seed if none provided
        use_seed = 0xC0FFEE42ULL;
    }

    generated_path_ = generator_->generate(use_seed, gen_config_);

    transport_ = std::make_shared<SyntheticTransport>(generated_path_);
    opened_ = transport_->open();
    return opened_;
}

void SyntheticProvider::close() {
    if (transport_) {
        transport_->close();
    }
    opened_ = false;
}

std::shared_ptr<IDataTransport> SyntheticProvider::get_transport() {
    return transport_;
}

void SyntheticProvider::configure(const engine_config& /*ecfg*/) {
    // Phase 1: nothing special to pull from engine_config
}

} // namespace truetest::simulation
