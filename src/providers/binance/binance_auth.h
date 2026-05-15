#pragma once
#ifdef HAS_BINANCE

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/params.h>
#include <openssl/sha.h>

#include <cstddef>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace binance {

inline void bytes_to_hex_lower(const unsigned char* in, std::size_t len, char* out)
{
    static constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < len; ++i)
    {
        out[(i << 1)]     = digits[(in[i] >> 4) & 0x0f];
        out[(i << 1) + 1] = digits[in[i]        & 0x0f];
    }
}

// Reusable HMAC-SHA256 signer holding a keyed EVP_MAC_CTX. Reusing the
// keyed context skips per-call key-schedule setup (~200 ns at our sizes)
// and matches the cost of native SHA-NI when -march=native is on. Not
// thread-safe by itself; pair with an external mutex for shared use.
class HmacSha256Signer
{
public:
    explicit HmacSha256Signer(std::string_view key)
    {
        // Empty key = no usable signer. Tests construct BinanceRestClient
        // with empty credentials to exercise non-network helpers; sign()
        // will simply return false and callers handle it.
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

    ~HmacSha256Signer()
    {
        if (ctx_) EVP_MAC_CTX_free(ctx_);
        if (mac_) EVP_MAC_free(mac_);
    }

    HmacSha256Signer(const HmacSha256Signer&) = delete;
    HmacSha256Signer& operator=(const HmacSha256Signer&) = delete;

    // Writes 32 raw digest bytes into out[]. Returns false if the context
    // failed to allocate or finalize.
    bool sign(std::string_view data, unsigned char out[32])
    {
        if (!ctx_) return false;
        // Null key + null params → reset state, keep the existing key.
        EVP_MAC_init(ctx_, nullptr, 0, nullptr);
        EVP_MAC_update(ctx_,
                       reinterpret_cast<const unsigned char*>(data.data()),
                       data.size());
        std::size_t out_len = 0;
        return EVP_MAC_final(ctx_, out, &out_len, 32) == 1 && out_len == 32;
    }

private:
    EVP_MAC*     mac_ = nullptr;
    EVP_MAC_CTX* ctx_ = nullptr;
};

inline std::string hmac_sha256(const std::string& key, const std::string& data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    HMAC(EVP_sha256(),
         key.c_str(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.c_str()),
         data.size(),
         digest, &len);

    std::string out(len * 2, '\0');
    bytes_to_hex_lower(digest, len, out.data());
    return out;
}

inline std::string sign_query(const std::string& query_string, const std::string& secret)
{
    auto sig = hmac_sha256(secret, query_string);
    return query_string + "&signature=" + sig;
}

inline int64_t server_time_ms()
{
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

inline std::string url_encode(const std::string& value)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (char c : value)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            ss << c;
        }
        else
        {
            ss << '%' << std::setw(2)
               << static_cast<int>(static_cast<unsigned char>(c));
        }
    }
    return ss.str();
}

}

#endif // HAS_BINANCE
