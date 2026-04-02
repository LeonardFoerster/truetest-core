#pragma once
#include "data_source.h"
#include <cstdint>
#include <string>
#include <memory>

// Binary cache file header (16 bytes):
//   [0..3]  magic:    "TTBC"
//   [4..5]  version:  uint16_t (currently 1)
//   [6..7]  reserved: 0
//   [8..15] crc64:    CRC-64/ECMA of the payload after the header
struct BinaryCacheHeader {
    char     magic[4]  = {'T','T','B','C'};
    uint16_t version   = 1;
    uint16_t reserved  = 0;
    uint64_t crc64     = 0;
};
static_assert(sizeof(BinaryCacheHeader) == 16, "header must be 16 bytes");

// CRC-64/ECMA-182
inline uint64_t crc64_update(uint64_t crc, const void* data, size_t len)
{
    static const uint64_t poly = 0x42F0E1EBA9EA3693ULL;
    static bool table_init = false;
    static uint64_t table[256];
    if (!table_init) {
        for (int i = 0; i < 256; ++i) {
            uint64_t c = static_cast<uint64_t>(i) << 56;
            for (int j = 0; j < 8; ++j)
                c = (c & (1ULL << 63)) ? (c << 1) ^ poly : (c << 1);
            table[i] = c;
        }
        table_init = true;
    }
    auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
        crc = table[((crc >> 56) ^ p[i]) & 0xFF] ^ (crc << 8);
    return crc;
}

class BinaryCacheSource : public IDataSource {
    std::unique_ptr<IDataSource> fallback_;
    std::string cache_path_;
public:
    BinaryCacheSource(std::unique_ptr<IDataSource> fallback, std::string cache_path);
    bool load_data(std::shared_ptr<data_handler> handler) override;
};
