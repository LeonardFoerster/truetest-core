#pragma once
#ifdef HAS_BITUNIX

// Bitunix futures REST/WS signing (double SHA-256).
// Official: digest = SHA256(nonce + timestamp + apiKey + sortedQuery + body)
//           sign   = SHA256(digestHex + secretKey)
// Both digests are lowercase hex. See:
//   https://www.bitunix.com/api-docs/futures/common/sign.html

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bitunix {

inline std::string sha256_hex(std::string_view data)
{
    unsigned char raw[32];
    unsigned int out_len = 0;
    if (EVP_Digest(data.data(), data.size(), raw, &out_len, EVP_sha256(), nullptr) != 1
        || out_len != 32)
    {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.resize(64);
    for (int i = 0; i < 32; ++i)
    {
        hex[static_cast<std::size_t>(i * 2)]     = kHex[(raw[i] >> 4) & 0xF];
        hex[static_cast<std::size_t>(i * 2 + 1)] = kHex[raw[i] & 0xF];
    }
    return hex;
}

// Sort query params by key (ASCII ascending) and concatenate as key+value
// with no separators (Bitunix convention: id1uid200).
inline std::string sorted_query_concat(
    const std::vector<std::pair<std::string, std::string>>& params)
{
    std::vector<std::pair<std::string, std::string>> sorted = params;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    for (const auto& [k, v] : sorted)
    {
        out.append(k);
        out.append(v);
    }
    return out;
}

// Convenience: parse "a=1&b=2" into sorted concat. Empty → "".
inline std::string sorted_query_from_string(std::string_view query)
{
    if (query.empty())
        return {};
    std::vector<std::pair<std::string, std::string>> params;
    std::size_t i = 0;
    while (i < query.size())
    {
        auto amp = query.find('&', i);
        if (amp == std::string_view::npos)
            amp = query.size();
        auto part = query.substr(i, amp - i);
        auto eq = part.find('=');
        if (eq != std::string_view::npos)
            params.emplace_back(std::string(part.substr(0, eq)),
                                std::string(part.substr(eq + 1)));
        else if (!part.empty())
            params.emplace_back(std::string(part), "");
        i = amp + 1;
    }
    return sorted_query_concat(params);
}

// First digest hex, then outer sign hex.
inline std::string sign_double_sha256(std::string_view nonce,
                                      std::string_view timestamp,
                                      std::string_view api_key,
                                      std::string_view sorted_query,
                                      std::string_view body,
                                      std::string_view secret)
{
    std::string first;
    first.reserve(nonce.size() + timestamp.size() + api_key.size()
                  + sorted_query.size() + body.size());
    first.append(nonce);
    first.append(timestamp);
    first.append(api_key);
    first.append(sorted_query);
    first.append(body);
    const std::string digest = sha256_hex(first);
    if (digest.empty())
        return {};
    std::string second;
    second.reserve(digest.size() + secret.size());
    second.append(digest);
    second.append(secret);
    return sha256_hex(second);
}

// Generate a simple 32-char hex nonce from a counter + timestamp (tests inject).
inline std::string make_nonce_hex(std::uint64_t n)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 31; i >= 0 && n > 0; --i)
    {
        out[static_cast<std::size_t>(i)] = kHex[n & 0xF];
        n >>= 4;
    }
    return out;
}

} // namespace bitunix

#endif // HAS_BITUNIX
