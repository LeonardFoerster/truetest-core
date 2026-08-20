#pragma once

#include "../breakout/breakout_strategy.h"

// Coiled Spring strategy (registered as "coiled-spring").
// This is the canonical name for the multi-factor breakout / volatility expansion
// strategy described in the Coiled_Spring_* documents. It reuses the proven
// technical implementation (ATR, consolidation detection, volume/ATR gates,
// 0.5% risk, scale-out + trailing exits) from breakout_strategy.
class coiled_spring_strategy : public breakout_strategy
{
public:
    using breakout_strategy::breakout_strategy;
};
