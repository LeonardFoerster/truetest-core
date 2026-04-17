#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tt::ascii {

enum class align { left, right, center };

// Number of visible codepoints in a UTF-8 string (BMP chars treated as width 1).
// Used so padding math is correct when unicode block glyphs appear in output.
std::size_t display_width(const std::string& s);

std::string ljust(const std::string& s, std::size_t width);
std::string rjust(const std::string& s, std::size_t width);
std::string cjust(const std::string& s, std::size_t width);

// Repeat a (possibly multi-byte) string n times.
std::string repeat(const std::string& s, std::size_t n);

// Horizontal rule: `width` codepoints of `fill`.
std::string rule(std::size_t width = 72, const std::string& fill = "\xe2\x94\x80");

// "━━━ <title> ━━━━━━━━━━" — total visible width = `width`.
std::string section_header(const std::string& title, std::size_t width = 72);

// Horizontal bar: fills proportional to value / max_value.
// Uses one-eighth unicode blocks for sub-cell precision. Empty slots are ░.
std::string hbar(double value, double max_value, std::size_t width);

// One-line sparkline using ▁▂▃▄▅▆▇█. Downsamples by bucket-average when
// values.size() exceeds max_width.
std::string sparkline(const std::vector<double>& values, std::size_t max_width = 60);

// Histogram bin (label + count/weight).
struct hbin { std::string label; double value; };

// Partition `values` into `bins` equal-width buckets.
std::vector<hbin> equal_width_bins(const std::vector<double>& values, std::size_t bins);

// Render labelled bars: "  <label>  <bar>  <count>".
std::string horizontal_histogram(const std::vector<hbin>& bins,
                                 std::size_t bar_width = 30);

// Render a table with per-column alignment.
std::string table(const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows,
                  const std::vector<align>& alignments = {});

// Number formatting.
std::string fmt_signed_pct(double fraction, int precision = 2);   // "+18.42%"
std::string fmt_pct(double fraction, int precision = 2);          // "18.42%"
std::string fmt_money(double v, int precision = 2);               // "12,345.67"
std::string fmt_signed(double v, int precision = 2);              // "+123.45"

} // namespace tt::ascii
