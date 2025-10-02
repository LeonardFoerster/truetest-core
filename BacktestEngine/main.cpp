#include "black_scholes.h"
#include "perform_calculations.h"
#include "data_handler.h"

#include <iostream>
#include <stdexcept> 
#include <chrono>


int main (int argc, char* argv[]) // default main method
{
    std::filesystem::path ohlc_data_path_ = "C:\\Users\\Leonard\\aktien_szenarien.csv";
    std::filesystem::path bs_data_path_ = "C:\\Users\\Leonard\\Desktop\\options_scenarios.csv";
           
    std::cout << "--- Backtesting Engine ---" << std::endl;
    std::cout << "Datenquelle waehlen:" << std::endl;
    std::cout << "  [o] OHLC-Daten " << std::endl;
    std::cout << "  [b] Black-Scholes-Daten" << std::endl;
    std::cout << "  [e] Eigene Eingabe " << std::endl;

    char decision_char;
    std::cin >> decision_char;

    std::filesystem::path selected_path;
    data_type_content selected_data;

    switch (decision_char)
    {
        case 'e':
            run_manual_calc();
                break;
        default:
            std::cerr << "Wrong input" << std::endl;
                return 1;
    }
    
    return 0;
} 