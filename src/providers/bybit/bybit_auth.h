#pragma once
#ifdef HAS_BYBIT

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/params.h>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace bybit {

inline void bytes_to_hex_lower(const unsigned char* in, std::size_t len, char* out)
{
    static constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < len; ++i)
    {
        out[(i << 1)]     = digits[(in[i] >> 4) & 0x0f];
        out[(i << 1) + 1] = digits[in[i]        & 0x0f];
    }
}

// Reusable HMAC-SHA256 signer producing lowercase hex (Bybit V5 REST + WS).
// Not thread-safe by itself; pair with an external mutex for shared use.
class HmacSha256HexSigner
{
public:
    explicit HmacSha256HexSigner(std::string_view key)
    {
        if (key.empty()) return;

        mac_ = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
        if (!mac_) return;
        ctx_ = EVP_MAC_CTX_new(mac_);
        if (!ctx_) return;

        char digest[] = "SHA256";
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string("digest", digest, 0),
            OSSL_PARAM_construct_end()
        };
        EVP_MAC_init(ctx_,
                     reinterpret_cast<const unsigned char*>(key.data()),
                     key.size(),
                     params);
    }

    ~HmacSha256HexSigner()
    {
        if (ctx_) EVP_MAC_CTX_free(ctx_);
        if (mac_) EVP_MAC_free(mac_);
    }

    HmacSha256HexSigner(const HmacSha256HexSigner&) = delete;
    HmacSha256HexSigner& operator=(const HmacSha256HexSigner&) = delete;

    // Returns lowercase hex(HMAC-SHA256(key, data)), or empty on failure.
    std::string sign(std::string_view data)
    {
        if (!ctx_) return {};
        unsigned char raw[32];
        // Null key + null params → reset state, keep the existing key.
        EVP_MAC_init(ctx_, nullptr, 0, nullptr);
        EVP_MAC_update(ctx_,
                       reinterpret_cast<const unsigned char*>(data.data()),
                       data.size());
        std::size_t out_len = 0;
        if (EVP_MAC_final(ctx_, raw, &out_len, sizeof(raw)) != 1 ||
            out_len != 32)
        {
            return {};
        }
        std::string out(out_len * 2, '\0');
        bytes_to_hex_lower(raw, out_len, out.data());
        return out;
    }

private:
    EVP_MAC*     mac_ = nullptr;
    EVP_MAC_CTX* ctx_ = nullptr;
};

// One-shot HMAC-SHA256 → lowercase hex (Bybit V5).
inline std::string hmac_sha256_hex(std::string_view secret,
                                   std::string_view data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(data.data()),
         data.size(),
         digest, &len);

    std::string out(static_cast<std::size_t>(len) * 2, '\0');
    bytes_to_hex_lower(digest, len, out.data());
    return out;
}

// Bybit REST prehash:
//   timestamp + api_key + recv_window + (queryString | jsonBody)
// timestamp and recv_window are decimal digit strings (ms).
// For GET: append query string (no leading '?').
// For POST: append raw JSON body.
// Empty query/body is valid (prehash ends after recv_window).
inline std::string build_rest_prehash(std::string_view timestamp,
                                      std::string_view api_key,
                                      std::string_view recv_window,
                                      std::string_view payload)
{
    std::string out;
    out.reserve(timestamp.size() + api_key.size()
                + recv_window.size() + payload.size());
    out.append(timestamp);
    out.append(api_key);
    out.append(recv_window);
    out.append(payload);
    return out;
}

// Sign REST request → value for X-BAPI-SIGN header.
inline std::string sign_rest(std::string_view secret,
                             std::string_view timestamp,
                             std::string_view api_key,
                             std::string_view recv_window,
                             std::string_view payload)
{
    return hmac_sha256_hex(
        secret,
        build_rest_prehash(timestamp, api_key, recv_window, payload));
}

// Private WS auth prehash: "GET/realtime" + expires (ms as decimal string).
// Signature is sent in the auth op args alongside api_key and expires.
inline std::string build_ws_auth_prehash(std::string_view expires_ms)
{
    std::string out;
    out.reserve(12 + expires_ms.size()); // "GET/realtime" == 12
    out.append("GET/realtime");
    out.append(expires_ms);
    return out;
}

inline std::string sign_ws_auth(std::string_view secret,
                                std::string_view expires_ms)
{
    return hmac_sha256_hex(secret, build_ws_auth_prehash(expires_ms));
}

// Default recv window used by Bybit samples (ms). Callers may override.
inline constexpr const char* k_default_recv_window = "5000";

inline std::int64_t server_time_ms()
{
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
}

inline bool ci_match_at(std::string_view s, std::size_t pos, std::string_view needle)
{
    if (pos + needle.size() > s.size()) return false;
    for (std::size_t i = 0; i < needle.size(); ++i)
    {
        const auto a = static_cast<unsigned char>(s[pos + i]);
        const auto b = static_cast<unsigned char>(needle[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

// Scrub secrets from log snippets. Covers Bybit header names, JSON fields,
// and query params. Mirrors binance::redact_for_log with venue keys added.
inline std::string redact_for_log(std::string_view input,
                                  std::size_t max_len = 160)
{
    std::string out(input.substr(0, max_len));
    const bool truncated = input.size() > max_len;

    auto redact_value = [&](std::size_t value_start) {
        if (value_start >= out.size()) return;
        std::size_t value_end = value_start;
        while (value_end < out.size() &&
               out[value_end] != '&' &&
               out[value_end] != '"' &&
               out[value_end] != '\'' &&
               out[value_end] != ',' &&
               out[value_end] != '}' &&
               !std::isspace(static_cast<unsigned char>(out[value_end])))
        {
            ++value_end;
        }
        out.replace(value_start, value_end - value_start, "<redacted>");
    };

    auto redact_json_field = [&](std::string_view key) {
        std::size_t pos = 0;
        while (pos < out.size())
        {
            const std::string needle = "\"" + std::string(key) + "\"";
            std::size_t key_pos = std::string::npos;
            for (std::size_t i = pos; i < out.size(); ++i)
            {
                if (ci_match_at(out, i, needle))
                {
                    key_pos = i;
                    break;
                }
            }
            if (key_pos == std::string::npos) break;

            auto colon = out.find(':', key_pos + needle.size());
            if (colon == std::string::npos) break;
            auto value = colon + 1;
            while (value < out.size() &&
                   std::isspace(static_cast<unsigned char>(out[value])))
                ++value;
            if (value < out.size() && out[value] == '"')
                redact_value(value + 1);
            else
                redact_value(value);
            pos = value + 1;
        }
    };

    auto redact_param = [&](std::string_view key) {
        std::size_t pos = 0;
        while (pos < out.size())
        {
            std::size_t key_pos = std::string::npos;
            for (std::size_t i = pos; i < out.size(); ++i)
            {
                if (ci_match_at(out, i, key))
                {
                    const auto before_ok =
                        i == 0 || out[i - 1] == '?' || out[i - 1] == '&' ||
                        out[i - 1] == ' ' || out[i - 1] == '"' ||
                        out[i - 1] == ':';
                    const auto after = i + key.size();
                    if (before_ok && after < out.size() && out[after] == '=')
                    {
                        key_pos = i;
                        break;
                    }
                }
            }
            if (key_pos == std::string::npos) break;
            redact_value(key_pos + key.size() + 1);
            pos = key_pos + key.size() + 1;
        }
    };

    // JSON / query keys that must never hit logs.
    static constexpr std::string_view keys[] = {
        "apiKey", "api_key", "apiSecret", "api_secret",
        "secret", "signature", "sign", "token",
        "Authorization", "X-BAPI-API-KEY", "X-BAPI-SIGN",
        "X-BAPI-TIMESTAMP", "orderLinkId",
    };
    for (auto key : keys)
    {
        redact_json_field(key);
        redact_param(key);
    }

    // Header-style "X-BAPI-SIGN: <value>" (space after colon).
    auto redact_header = [&](std::string_view header) {
        std::size_t pos = 0;
        while (pos < out.size())
        {
            std::size_t found = std::string::npos;
            for (std::size_t i = pos; i < out.size(); ++i)
            {
                if (ci_match_at(out, i, header))
                {
                    found = i;
                    break;
                }
            }
            if (found == std::string::npos) break;
            auto value = found + header.size();
            while (value < out.size() &&
                   (out[value] == ':' ||
                    std::isspace(static_cast<unsigned char>(out[value]))))
                ++value;
            redact_value(value);
            pos = value + 1;
        }
    };
    redact_header("X-BAPI-API-KEY");
    redact_header("X-BAPI-SIGN");
    redact_header("X-BAPI-TIMESTAMP");

    std::size_t bearer = 0;
    while (bearer < out.size())
    {
        std::size_t found = std::string::npos;
        for (std::size_t i = bearer; i < out.size(); ++i)
        {
            if (ci_match_at(out, i, "Bearer "))
            {
                found = i;
                break;
            }
        }
        if (found == std::string::npos) break;
        redact_value(found + 7);
        bearer = found + 7;
    }

    if (truncated) out += "...";
    return out;
}

} // namespace bybit

#endif // HAS_BYBIT
