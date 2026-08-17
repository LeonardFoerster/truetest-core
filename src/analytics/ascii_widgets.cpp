#include "ascii_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace tt::ascii {

namespace {

constexpr const char* EIGHTHS[9] = {
    " ",
    "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c",
    "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89", "\xe2\x96\x88",
};
constexpr const char* BAR_EMPTY = "\xe2\x96\x91";

constexpr const char* SPARK[8] = {
    "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
};

std::string pad(const std::string& s, std::size_t w, bool right_pad)
{
    std::size_t cur = display_width(s);
    if (cur >= w) return s;
    std::string p(w - cur, ' ');
    return right_pad ? s + p : p + s;
}

}

std::size_t display_width(const std::string& s)
{
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string ljust(const std::string& s, std::size_t w) { return pad(s, w, true); }
std::string rjust(const std::string& s, std::size_t w) { return pad(s, w, false); }

std::string cjust(const std::string& s, std::size_t w)
{
    std::size_t cur = display_width(s);
    if (cur >= w) return s;
    std::size_t total = w - cur;
    std::string left(total / 2, ' ');
    std::string right(total - total / 2, ' ');
    return left + s + right;
}

std::string repeat(const std::string& s, std::size_t n)
{
    std::string out;
    out.reserve(s.size() * n);
    for (std::size_t i = 0; i < n; ++i) out += s;
    return out;
}

std::string rule(std::size_t width, const std::string& fill)
{
    return repeat(fill, width);
}

std::string section_header(const std::string& title, std::size_t width)
{
    std::string out = "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81 " + title + " ";
    std::size_t used = display_width(out);
    if (used >= width) return out;
    out += repeat("\xe2\x94\x81", width - used);
    return out;
}

std::string hbar(double value, double max_value, std::size_t width)
{
    if (width == 0) return "";
    if (max_value <= 0.0 || !std::isfinite(value))
        return repeat(BAR_EMPTY, width);

    double frac = std::max(0.0, std::min(1.0, value / max_value));
    std::size_t eighths = static_cast<std::size_t>(
        std::round(frac * static_cast<double>(width) * 8.0));
    std::size_t full = eighths / 8;
    std::size_t rem = eighths % 8;
    if (full > width) { full = width; rem = 0; }

    std::string out;
    for (std::size_t i = 0; i < full; ++i) out += EIGHTHS[8];
    if (full < width && rem > 0) { out += EIGHTHS[rem]; ++full; }
    for (std::size_t i = full; i < width; ++i) out += BAR_EMPTY;
    return out;
}

std::string sparkline(const std::vector<double>& values, std::size_t max_width)
{
    if (values.empty() || max_width == 0) return "";

    std::vector<double> samples;
    if (values.size() <= max_width)
    {
        samples = values;
    }
    else
    {
        samples.reserve(max_width);
        double step = static_cast<double>(values.size()) / static_cast<double>(max_width);
        for (std::size_t i = 0; i < max_width; ++i)
        {
            std::size_t a = static_cast<std::size_t>(static_cast<double>(i) * step);
            std::size_t b = static_cast<std::size_t>(
                static_cast<double>(i + 1) * step);
            if (b <= a) b = a + 1;
            if (b > values.size()) b = values.size();
            double sum = 0.0;
            for (std::size_t j = a; j < b; ++j) sum += values[j];
            samples.push_back(sum / static_cast<double>(b - a));
        }
    }

    double lo = *std::min_element(samples.begin(), samples.end());
    double hi = *std::max_element(samples.begin(), samples.end());
    double range = hi - lo;

    std::string out;
    for (double v : samples)
    {
        int idx = (range <= 0.0)
            ? 3
            : static_cast<int>(std::round(((v - lo) / range) * 7.0));
        if (idx < 0) idx = 0;
        if (idx > 7) idx = 7;
        out += SPARK[idx];
    }
    return out;
}

std::vector<hbin> equal_width_bins(const std::vector<double>& values, std::size_t bins)
{
    std::vector<hbin> out;
    if (values.empty() || bins == 0) return out;

    double lo = *std::min_element(values.begin(), values.end());
    double hi = *std::max_element(values.begin(), values.end());
    if (hi <= lo) hi = lo + 1.0;

    double bw = (hi - lo) / static_cast<double>(bins);
    std::vector<std::size_t> counts(bins, 0);
    for (double v : values)
    {
        std::size_t idx = static_cast<std::size_t>((v - lo) / bw);
        if (idx >= bins) idx = bins - 1;
        ++counts[idx];
    }

    out.reserve(bins);
    for (std::size_t i = 0; i < bins; ++i)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%+.2f..%+.2f",
                      lo + static_cast<double>(i) * bw,
                      lo + static_cast<double>(i + 1) * bw);
        out.push_back({std::string(buf), static_cast<double>(counts[i])});
    }
    return out;
}

std::string horizontal_histogram(const std::vector<hbin>& bins, std::size_t bar_width)
{
    if (bins.empty()) return "";

    std::size_t label_w = 0;
    double maxv = 0.0;
    for (const auto& b : bins)
    {
        label_w = std::max(label_w, display_width(b.label));
        maxv = std::max(maxv, b.value);
    }

    std::ostringstream oss;
    for (const auto& b : bins)
    {
        char countbuf[32];
        std::snprintf(countbuf, sizeof(countbuf), "%6.0f", b.value);
        oss << "  " << ljust(b.label, label_w) << "  "
            << hbar(b.value, maxv, bar_width) << " "
            << countbuf << "\n";
    }
    return oss.str();
}

std::string table(const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows,
                  const std::vector<align>& alignments)
{
    if (headers.empty()) return "";

    std::size_t ncols = headers.size();
    std::vector<std::size_t> widths(ncols);
    for (std::size_t c = 0; c < ncols; ++c) widths[c] = display_width(headers[c]);
    for (const auto& row : rows)
        for (std::size_t c = 0; c < ncols && c < row.size(); ++c)
            widths[c] = std::max(widths[c], display_width(row[c]));

    auto render_cell = [&](const std::string& cell, std::size_t col) -> std::string {
        align a = (col < alignments.size())
            ? alignments[col]
            : (col == 0 ? align::left : align::right);
        switch (a) {
            case align::left:   return ljust(cell, widths[col]);
            case align::right:  return rjust(cell, widths[col]);
            case align::center: return cjust(cell, widths[col]);
        }
        return cell;
    };

    std::ostringstream oss;
    oss << "  ";
    for (std::size_t c = 0; c < ncols; ++c)
        oss << render_cell(headers[c], c) << (c + 1 < ncols ? "  " : "");
    oss << "\n  ";
    for (std::size_t c = 0; c < ncols; ++c)
        oss << std::string(widths[c], '-') << (c + 1 < ncols ? "  " : "");
    oss << "\n";

    for (const auto& row : rows)
    {
        oss << "  ";
        for (std::size_t c = 0; c < ncols; ++c)
        {
            const std::string& cell = c < row.size() ? row[c] : std::string();
            oss << render_cell(cell, c) << (c + 1 < ncols ? "  " : "");
        }
        oss << "\n";
    }
    return oss.str();
}

std::string fmt_signed_pct(double v, int precision)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%+.*f%%", precision, v * 100.0);
    return buf;
}

std::string fmt_pct(double v, int precision)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f%%", precision, v * 100.0);
    return buf;
}

std::string fmt_money(double v, int precision)
{
    char numbuf[64];
    std::snprintf(numbuf, sizeof(numbuf), "%.*f", precision, v);
    std::string s(numbuf);

    bool neg = !s.empty() && s[0] == '-';
    std::size_t int_start = neg ? 1 : 0;
    std::size_t dot = s.find('.');
    std::size_t int_end = (dot == std::string::npos) ? s.size() : dot;

    std::string int_part = s.substr(int_start, int_end - int_start);
    std::string frac_part = (dot == std::string::npos) ? "" : s.substr(dot);

    std::string with_commas;
    for (std::size_t i = 0; i < int_part.size(); ++i)
    {
        if (i > 0 && (int_part.size() - i) % 3 == 0) with_commas += ',';
        with_commas += int_part[i];
    }
    return std::string(neg ? "-" : "") + with_commas + frac_part;
}

std::string fmt_signed(double v, int precision)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%+.*f", precision, v);
    return buf;
}

}
