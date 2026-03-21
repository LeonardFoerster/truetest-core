#include "binary_cache_source.h"
#include "data_handler.h"
#include <filesystem>
#include <fstream>
#include <iostream>

BinaryCacheSource::BinaryCacheSource(std::unique_ptr<IDataSource> fallback, std::string cache_path)
    : fallback_(std::move(fallback)), cache_path_(std::move(cache_path)) {}

bool BinaryCacheSource::load_data(std::shared_ptr<data_handler> handler) {
    if (std::filesystem::exists(cache_path_)) {
        std::cout << "Loading from cache..." << std::endl;
        std::ifstream ifs(cache_path_, std::ios::binary);
        size_t size;
        ifs.read(reinterpret_cast<char*>(&size), sizeof(size));

        handler->db_data_symbol.resize(size);
        for (auto& s : handler->db_data_symbol) {
            size_t len;
            ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
            s.resize(len);
            ifs.read(&s[0], len);
        }

        handler->db_data_open_value.resize(size);
        ifs.read(reinterpret_cast<char*>(handler->db_data_open_value.data()), size * sizeof(double));

        handler->db_data_high_value.resize(size);
        ifs.read(reinterpret_cast<char*>(handler->db_data_high_value.data()), size * sizeof(double));

        handler->db_data_low_value.resize(size);
        ifs.read(reinterpret_cast<char*>(handler->db_data_low_value.data()), size * sizeof(double));

        handler->db_data_close_value.resize(size);
        ifs.read(reinterpret_cast<char*>(handler->db_data_close_value.data()), size * sizeof(double));

        handler->db_data_volume_value.resize(size);
        ifs.read(reinterpret_cast<char*>(handler->db_data_volume_value.data()), size * sizeof(int64_t));

        std::cout << "Loaded " << size << " records from cache." << std::endl;
        return true;
    }

    if (!fallback_) {
        std::cerr << "No cache found and no fallback source." << std::endl;
        return false;
    }

    if (!fallback_->load_data(handler)) {
        return false;
    }

    std::cout << "Saving to cache..." << std::endl;
    std::ofstream ofs(cache_path_, std::ios::binary);
    size_t size = handler->db_data_symbol.size();
    ofs.write(reinterpret_cast<const char*>(&size), sizeof(size));

    for (const auto& s : handler->db_data_symbol) {
        size_t len = s.size();
        ofs.write(reinterpret_cast<const char*>(&len), sizeof(len));
        ofs.write(s.data(), len);
    }

    ofs.write(reinterpret_cast<const char*>(handler->db_data_open_value.data()), size * sizeof(double));
    ofs.write(reinterpret_cast<const char*>(handler->db_data_high_value.data()), size * sizeof(double));
    ofs.write(reinterpret_cast<const char*>(handler->db_data_low_value.data()), size * sizeof(double));
    ofs.write(reinterpret_cast<const char*>(handler->db_data_close_value.data()), size * sizeof(double));
    ofs.write(reinterpret_cast<const char*>(handler->db_data_volume_value.data()), size * sizeof(int64_t));

    std::cout << "Cache saved." << std::endl;
    return true;
}
