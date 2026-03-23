#pragma once
#ifdef HAS_BINANCE

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <iomanip>
#include <sstream>
#include <string>

namespace binance {

// HMAC-SHA256 signing for Binance API authentication.
// All signed endpoints require: signature = HMAC-SHA256(secretKey, queryString)
inline std::string hmac_sha256(const std::string& key, const std::string& data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    HMAC(EVP_sha256(),
         key.c_str(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.c_str()),
         data.size(),
         digest, &len);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i)
        ss << std::setw(2) << static_cast<int>(digest[i]);

    return ss.str();
}

// Sign a query string with the API secret and append the signature parameter.
inline std::string sign_query(const std::string& query_string, const std::string& secret)
{
    auto sig = hmac_sha256(secret, query_string);
    return query_string + "&signature=" + sig;
}

// Get current server timestamp in milliseconds (local clock).
inline int64_t server_time_ms()
{
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

// URL-encode a string (minimal: spaces + special chars).
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

} // namespace binance

#endif // HAS_BINANCE
