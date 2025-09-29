#include "perform_calculations.h"
#include "black_scholes.h"
#include "strategy.h"
#include "portfolio.h"
#include "data_handler.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <sstream> 

struct bs_input
{
    double user_current_price = 0.0;
    double user_strike_price = 0.0;
    double user_rate = 0.0;
    double user_volatility = 0.0;
    double user_duration = 0.0;
    double user_dividend = 0.0;
};

void print_manual_results(const black_scholes& cin_input)
{
    std::cout << "------------------" << std::endl;
    std::cout << "Call:     " << cin_input.get_call_price() << std::endl;
    std::cout << "Delta:    " << cin_input.get_delta() << std::endl;
    std::cout << "Gamma:    " << cin_input.get_gamma() << std::endl;
    std::cout << "Theta:    " << cin_input.get_theta() << std::endl;
    std::cout << "Vega:     " << cin_input.get_vega() << std::endl;
    std::cout << "Rho:      " << cin_input.get_call_rho() << std::endl;
}

void run_manual_calc()
{

    //strategy strategy;
    //portfolio portfolio(10000);

    std::cout << "Awaiting manual Input: " << std::endl;

    double user_current_price;
    double user_strike_price;
    double user_rate;
    double user_volatility;
    double user_duration;
    double user_dividend;

    std::cout << "Current Price: ";
    std::cin >> user_current_price;

    std::cout << "Strike Price: ";
    std::cin >> user_strike_price;

    std::cout << "Interest Rate: ";
    std::cin >> user_rate;

    std::cout << "Volatility: ";
    std::cin >> user_volatility;

    std::cout << "Duration: ";
    std::cin >> user_duration;

    std::cout << "Dividend: ";
    std::cin >> user_dividend;

    black_scholes cin_input
    (
        user_current_price,
        user_strike_price,
        user_rate,
        user_volatility,
        user_duration,
        user_dividend
    );

    print_manual_results(cin_input);
}