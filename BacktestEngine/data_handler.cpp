#include "data_handler.h"
#include "strategy.h"
#include "black_scholes.h"

#include <sstream>
#include <iostream>
#include <fstream>
#include <chrono>
#include <map>
#include <limits> 
#include <stdexcept> 


data_handler::data_handler() {};
   

int data_handler::load_bs_data(const std::filesystem::path& data_path)
{
    std::filesystem::path o_path = "C:\\Users\\Leonard\\Desktop\\bs_data.txt";
    std::ifstream iff(data_path);
    std::ofstream off(o_path);
    

    if (!iff.good() || !off.good())
    {
        std::cerr << "Error opening file" << std::endl;
        return 1;
    }
   
    std::string header_line;
    std::getline(iff, header_line);
    std::string line;
    size_t bs_linecount = 1;
    auto start = std::chrono::high_resolution_clock::now();

    while (std::getline(iff, line))
    {
        std::stringstream ss(line);
        std::string value_str;
        std::vector<double> values;

        while (std::getline(ss, value_str, ','))
        {
            try
            {
                values.push_back(std::stod(value_str));
            }
            catch (const std::exception& e)
            {
                std::cerr << "Line: " << line << "contains wrong format" << std::endl;
                values.clear(); 
                break;
            }
        }

        if (values.size() == 6)
        {
            black_scholes option_calculator(values[0], values[1], values[2], values[3], values[4], values[5]);
            double fair_price = option_calculator.get_call_price();
            bs_line_data_[bs_linecount] = fair_price;
            off << fair_price << "\n";
        }
    }

    iff.close();
    off.close();

    auto end = std::chrono::high_resolution_clock::now();
    auto run_time_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "BS data written to: " << o_path << std::endl;
    std::cout << "Calculation time: " << run_time_duration_ms.count() << " ms" << std::endl;

}


void data_handler::load_olhc_data(const std::filesystem::path& data_path)
{
    std::ifstream iff(data_path);
    std::ofstream off("C:\\Users\\Leonard\\Desktop\\olhc_test.txt");

    if (!iff.good()) 
    {
        std::cerr << "Error opening input file: " << data_path << "\n";
        return;
    }
      
    std::string line;
    const char seperator = ','; 
    auto start = std::chrono::high_resolution_clock::now();
    size_t olhc_line_count = 1;

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

            ohlc_line_data_[olhc_line_count] = bar;
            olhc_line_count++;
            off << bar.open << seperator << bar.high << seperator << bar.low << seperator << bar.close << std::endl;

        }
        catch (const std::exception &e) 
        {
            std::cerr << "Error on line" << olhc_line_count << ": " << e.what() << " in line: " << line << "\n";
            olhc_line_count++;
            continue; 
        }
    }

    auto end = std::chrono::high_resolution_clock::now();

    if (std::filesystem::file_size("C:\\Users\\Leonard\\Desktop\\test.txt") != 0)
    {
        std::cerr << "File successfully written" << "\n";
        std::cout << "calculation time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << " ms\n";
    }
    else
    {
        std::cerr << "Error writing File\n";
    }
    
    
}

void data_handler::load_data(std::filesystem::path data_path_, data_type_content type)
{
    switch (type)
    {
        case(data_type_content::black_scholes_data):
            load_bs_data(data_path_);
                break;
        case(data_type_content::ohlc_data):
            load_olhc_data(data_path_);
                break;
        
    }
}