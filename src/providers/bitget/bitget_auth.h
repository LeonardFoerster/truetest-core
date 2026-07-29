#pragma once
#ifdef HAS_BITGET

#include <openssl/evp.h>
#include <openssl/params.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace bitget {

// Base64-encode raw bytes (no newlines). OpenSSL EVP_EncodeBlock.
inline std::string base64_encode(const unsigned char* data, std::size_t len)
{
    // Encoded length is 4 * ceil(n/3), plus a NUL from EVP_EncodeBlock.
    const std::size_t out_cap = 4 * ((len + 2) / 3);
    std::string out(out_cap, '\0');
    const int n = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()),
        data,
        static_cast<int>(len));
    if (n < 0)
    {
        out.clear();
        return out;
    }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

// Reusable HMAC-SHA256 signer producing Base64 (Bitget UTA v3).
// Not thread-safe by itself; pair with an external mutex for shared use.
class HmacSha256Base64Signer
{
public:
    explicit HmacSha256Base64Signer(std::string_view key)
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

    ~HmacSha256Base64Signer()
    {
        if (ctx_) EVP_MAC_CTX_free(ctx_);
        if (mac_) EVP_MAC_free(mac_);
    }

    HmacSha256Base64Signer(const HmacSha256Base64Signer&) = delete;
    HmacSha256Base64Signer& operator=(const HmacSha256Base64Signer&) = delete;

    // Returns Base64(HMAC-SHA256(key, data)), or empty on failure.
    std::string sign(std::string_view data)
    {
        if (!ctx_) return {};
        unsigned char raw[32];
        // Null key + null params -> reset state, keep the existing key.
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
        return base64_encode(raw, out_len);
    }

private:
    EVP_MAC*     mac_ = nullptr;
    EVP_MAC_CTX* ctx_ = nullptr;
};

// Bitget REST prehash:
//   timestamp + METHOD + requestPath + ["?" + queryString] + body
inline std::string build_prehash(std::string_view ts,
                                 std::string_view method,
                                 std::string_view path,
                                 std::string_view query,
                                 std::string_view body)
{
    std::string out;
    out.reserve(ts.size() + method.size() + path.size()
                + (query.empty() ? 0 : 1 + query.size())
                + body.size());
    out.append(ts);
    out.append(method);
    out.append(path);
    if (!query.empty())
    {
        out.push_back('?');
        out.append(query);
    }
    out.append(body);
    return out;
}

inline std::string sign_rest(std::string_view secret,
                             std::string_view ts,
                             std::string_view method,
                             std::string_view path,
                             std::string_view query,
                             std::string_view body)
{
    HmacSha256Base64Signer signer(secret);
    return signer.sign(build_prehash(ts, method, path, query, body));
}

// WS login prehash is fixed-shape: ts + "GET" + "/user/verify"
inline std::string sign_ws_login(std::string_view secret, std::string_view ts)
{
    return sign_rest(secret, ts, "GET", "/user/verify", "", "");
}

} // namespace bitget

#endif // HAS_BITGET
