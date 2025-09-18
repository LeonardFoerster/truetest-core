#include <iostream>
#include <cmath>
#include "black_scholes.h"
#include <fstream>
#include <filesystem>
#include <string> 
#include <sstream>
#include <chrono>


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
    double user_dividend = 0;

};

int run_csv_calc()
{
    std::ifstream iff("C:\\Users\\Leonard\\Desktop\\call_option_scenarios.csv");
    std::ofstream off("C:\\Users\\Leonard\\Desktop\\results.txt");

    std::filesystem::path o_path = "C:\\Users\\Leonard\\Desktop\\results.txt";

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

        off << "Call: " << csv_call_price << '|' << "Delta: " << csv_delta << std::endl;
        
    }
    std::cout << "Die Daten finden sich in: " << o_path;
}

void run_manual_calc()
{
    std::cout << "Manuller Modus erwarte Input.. " << std::endl;    
    
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


    std::cout << "------------------" << std::endl;
    std::cout << "Call: " << cin_call_price << std::endl;
    std::cout << "Delta: " << cin_delta << std::endl;
    std::cout << "Gamma: " << cin_gamma << std::endl;
}



int main (int argc, char* argv[]) // default main method
{
    std::cout << "Eigene Daten(e) oder CSV(c)?" << std::endl;
    char decision = 'x';
    std::cin >> decision;
    
    while (decision != 'e' || decision != 'c') // Problem er meckert
    {
        std::cout << "Falscher Modus erwarte neue Eingabe" << std::endl;        
        std::cin >> decision;

        switch (decision)
        {
            case 'e':
            run_manual_calc();
                break;
            case 'c':
            run_csv_calc();
                break;
        }
    }

    return 0;
} 