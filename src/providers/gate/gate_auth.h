#pragma once
#ifdef HAS_GATE

#include <openssl/evp.h>
#include <openssl/params.h>

#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace gate {

// Lower-case hex encode of `len` raw bytes into `out` (must hold 2*len).
inline void bytes_to_hex_lower(const unsigned char* in,
                               std::size_t len,
                               char* out)
{
    static constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < len; ++i)
    {
        out[(i << 1)]     = digits[(in[i] >> 4) & 0x0f];
        out[(i << 1) + 1] = digits[in[i]        & 0x0f];
    }
}

// SHA-512 hex digest of `data` (128 lowercase hex chars). Empty on failure.
// Empty body known digest (Gate REST empty payload hash):
//   cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce
//   47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e
inline std::string sha512_hex(std::string_view data)
{
    unsigned char dig[EVP_MAX_MD_SIZE];
    unsigned int dig_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    if (EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr) != 1
        || EVP_DigestUpdate(ctx, data.data(), data.size()) != 1
        || EVP_DigestFinal_ex(ctx, dig, &dig_len) != 1
        || dig_len != 64)
    {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);

    std::string out(static_cast<std::size_t>(dig_len) * 2, '\0');
    bytes_to_hex_lower(dig, dig_len, out.data());
    return out;
}

// Reusable HMAC-SHA512 signer → lowercase hex (128 chars).
// Gate REST SIGN and private WS auth both use this. Not thread-safe by
// itself; pair with an external mutex for shared use.
class HmacSha512HexSigner
{
public:
    explicit HmacSha512HexSigner(std::string_view key)
    {
        if (key.empty()) return;

        mac_ = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
        if (!mac_) return;
        ctx_ = EVP_MAC_CTX_new(mac_);
        if (!ctx_) return;

        char digest[] = "SHA512";
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string("digest", digest, 0),
            OSSL_PARAM_construct_end()
        };
        EVP_MAC_init(ctx_,
                     reinterpret_cast<const unsigned char*>(key.data()),
                     key.size(),
                     params);
    }

    ~HmacSha512HexSigner()
    {
        if (ctx_) EVP_MAC_CTX_free(ctx_);
        if (mac_) EVP_MAC_free(mac_);
    }

    HmacSha512HexSigner(const HmacSha512HexSigner&) = delete;
    HmacSha512HexSigner& operator=(const HmacSha512HexSigner&) = delete;

    // Returns hex(HMAC-SHA512(key, data)), or empty on failure.
    std::string sign(std::string_view data)
    {
        if (!ctx_) return {};
        unsigned char raw[64];
        // Null key + null params → reset state, keep the existing key.
        EVP_MAC_init(ctx_, nullptr, 0, nullptr);
        EVP_MAC_update(ctx_,
                       reinterpret_cast<const unsigned char*>(data.data()),
                       data.size());
        std::size_t out_len = 0;
        if (EVP_MAC_final(ctx_, raw, &out_len, sizeof(raw)) != 1
            || out_len != 64)
        {
            return {};
        }
        std::string hex(128, '\0');
        bytes_to_hex_lower(raw, 64, hex.data());
        return hex;
    }

private:
    EVP_MAC*     mac_ = nullptr;
    EVP_MAC_CTX* ctx_ = nullptr;
};

// Gate REST signature string (exact newlines, no trailing newline after ts):
//   METHOD\n
//   URL_PATH\n
//   QUERY_STRING\n
//   HEX(SHA512(body))\n
//   TIMESTAMP
//
// METHOD uppercase; URL_PATH includes /api/v4; QUERY without leading '?';
// empty body → SHA512("") known digest; TIMESTAMP is Unix **seconds**.
inline std::string build_rest_sign_string(std::string_view method,
                                          std::string_view path,
                                          std::string_view query,
                                          std::string_view body,
                                          std::string_view timestamp_s)
{
    const std::string body_hash = sha512_hex(body);
    std::string out;
    out.reserve(method.size() + path.size() + query.size()
                + body_hash.size() + timestamp_s.size() + 4);
    out.append(method);
    out.push_back('\n');
    out.append(path);
    out.push_back('\n');
    out.append(query);
    out.push_back('\n');
    out.append(body_hash);
    out.push_back('\n');
    out.append(timestamp_s);
    return out;
}

// Full REST SIGN header value (HMAC-SHA512 hex of the sign string).
inline std::string sign_rest(std::string_view secret,
                             std::string_view method,
                             std::string_view path,
                             std::string_view query,
                             std::string_view body,
                             std::string_view timestamp_s)
{
    HmacSha512HexSigner signer(secret);
    return signer.sign(
        build_rest_sign_string(method, path, query, body, timestamp_s));
}

// Private WS auth prehash:
//   channel=<channel>&event=<event>&time=<time>
// HMAC-SHA512(secret, prehash) → hex SIGN.
inline std::string build_ws_sign_string(std::string_view channel,
                                        std::string_view event,
                                        std::string_view time_s)
{
    std::string out;
    out.reserve(8 + channel.size() + 7 + event.size() + 6 + time_s.size());
    out.append("channel=");
    out.append(channel);
    out.append("&event=");
    out.append(event);
    out.append("&time=");
    out.append(time_s);
    return out;
}

inline std::string sign_ws(std::string_view secret,
                           std::string_view channel,
                           std::string_view event,
                           std::string_view time_s)
{
    HmacSha512HexSigner signer(secret);
    return signer.sign(build_ws_sign_string(channel, event, time_s));
}

// Cold-path log redaction for Gate headers / bodies (KEY, SIGN, secrets).
// Mirrors binance::redact_for_log shape; not for hot path.
inline bool ci_match_at(std::string_view s,
                        std::size_t pos,
                        std::string_view needle)
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

inline std::string redact_for_log(std::string_view input,
                                  std::size_t max_len = 160)
{
    std::string out(input.substr(0, max_len));
    const bool truncated = input.size() > max_len;

    auto redact_value = [&](std::size_t value_start) {
        if (value_start >= out.size()) return;
        std::size_t value_end = value_start;
        while (value_end < out.size()
               && out[value_end] != '&'
               && out[value_end] != '"'
               && out[value_end] != '\''
               && out[value_end] != ','
               && out[value_end] != '}'
               && !std::isspace(static_cast<unsigned char>(out[value_end])))
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
            while (value < out.size()
                   && std::isspace(static_cast<unsigned char>(out[value])))
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
                        i == 0 || out[i - 1] == '?' || out[i - 1] == '&'
                        || out[i - 1] == ' ' || out[i - 1] == '"'
                        || out[i - 1] == '\n';
                    const auto after = i + key.size();
                    if (before_ok && after < out.size()
                        && (out[after] == '=' || out[after] == ':'))
                    {
                        key_pos = i;
                        break;
                    }
                }
            }
            if (key_pos == std::string::npos) break;
            // Skip KEY: / SIGN: header style (colon) or query (=).
            const auto after = key_pos + key.size();
            if (after < out.size() && out[after] == ':')
            {
                auto value = after + 1;
                while (value < out.size()
                       && std::isspace(
                           static_cast<unsigned char>(out[value])))
                    ++value;
                redact_value(value);
                pos = value + 1;
            }
            else
            {
                redact_value(after + 1);
                pos = after + 1;
            }
        }
    };

    static constexpr std::string_view keys[] = {
        "KEY", "SIGN", "apiKey", "api_key", "apiSecret", "api_secret",
        "secret", "signature", "token", "Authorization", "passphrase"
    };
    for (auto key : keys)
    {
        redact_json_field(key);
        redact_param(key);
    }

    if (truncated) out += "...";
    return out;
}

} // namespace gate

#endif // HAS_GATE
