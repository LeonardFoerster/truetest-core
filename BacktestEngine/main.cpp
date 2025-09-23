#include "black_scholes.h"
#include "perform_calculations.h"

#include <iostream>


int main (int argc, char* argv[]) // default main method
{
    std::cout << "Own data(d) or CSV(c)?" << std::endl;
    char decision = 'x';
    std::cin >> decision;
    
    while (decision != 'd' && decision != 'c') 
    {
        std::cout << "Wrong Input try again" << std::endl;        
        std::cin >> decision;
    }

    switch (decision)
    {
        case 'd':
            run_manual_calc();
            break;
        case 'c':
            run_csv_calc();
            break;
    }

    return 0;
} 