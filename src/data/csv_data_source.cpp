#include "csv_data_source.h"
#include "data_handler.h"
#include <iostream>

CsvDataSource::CsvDataSource(std::filesystem::path path)
    : path_(std::move(path)) {}

bool CsvDataSource::load_data(std::shared_ptr<data_handler> handler) {
    try {
        handler->load_from_csv(path_);
        std::cout << "Loaded " << handler->db_data_symbol.size() << " records from CSV." << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "CSV load failed: " << e.what() << std::endl;
        return false;
    }
}
