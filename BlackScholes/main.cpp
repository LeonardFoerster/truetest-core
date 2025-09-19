#include "black_scholes.h"
#include "perform_calculations.h"

#include <iostream>


int main (int argc, char* argv[]) // default main method
{
    std::cout << "Eigene Daten(e) oder CSV(c)?" << std::endl;
    char decision = 'x';
    std::cin >> decision;
    
    while (decision != 'e' && decision != 'c') // Problem, wenn di erste Eingabe korrekt ist, wird gar nix ausgeführt
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