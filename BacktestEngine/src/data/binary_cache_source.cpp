#include "binary_cache_source.h"
#include "data_handler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <vector>

BinaryCacheSource::BinaryCacheSource(std::unique_ptr<IDataSource> fallback, std::string cache_path)
    : fallback_(std::move(fallback)), cache_path_(std::move(cache_path)) {}

bool BinaryCacheSource::load_data(std::shared_ptr<data_handler> handler) {
    if (std::filesystem::exists(cache_path_)) {
        std::cout << "Loading from cache..." << std::endl;
        std::ifstream ifs(cache_path_, std::ios::binary);

        // Read and validate header
        BinaryCacheHeader hdr{};
        ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!ifs || hdr.magic[0] != 'T' || hdr.magic[1] != 'T' ||
            hdr.magic[2] != 'B' || hdr.magic[3] != 'C') {
            std::cerr << "Cache file has invalid magic — deleting stale cache.\n";
            ifs.close();
            std::filesystem::remove(cache_path_);
            // Fall through to fallback below
        } else if (hdr.version != 1) {
            std::cerr << "Cache file version " << hdr.version
                      << " is not supported (expected 1) — deleting stale cache.\n";
            ifs.close();
            std::filesystem::remove(cache_path_);
        } else {
            // Read payload into memory for CRC check
            std::ostringstream payload_buf;
            payload_buf << ifs.rdbuf();
            std::string payload_str = payload_buf.str();

            uint64_t computed_crc = crc64_update(0, payload_str.data(), payload_str.size());
            if (computed_crc != hdr.crc64) {
                std::cerr << "Cache file checksum mismatch — deleting corrupt cache.\n";
                ifs.close();
                std::filesystem::remove(cache_path_);
            } else {
                // Parse the payload
                const char* p = payload_str.data();
                size_t remaining = payload_str.size();

                auto read_raw = [&](void* dst, size_t n) {
                    if (remaining < n) throw std::runtime_error("truncated cache");
                    std::memcpy(dst, p, n);
                    p += n;
                    remaining -= n;
                };

                size_t size;
                read_raw(&size, sizeof(size));

                handler->db_data_symbol.resize(size);
                for (auto& s : handler->db_data_symbol) {
                    size_t len;
                    read_raw(&len, sizeof(len));
                    s.resize(len);
                    read_raw(&s[0], len);
                }

                handler->db_data_date.resize(size);
                for (auto& s : handler->db_data_date) {
                    size_t len;
                    read_raw(&len, sizeof(len));
                    s.resize(len);
                    read_raw(&s[0], len);
                }

                handler->db_data_open_value.resize(size);
                read_raw(handler->db_data_open_value.data(), size * sizeof(double));

                handler->db_data_high_value.resize(size);
                read_raw(handler->db_data_high_value.data(), size * sizeof(double));

                handler->db_data_low_value.resize(size);
                read_raw(handler->db_data_low_value.data(), size * sizeof(double));

                handler->db_data_close_value.resize(size);
                read_raw(handler->db_data_close_value.data(), size * sizeof(double));

                handler->db_data_volume_value.resize(size);
                read_raw(handler->db_data_volume_value.data(), size * sizeof(int64_t));

                std::cout << "Loaded " << size << " records from cache." << std::endl;
                return true;
            }
        }
    }

    if (!fallback_) {
        std::cerr << "No cache found and no fallback source." << std::endl;
        return false;
    }

    if (!fallback_->load_data(handler)) {
        return false;
    }

    // Build payload in memory so we can compute CRC before writing
    std::cout << "Saving to cache..." << std::endl;
    std::ostringstream payload_buf(std::ios::binary);
    size_t size = handler->db_data_symbol.size();
    payload_buf.write(reinterpret_cast<const char*>(&size), sizeof(size));

    for (const auto& s : handler->db_data_symbol) {
        size_t len = s.size();
        payload_buf.write(reinterpret_cast<const char*>(&len), sizeof(len));
        payload_buf.write(s.data(), len);
    }

    for (const auto& s : handler->db_data_date) {
        size_t len = s.size();
        payload_buf.write(reinterpret_cast<const char*>(&len), sizeof(len));
        payload_buf.write(s.data(), len);
    }

    payload_buf.write(reinterpret_cast<const char*>(handler->db_data_open_value.data()), size * sizeof(double));
    payload_buf.write(reinterpret_cast<const char*>(handler->db_data_high_value.data()), size * sizeof(double));
    payload_buf.write(reinterpret_cast<const char*>(handler->db_data_low_value.data()), size * sizeof(double));
    payload_buf.write(reinterpret_cast<const char*>(handler->db_data_close_value.data()), size * sizeof(double));
    payload_buf.write(reinterpret_cast<const char*>(handler->db_data_volume_value.data()), size * sizeof(int64_t));

    std::string payload_str = payload_buf.str();

    // Write header + payload
    BinaryCacheHeader hdr{};
    hdr.crc64 = crc64_update(0, payload_str.data(), payload_str.size());

    std::ofstream ofs(cache_path_, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    ofs.write(payload_str.data(), static_cast<std::streamsize>(payload_str.size()));

    std::cout << "Cache saved." << std::endl;
    return true;
}
