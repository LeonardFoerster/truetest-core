#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace truetest::reproducibility {

using Sha256Digest = std::array<std::byte, 32>;

class Sha256 final
{
public:
    Sha256() noexcept;

    void reset() noexcept;
    void update(std::span<const std::byte> bytes);
    void update(std::string_view text);

    [[nodiscard]] Sha256Digest digest() const;

private:
    void process_block(const std::byte* block) noexcept;
    [[nodiscard]] Sha256Digest finalize_in_place();

    std::array<std::uint32_t, 8> state_{};
    std::array<std::byte, 64> buffer_{};
    std::uint64_t total_bytes_{0};
    std::size_t buffered_{0};
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> bytes);
[[nodiscard]] Sha256Digest sha256(std::string_view text);
[[nodiscard]] Sha256Digest sha256_file(const std::filesystem::path& path);
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);
[[nodiscard]] std::string sha256_hex(std::string_view text);
[[nodiscard]] std::string sha256_file_hex(const std::filesystem::path& path);

} // namespace truetest::reproducibility
