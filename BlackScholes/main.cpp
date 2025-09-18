#include <iostream>
#include <cmath>
#include "black_scholes.h"
#include <fstream>
#include <filesystem>
#include <string> 
#include <sstream>


/*
void print_data(std::vector<std::vector<double>> data)
{
    for (auto c : data)
    {
        for (auto v : c)
        {
            std::cout << v << std::endl;
        }
    }
}
*/

struct bs_input
{
    double user_current_price = 0;
    double user_strike_price = 0;
    double user_rate = 0.;
    double user_volatility = 0;
    double user_duration = 1;
    //double user_dividend = 0;

};

int run_csv_calc()
{
    std::ifstream iff("C:\\Users\\Leonard\\Desktop\\call_option_scenarios.csv");
    std::ofstream off("C:\\Users\\Leonard\\Desktop\\results.txt");

    if (!iff.good())
    {
        std::cerr << "Problem beim oeffnen" << std::endl;
        return 1;
    }

    std::string line;
    while (std::getline(iff, line))
    {
        std::stringstream ss(line);
        bs_input bsi;

        ss >> bsi.user_current_price;
        ss >> bsi.user_strike_price;
        ss >> bsi.user_rate;
        ss >> bsi.user_volatility;
        ss >> bsi.user_duration;

        black_scholes user_option
        (
            bsi.user_current_price,
            bsi.user_strike_price,
            bsi.user_rate,
            bsi.user_volatility,
            bsi.user_duration

        );

        double csv_call_price = user_option.get_call_price();
        //double csv_put_price = user_option.get_put_price();
        double csv_delta = user_option.get_delta();


        off << "Call: " << csv_call_price << '|' << "Delta: " << csv_delta << std::endl;


    }
    
}

void run_manual_calc()
{
    bs_input cin;

    cin.user_current_price;
    cin.user_strike_price;
    cin.user_rate;
    cin.user_volatility;
    cin.user_duration;

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

    black_scholes cin_input
    (
        cin.user_current_price,
        cin.user_strike_price,
        cin.user_rate,
        cin.user_volatility,
        cin.user_duration
    );

    double cin_call_price = cin_input.get_call_price();
    //double cin_put_price = cin_input.get_put_price();
    double cin_delta = cin_input.get_delta();
    //std::cout << "Call: " << cin_call_price << '|' << "Put: " << cin_put_price << std::endl;

    std::cout << cin_call_price << std::endl;
    std::cout << cin_delta;
}

int main (int argc, char* argv[]) // default main method
{
    
    



    return 0;
} 