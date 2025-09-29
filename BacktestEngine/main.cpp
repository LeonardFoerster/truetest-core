#include "black_scholes.h"
#include "perform_calculations.h"
#include "data_handler.h"

#include <iostream>


int main (int argc, char* argv[]) // default main method
{

    std::filesystem::path ohlc_data_path_ = "C:\\Users\\Leonard\\aktien_szenarien.csv";
    std::filesystem::path bs_data_path_ = "C:\\Users\\Leonard\\Desktop\\options_scenarios.csv";

    std::cout << "--- Backtesting Engine ---" << std::endl;
    std::cout << "Datenquelle waehlen:" << std::endl;
    std::cout << "  [o] OHLC-Daten (Aktienkurs-Simulation)" << std::endl;
    std::cout << "  [b] Black-Scholes-Daten (Optionsszenarien)" << std::endl;

    char decision_char;
    std::cin >> decision_char;
    
    std::filesystem::path selected_path;
    data_type_content selected_data;

    switch (decision_char)
    {
    case 'o':
        selected_data = data_type_content::ohlc_data;
        selected_path = ohlc_data_path_;
        break;

    case 'b':
        selected_data = data_type_content::black_scholes_data;
        selected_path = bs_data_path_;
        break;

    default:
        std::cerr << "Ungueltige Auswahl. Programm wird beendet." << std::endl;
        return 1;
    }
        
    data_handler dh;
    dh.load_data(selected_path, selected_data);

            
    return 0;
} 