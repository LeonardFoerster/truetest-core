#pragma once

#include "core/tt_target.h"

#if TT_TARGET == TT_TARGET_LIVE
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#endif

namespace truetest::bin {

// This entropy is intentionally isolated from every deterministic run input.
// It exists only in engine_live and only for the interactive real-money
// operator challenge. Non-live targets compile a fail-closed stub.
inline bool confirm_real_money_math()
{
#if TT_TARGET == TT_TARGET_LIVE
    const char* red = ::isatty(fileno(stdout)) ? "\033[1;31m" : "";
    const char* yellow = ::isatty(fileno(stdout)) ? "\033[1;33m" : "";
    const char* off = ::isatty(fileno(stdout)) ? "\033[0m" : "";

    std::random_device random_device;
    std::mt19937 random_engine(random_device());
    std::uniform_int_distribution<int> challenge_operand(100, 9999);
    const int lhs = challenge_operand(random_engine);
    const int rhs = challenge_operand(random_engine);
    const int answer = lhs + rhs;

    std::cout << "\n";
    std::cout << red << "  ###############################################\n";
    std::cout << "  #                                             #\n";
    std::cout << "  #   !!!  LIVE TRADING - REAL MONEY  !!!       #\n";
    std::cout << "  #                                             #\n";
    std::cout << "  #   Orders submitted from here will execute   #\n";
    std::cout << "  #   on a real exchange against real funds.    #\n";
    std::cout << "  #   Losses are real and irreversible.         #\n";
    std::cout << "  #                                             #\n";
    std::cout << "  ###############################################" << off
              << "\n\n";

    std::cout << yellow << "  Solve to confirm: " << lhs << " + " << rhs
              << " = " << off;
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line))
    {
        std::cout << "  Aborted (no input).\n";
        return false;
    }

    int given = 0;
    try
    {
        given = std::stoi(line);
    }
    catch (...)
    {
        std::cout << "  ! Could not parse a number from \"" << line
                  << "\". Aborted.\n";
        return false;
    }

    if (given != answer)
    {
        std::cout << "  ! Wrong answer (expected " << answer
                  << "). Aborted.\n";
        return false;
    }

    std::cout << "  Confirmed. Proceeding with live execution.\n";
    return true;
#else
    return false;
#endif
}

} // namespace truetest::bin
