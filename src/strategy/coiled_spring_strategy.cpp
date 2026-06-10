#include "coiled_spring_strategy.h"
#include "strategy_registry.h"

REGISTER_STRATEGY("coiled-spring", []() {
    return std::make_shared<coiled_spring_strategy>();
})
