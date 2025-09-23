
#include "perform_calculations.h"
#include "black_scholes.h"
#include "strategy.h"
#include "portfolio.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <vector>
#include <string>

struct bs_input
{
    double user_current_price = 0.0;
    double user_strike_price = 0.0;
    double user_rate = 0.0;
    double user_volatility = 0.0;
    double user_duration = 0.0;
    double user_dividend = 0.0;
};


void print_manual_results(const black_scholes & cin_input)
{
    std::cout << "------------------" << std::endl;
    std::cout << "Call:     " << cin_input.get_call_price() << std::endl;
    std::cout << "Delta:    " << cin_input.get_delta()      << std::endl;
    std::cout << "Gamma:    " << cin_input.get_gamma()      << std::endl;
    std::cout << "Theta:    " << cin_input.get_theta()      << std::endl;
    std::cout << "Vega:     " << cin_input.get_vega()       << std::endl;
    std::cout << "Rho:      " << cin_input.get_call_rho()   << std::endl;
}


int run_csv_calc()
{
    auto start = std::chrono::high_resolution_clock::now();

    std::ifstream iff("C:\\Users\\Leonard\\option_scenarios.csv");
    std::ofstream off("C:\\Users\\Leonard\\Desktop\\results.txt");
    std::filesystem::path o_path = "C:\\Users\\Leonard\\Desktop\\results.txt";

    strategy strat_test;
    portfolio portfolio_test(10000);

    if (!iff.good())
    {
        std::cerr << "Error opening file" << std::endl;
        return 1;
    }

    std::string line;
    std::vector <double> data;
    
    while (std::getline(iff, line)) 
    {
        std::stringstream ss(line);
        std::string value;
        std::vector<double> values;
        while (std::getline(ss, value, ',')) 
        { 
            try 
            {
                values.push_back(std::stod(value));
            }
            catch (const std::invalid_argument& e) 
            {
                std::cerr << "Could not convert string to double: " << value << std::endl;
                continue; 
            }
        }

        if (values.size() < 6) continue; 

        double current_price = values[0];

        black_scholes option_calculator(values[0], values[1], values[2], values[3], values[4], values[5]);
        double fair_price = option_calculator.get_call_price();
        signal_event signal = strat_test.check_for_signal(current_price, fair_price);
        portfolio_test.execute_signal(signal, current_price);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto run_time_duration = start - end;
    auto run_time_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    portfolio_test.print_summary();

    std::cout << "Data logged in:  " << o_path << std::endl;
    std::cout << "Calculation Time: " << run_time_duration_ms << std::endl;
}

void run_manual_calc()
{
    strategy strategy;
    portfolio portfolio(10000);

    std::cout << "Awaiting manual Input.. " << std::endl;
    
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

    auto start = std::chrono::high_resolution_clock::now();

    black_scholes cin_input
    (
        user_current_price,
        user_strike_price,
        user_rate,
        user_volatility,
        user_duration,
        user_dividend
    );

    double cin_call_price = cin_input.get_call_price();     
    
    signal_event signal = strategy.check_for_signal(user_current_price, cin_call_price);

    if (signal != signal_event::hold)
    {
        portfolio.execute_signal(signal, user_current_price);
    }
    
        //print_manual_results(cin_input);
    
       
      
    
}
