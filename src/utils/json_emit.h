#pragma once

// Minimal, dependency-free JSON writer for cold-path serializers.
//
// Shared by cold-path analytics and optional web serializers. The engine
// convention is hand-rolled JSON, and the hot-path gate forbids heavy JSON
// dependencies in latency-sensitive modules, so this utility stays small and
// dependency-free.
//
// The writer tracks commas via a small stack of "is this the first member?"
// flags, so callers never manage separators by hand.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace truetest::json_emit {

class Json
{
public:
    explicit Json(std::string& out) : o_(out) {}

    Json& obj()    { pre(); o_ += '{'; push(); return *this; }
    Json& endobj() { pop(); o_ += '}'; return *this; }
    Json& arr()    { pre(); o_ += '['; push(); return *this; }
    Json& endarr() { pop(); o_ += ']'; return *this; }

    Json& key(const char* k) { pre(); esc(k, len(k)); o_ += ':'; pending_key_ = true; return *this; }

    Json& str(const std::string& s) { pre(); esc(s.data(), s.size()); return *this; }
    Json& str(std::string_view s)   { pre(); esc(s.data(), s.size()); return *this; }
    Json& str(const char* s)        { pre(); esc(s, len(s)); return *this; }
    Json& chr(char c)               { pre(); esc(&c, 1); return *this; }
    Json& boolean(bool v)           { pre(); o_ += v ? "true" : "false"; return *this; }
    Json& null()                    { pre(); o_ += "null"; return *this; }

    Json& num(double v)
    {
        pre();
        if (!std::isfinite(v))
            throw std::invalid_argument(
                "non-finite value in financial JSON document");
        char b[64];
        const auto [end, error] = std::to_chars(
            b, b + sizeof b, v, std::chars_format::general);
        if (error != std::errc{})
            throw std::runtime_error("finite JSON number encoding failed");
        o_.append(b, end);
        return *this;
    }
    Json& inum(long long v)          { pre(); char b[24]; std::snprintf(b, sizeof b, "%lld", v); o_ += b; return *this; }
    Json& unum(unsigned long long v) { pre(); char b[24]; std::snprintf(b, sizeof b, "%llu", v); o_ += b; return *this; }

    // key + value convenience.
    Json& kv(const char* k, double v)             { return key(k).num(v); }
    Json& kv(const char* k, long long v)          { return key(k).inum(v); }
    Json& kv(const char* k, unsigned long long v) { return key(k).unum(v); }
    Json& kv(const char* k, std::size_t v)        { return key(k).unum(static_cast<unsigned long long>(v)); }
    Json& kv(const char* k, int v)                { return key(k).inum(v); }
    Json& kv(const char* k, bool v)               { return key(k).boolean(v); }
    Json& kv(const char* k, const std::string& v) { return key(k).str(v); }
    Json& kv(const char* k, std::string_view v)   { return key(k).str(v); }
    Json& kv(const char* k, const char* v)        { return key(k).str(v ? v : ""); }
    Json& kv_char(const char* k, char v)          { return key(k).chr(v); }

private:
    void push() { stack_.push_back(true); }
    void pop()  { if (!stack_.empty()) stack_.pop_back(); }

    // Emit a separator before the next value/key if required.
    void pre()
    {
        if (pending_key_) { pending_key_ = false; return; } // value immediately after a key
        if (stack_.empty()) return;
        if (stack_.back()) stack_.back() = false;
        else o_ += ',';
    }

    static std::size_t len(const char* s)
    {
        std::size_t n = 0;
        while (s && s[n]) ++n;
        return n;
    }

    void esc(const char* s, std::size_t n)
    {
        o_ += '"';
        for (std::size_t i = 0; i < n; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 0x80)
            {
                std::size_t width = 0;
                if (c >= 0xc2 && c <= 0xdf) width = 2;
                else if (c >= 0xe0 && c <= 0xef) width = 3;
                else if (c >= 0xf0 && c <= 0xf4) width = 4;
                else throw std::invalid_argument("invalid UTF-8 in JSON string");
                if (width > n - i)
                    throw std::invalid_argument("truncated UTF-8 in JSON string");
                const auto continuation = [&](std::size_t offset) {
                    const auto byte = static_cast<unsigned char>(s[i + offset]);
                    return byte >= 0x80 && byte <= 0xbf;
                };
                for (std::size_t offset = 1; offset < width; ++offset)
                    if (!continuation(offset))
                        throw std::invalid_argument("invalid UTF-8 continuation byte");
                const auto second = static_cast<unsigned char>(s[i + 1]);
                if ((c == 0xe0 && second < 0xa0) ||
                    (c == 0xed && second > 0x9f) ||
                    (c == 0xf0 && second < 0x90) ||
                    (c == 0xf4 && second > 0x8f))
                    throw std::invalid_argument("non-scalar UTF-8 in JSON string");
                o_.append(s + i, width);
                i += width - 1;
                continue;
            }
            switch (c)
            {
                case '"':  o_ += "\\\""; break;
                case '\\': o_ += "\\\\"; break;
                case '\n': o_ += "\\n";  break;
                case '\r': o_ += "\\r";  break;
                case '\t': o_ += "\\t";  break;
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o_ += b; }
                    else o_ += static_cast<char>(c);
            }
        }
        o_ += '"';
    }

    std::string&       o_;
    std::vector<bool>  stack_;
    bool               pending_key_ = false;
};

} // namespace truetest::json_emit
