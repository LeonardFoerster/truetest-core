#include "perform_calculations.h"
#include "black_scholes.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <vector>


struct bs_input
{
    double user_current_price = 0;
    double user_strike_price = 0;
    double user_rate = 0.;
    double user_volatility = 0;
    double user_duration = 1;
    double user_dividend = 0;
};


int run_csv_calc()
{
    auto start = std::chrono::high_resolution_clock::now();

    std::ifstream iff("C:\\Users\\Leonard\\Desktop\\call_option_scenarios.csv");
    std::ofstream off("C:\\Users\\Leonard\\Desktop\\results.txt");

    std::filesystem::path o_path = "C:\\Users\\Leonard\\Desktop\\results.txt";

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
        bs_input bsi;

        ss >> bsi.user_current_price;
        ss >> bsi.user_strike_price;
        ss >> bsi.user_rate;
        ss >> bsi.user_volatility;
        ss >> bsi.user_duration;
        ss >> bsi.user_dividend;

        black_scholes user_option
        (
            bsi.user_current_price,
            bsi.user_strike_price,
            bsi.user_rate,
            bsi.user_volatility,
            bsi.user_duration,
            bsi.user_dividend
        );

                      
        double csv_call_price = user_option.get_call_price();
        double csv_delta = user_option.get_delta();
        double csv_gamma = user_option.get_gamma();
        double csv_theta = user_option.get_theta();
        double csv_vega = user_option.get_vega();
        double csv_call_rho = user_option.get_call_rho();

        off << "Call: " << csv_call_price << "Delta: " << csv_delta << "Gamma: " << csv_gamma << "Theta: " << csv_theta << "Vega: " << csv_vega <<  "Rho: " << csv_call_rho << std::endl;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto run_time_duration = start - end;
    auto run_time_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Data logged in:  " << o_path << std::endl;
    std::cout << "Calculation Time: " << run_time_duration_ms << std::endl;
}

void run_manual_calc()
{
    std::cout << "Manual: Awaiting Input.. " << std::endl;

    bs_input cin;
    cin.user_current_price;
    cin.user_strike_price;
    cin.user_rate;
    cin.user_volatility;
    cin.user_duration;
    cin.user_dividend;

    std::cout << "Current Price: ";
    std::cin >> cin.user_current_price;

    std::cout << "Strike Price: ";
    std::cin >> cin.user_strike_price;

    std::cout << "Interest Rate: ";
    std::cin >> cin.user_rate;

    std::cout << "Volatility: ";
    std::cin >> cin.user_volatility;

    std::cout << "Duration: ";
    std::cin >> cin.user_duration;

    std::cout << "Dividend: ";
    std::cin >> cin.user_dividend;

    black_scholes cin_input
    (
        cin.user_current_price,
        cin.user_strike_price,
        cin.user_rate,
        cin.user_volatility,
        cin.user_duration,
        cin.user_dividend
    );

    double cin_call_price = cin_input.get_call_price();
    double cin_delta = cin_input.get_delta();
    double cin_gamma = cin_input.get_gamma();
    double cin_theta = cin_input.get_theta();
    double cin_vega = cin_input.get_vega();
    double cin_call_rho = cin_input.get_call_rho();


    std::cout << "------------------" << std::endl;
    std::cout << "Call:     " << cin_call_price << std::endl;
    std::cout << "Delta:    " << cin_delta << std::endl;
    std::cout << "Gamma:    " << cin_gamma << std::endl;
    std::cout << "Theta:    " << cin_theta << std::endl;
    std::cout << "Vega:     " << cin_vega << std::endl;
    std::cout << "Rho:      " << cin_call_rho << std::endl;
}
