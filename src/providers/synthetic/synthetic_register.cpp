#include "providers/provider_registry.h"
#include "providers/synthetic/synthetic_provider.h"

#include <memory>

namespace truetest::simulation {

// Register under both "synthetic" and "montecarlo" aliases for discoverability
static const bool registered_synthetic = [] {
    ProviderRegistry::instance().register_provider("synthetic", [](const provider_config& cfg) {
        return std::make_shared<SyntheticProvider>(cfg);
    });
    ProviderRegistry::instance().register_provider("montecarlo", [](const provider_config& cfg) {
        return std::make_shared<SyntheticProvider>(cfg);
    });
    return true;
}();

} // namespace truetest::simulation
