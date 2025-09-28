#include "data_handler.h"

#include <sstream>
#include <iostream>
#include <fstream>
#include <chrono>
#include <map>
#include <limits> 
#include <stdexcept> 


data_handler::data_handler(const std::filesystem::path& file_path)
    : data_path_(file_path)
{
}

void data_handler::load_data(const std::filesystem::path& data_path)
{
    std::ifstream iff(data_path);
    std::ofstream off("C:\\Users\\Leonard\\Desktop\\test.txt");

    if (!iff.good()) 
    {
        std::cerr << "Error opening input file: " << data_path << "\n";
        return;
    }

    std::string line;
    const char seperator = ','; 
    auto start = std::chrono::high_resolution_clock::now();
    int line_count = 1;

    while (std::getline(iff, line))
    {
        if (line.empty())
        {
            continue;
        }

        market_data_bar bar;
        std::stringstream ss(line);
        std::string line;
        bool parse_valid = true;

        try 
        {
            
            if (!std::getline(ss, line, seperator)) throw std::runtime_error("Missing 'open'");
            bar.open = std::stod(line);

            if (!std::getline(ss, line, seperator)) throw std::runtime_error("Missing 'high'");
            bar.high = std::stod(line);

            if (!std::getline(ss, line, seperator)) throw std::runtime_error("Missing 'low'");
            bar.low = std::stod(line);

            if (!std::getline(ss, line)) throw std::runtime_error("Missing 'close'");
            bar.close = std::stod(line);

            line_data_[line_count] = bar;
            line_count++;
            off << bar.open << seperator << bar.high << seperator << bar.low << seperator << bar.close << std::endl;

        }
        catch (const std::exception &e) 
        {
            std::cerr << "Error on lin " << line_count << ": " << e.what() << " in line: " << line << "\n";
            line_count++;
            continue; 
        }
    }

    
    if (std::filesystem::file_size("C:\\Users\\Leonard\\Desktop\\test.txt") != 0)
    {
        std::cerr << "File successfully written" << "\n";
    }
    else
    {
        std::cerr << "Error writing File\n";
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "Laufzeit: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << " ms\n";
}

void data_handler::get_next_bar() const
{
    

    return;
}