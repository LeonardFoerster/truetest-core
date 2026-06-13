#pragma once

// Minimal, dependency-free JSON writer for the web serializers.
//
// The engine convention is hand-rolled JSON (snprintf), and the
// check-hotpath-json.sh gate forbids nlohmann/json in core/engine/execution/
// strategy/providers. src/web/ is off the hot path and outside that gate, but
// we still avoid pulling nlohmann in here so the serializers (and the fixture
// dump tool) compile standalone against the plain struct headers.
//
// The writer tracks commas via a small stack of "is this the first member?"
// flags, so callers never manage separators by hand.

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace truetest::web::jx {

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
    Json& str(const char* s)        { pre(); esc(s, len(s)); return *this; }
    Json& chr(char c)               { pre(); esc(&c, 1); return *this; }
    Json& boolean(bool v)           { pre(); o_ += v ? "true" : "false"; return *this; }
    Json& null()                    { pre(); o_ += "null"; return *this; }

    Json& num(double v)
    {
        pre();
        if (!std::isfinite(v)) { o_ += "null"; return *this; }
        char b[32];
        std::snprintf(b, sizeof b, "%.10g", v);
        o_ += b;
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
            unsigned char c = static_cast<unsigned char>(s[i]);
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

} // namespace truetest::web::jx
