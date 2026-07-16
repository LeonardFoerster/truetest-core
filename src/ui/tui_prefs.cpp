#ifdef HAS_RICH_TUI

#include "tui_prefs.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <cstdlib>

namespace truetest::ui {

namespace {

std::filesystem::path resolve_prefs_path()
{
    namespace fs = std::filesystem;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && *xdg) base = fs::path(xdg);
    else
    {
        const char* home = std::getenv("HOME");
        if (!home || !*home) return {};
        base = fs::path(home) / ".config";
    }
    return base / "truetest" / "tui.json";
}

}

std::string tui_prefs_path()
{
    auto p = resolve_prefs_path();
    return p.empty() ? std::string{} : p.string();
}

TuiPrefs load_tui_prefs()
{
    TuiPrefs out;
    auto p = resolve_prefs_path();
    if (p.empty()) return out;

    std::ifstream f(p);
    if (!f.is_open()) return out;

    std::string line;
    while (std::getline(f, line))
    {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.erase(s.begin());
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '"' ||
                                   s.back()  == ',' || s.back()  == '}'))   s.pop_back();
        };
        trim(k); trim(v);
        try {
            if      (k == "active_tab") out.active_tab = std::stoi(v);
            else if (k == "theme")      out.theme      = std::stoi(v);
            else if (k == "frozen")     out.frozen     = std::stoi(v);
        } catch (...) {}
    }
    return out;
}

void save_tui_prefs(const TuiPrefs& in)
{
    namespace fs = std::filesystem;
    auto p = resolve_prefs_path();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    if (!f.is_open()) return;
    f << "{\n";
    f << "  \"active_tab\": " << in.active_tab << ",\n";
    f << "  \"theme\":      " << in.theme      << ",\n";
    f << "  \"frozen\":     " << in.frozen     << "\n";
    f << "}\n";
}

}

#endif // HAS_RICH_TUI
