#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>

#include "../../core/backtest_core.h"
#include "../../data/db_connection.h"



bool mkdir()
{


    std::error_code ec;
    bool created = std::filesystem::create_directory("Log", ec);

    if (ec) 
    {
        std::cerr << "Error while creating directory: " << ec.message() << '\n';
        return false;  
    }

    if (created) std::cout << "Directory created successfully\n";
    else std::cout << "Directory already exists\n";
    
    return true;  
}


void log_event(bool s)
{
        (void)s;
        mkdir();





    
}
