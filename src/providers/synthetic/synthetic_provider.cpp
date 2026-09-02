#include "synthetic_provider.h"

#include "engine/engine_config.h"
#include "simulation/monte_carlo_types.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace truetest::simulation {

namespace {

std::string_view trim(std::string_view value)
{
    const auto whitespace = [](unsigned char c) { return std::isspace(c); };
    while (!value.empty()
           && whitespace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty()
           && whitespace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

template <typename T>
T parse_exact(std::string_view value, std::string_view key)
{
    value = trim(value);
    T parsed{};
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{}
        || end != value.data() + value.size())
        throw std::invalid_argument(
            "invalid synthetic provider value for " + std::string(key));
    return parsed;
}

bool parse_bool(std::string_view value, std::string_view key)
{
    value = trim(value);
    if (value == "1" || value == "true") return true;
    if (value == "0" || value == "false") return false;
    throw std::invalid_argument(
        "invalid synthetic provider boolean for " + std::string(key));
}

std::string_view canonical_param_key(std::string_view key)
{
    return key == "n_bars" ? std::string_view{"n_steps"} : key;
}

void claim_param(std::unordered_set<std::string>& seen, std::string_view key)
{
    const std::string canonical(canonical_param_key(key));
    if (!seen.insert(canonical).second)
        throw std::invalid_argument(
            "duplicate synthetic provider parameter: " + canonical);
}

void apply_param(McGeneratorConfig& out, std::string_view key,
                 std::string_view value)
{
    if (key == "symbol") out.symbol = std::string(trim(value));
    else if (key == "initial_price")
        out.initial_price = parse_exact<double>(value, key);
    else if (key == "n_steps" || key == "n_bars")
        out.n_steps = parse_exact<std::int64_t>(value, key);
    else if (key == "dt") out.dt = parse_exact<double>(value, key);
    else if (key == "mu") out.mu = parse_exact<double>(value, key);
    else if (key == "sigma") out.sigma = parse_exact<double>(value, key);
    else if (key == "emit_l2") out.emit_synthetic_l2 = parse_bool(value, key);
    else if (key == "spread_bps")
        out.base_spread_bps = parse_exact<double>(value, key);
    else if (key == "depth_noise")
        out.depth_noise = parse_exact<double>(value, key);
    else
        throw std::invalid_argument(
            "unknown synthetic provider parameter: " + std::string(key));
}

// Strict key=value parser for --mc-params.
void apply_params_string(McGeneratorConfig& out, const std::string& params,
                         std::unordered_set<std::string>& seen) {
    std::istringstream iss(params);
    std::string token;
    while (std::getline(iss, token, ',')) {
        auto eq = token.find('=');
        if (eq == std::string::npos || token.find('=', eq + 1) != std::string::npos)
            throw std::invalid_argument(
                "synthetic provider parameter must be key=value");
        const std::string_view key_view = trim(
            std::string_view(token).substr(0, eq));
        const std::string_view value_view = trim(
            std::string_view(token).substr(eq + 1));
        if (key_view.empty() || value_view.empty())
            throw std::invalid_argument(
                "synthetic provider parameter key/value must not be empty");
        claim_param(seen, key_view);
        apply_param(out, key_view, value_view);
    }
    if (params.empty() || (!params.empty() && params.back() == ','))
        throw std::invalid_argument("synthetic provider parameter list is empty or trailing");
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
    std::unordered_set<std::string> seen;

    // Pull common keys from provider_config (populated by main.inc)
    if (auto it = config_.find("symbol"); it != config_.end()) {
        claim_param(seen, "symbol");
        apply_param(gen_config_, "symbol", it->second);
    }
    if (config_.contains("n_steps") && config_.contains("n_bars"))
        throw std::invalid_argument(
            "synthetic provider accepts only one of n_steps/n_bars");
    for (const std::string_view key : {
             "initial_price", "n_steps", "n_bars", "dt", "mu", "sigma",
             "emit_l2", "spread_bps", "depth_noise"})
        if (auto it = config_.find(std::string(key)); it != config_.end())
        {
            claim_param(seen, key);
            apply_param(gen_config_, key, it->second);
        }
    if (config_.contains("mc_params") && config_.contains("params"))
        throw std::invalid_argument(
            "synthetic provider accepts only one of mc_params/params");
    if (auto it = config_.find("mc_params"); it != config_.end() && !it->second.empty()) {
        apply_params_string(gen_config_, it->second, seen);
    }
    // Also support "params" as alias (and future --param style)
    if (auto it = config_.find("params"); it != config_.end() && !it->second.empty()) {
        apply_params_string(gen_config_, it->second, seen);
    }

    validate_mc_generator_config(gen_config_);
    if (config_.contains("seed"))
        throw std::invalid_argument(
            "synthetic provider refuses a provider-local seed; use the explicit master seed");
    if (gen_config_.emit_synthetic_l2)
        throw std::invalid_argument(
            "synthetic provider L2 is unsupported by its bar transport");
}

void SyntheticProvider::ensure_generator() {
    // Phase 1: only GBM supported
    if (!generator_) {
        generator_ = std::make_unique<GBMGenerator>();
    }
}

bool SyntheticProvider::open() {
    if (opened_) return true;
    if (!deterministic_seed_configured_)
        throw std::logic_error(
            "synthetic provider requires configure() with an explicit master seed before open()");

    ensure_generator();

    auto generated_path = generator_->generate(gen_config_.seed, gen_config_);
    transport_ = std::make_shared<SyntheticTransport>(
        std::move(generated_path.bars));
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

void SyntheticProvider::configure(const engine_config& ecfg) {
    if (!ecfg.seed_explicitly_set)
        throw std::invalid_argument(
            "synthetic provider requires an explicit deterministic master seed");
    gen_config_.seed = derive_synthetic_price_seed(ecfg.seed);
    deterministic_seed_configured_ = true;
}

} // namespace truetest::simulation
