#include "sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace truetest::reproducibility {

namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] constexpr std::uint32_t choose(
    std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
{
    return (x & y) ^ (~x & z);
}

[[nodiscard]] constexpr std::uint32_t majority(
    std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
{
    return (x & y) ^ (x & z) ^ (y & z);
}

[[nodiscard]] constexpr std::uint32_t big_sigma0(std::uint32_t value) noexcept
{
    return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22);
}

[[nodiscard]] constexpr std::uint32_t big_sigma1(std::uint32_t value) noexcept
{
    return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25);
}

[[nodiscard]] constexpr std::uint32_t small_sigma0(std::uint32_t value) noexcept
{
    return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3U);
}

[[nodiscard]] constexpr std::uint32_t small_sigma1(std::uint32_t value) noexcept
{
    return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10U);
}

} // namespace

Sha256::Sha256() noexcept
{
    reset();
}

void Sha256::reset() noexcept
{
    state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    buffer_.fill(std::byte{0});
    total_bytes_ = 0;
    buffered_ = 0;
}

void Sha256::update(std::span<const std::byte> bytes)
{
    if (bytes.size() > (std::numeric_limits<std::uint64_t>::max() - total_bytes_))
        throw std::length_error("SHA-256 input exceeds the supported length");
    total_bytes_ += static_cast<std::uint64_t>(bytes.size());

    std::size_t offset = 0;
    if (buffered_ != 0)
    {
        const std::size_t count = std::min(buffer_.size() - buffered_, bytes.size());
        std::copy_n(bytes.begin(), count, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += count;
        offset += count;
        if (buffered_ == buffer_.size())
        {
            process_block(buffer_.data());
            buffered_ = 0;
        }
    }

    while (bytes.size() - offset >= buffer_.size())
    {
        process_block(bytes.data() + static_cast<std::ptrdiff_t>(offset));
        offset += buffer_.size();
    }

    const std::size_t remaining = bytes.size() - offset;
    if (remaining != 0)
    {
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), remaining,
                    buffer_.begin());
        buffered_ = remaining;
    }
}

void Sha256::update(std::string_view text)
{
    update(std::as_bytes(std::span(text.data(), text.size())));
}

Sha256Digest Sha256::digest() const
{
    Sha256 copy = *this;
    return copy.finalize_in_place();
}

void Sha256::process_block(const std::byte* block) noexcept
{
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16; ++i)
    {
        const std::size_t offset = i * 4;
        schedule[i] = (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset])) << 24U)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 1])) << 16U)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 2])) << 8U)
            | static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 3]));
    }
    for (std::size_t i = 16; i < schedule.size(); ++i)
    {
        schedule[i] = small_sigma1(schedule[i - 2]) + schedule[i - 7]
            + small_sigma0(schedule[i - 15]) + schedule[i - 16];
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < schedule.size(); ++i)
    {
        const std::uint32_t temp1 = h + big_sigma1(e) + choose(e, f, g)
            + round_constants[i] + schedule[i];
        const std::uint32_t temp2 = big_sigma0(a) + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

Sha256Digest Sha256::finalize_in_place()
{
    if (total_bytes_ > std::numeric_limits<std::uint64_t>::max() / 8U)
        throw std::length_error("SHA-256 bit length exceeds the supported range");
    const std::uint64_t bit_length = total_bytes_ * 8U;

    buffer_[buffered_++] = std::byte{0x80};
    if (buffered_ > 56)
    {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                  buffer_.end(), std::byte{0});
        process_block(buffer_.data());
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
              buffer_.begin() + 56, std::byte{0});
    for (std::size_t i = 0; i < 8; ++i)
    {
        const unsigned shift = static_cast<unsigned>((7U - i) * 8U);
        buffer_[56 + i] = static_cast<std::byte>((bit_length >> shift) & 0xffU);
    }
    process_block(buffer_.data());

    Sha256Digest result{};
    for (std::size_t i = 0; i < state_.size(); ++i)
    {
        const std::uint32_t word = state_[i];
        result[i * 4] = static_cast<std::byte>((word >> 24U) & 0xffU);
        result[i * 4 + 1] = static_cast<std::byte>((word >> 16U) & 0xffU);
        result[i * 4 + 2] = static_cast<std::byte>((word >> 8U) & 0xffU);
        result[i * 4 + 3] = static_cast<std::byte>(word & 0xffU);
    }
    return result;
}

Sha256Digest sha256(std::span<const std::byte> bytes)
{
    Sha256 hasher;
    hasher.update(bytes);
    return hasher.digest();
}

Sha256Digest sha256(std::string_view text)
{
    Sha256 hasher;
    hasher.update(text);
    return hasher.digest();
}

Sha256Digest sha256_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open SHA-256 input: " + path.string());

    Sha256 hasher;
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
        {
            hasher.update(std::as_bytes(std::span(
                buffer.data(), static_cast<std::size_t>(count))));
        }
    }
    if (!input.eof())
        throw std::runtime_error("failed while hashing input: " + path.string());
    return hasher.digest();
}

std::string sha256_hex(const Sha256Digest& digest)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i)
    {
        const std::uint8_t byte = std::to_integer<std::uint8_t>(digest[i]);
        result[i * 2] = alphabet[byte >> 4U];
        result[i * 2 + 1] = alphabet[byte & 0x0fU];
    }
    return result;
}

std::string sha256_hex(std::string_view text)
{
    return sha256_hex(sha256(text));
}

std::string sha256_file_hex(const std::filesystem::path& path)
{
    return sha256_hex(sha256_file(path));
}

} // namespace truetest::reproducibility
