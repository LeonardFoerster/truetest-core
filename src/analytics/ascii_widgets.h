#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tt::ascii {

enum class align { left, right, center };

std::size_t display_width(const std::string& s);

std::string ljust(const std::string& s, std::size_t width);
std::string rjust(const std::string& s, std::size_t width);
std::string cjust(const std::string& s, std::size_t width);

std::string repeat(const std::string& s, std::size_t n);

std::string rule(std::size_t width = 72, const std::string& fill = "\xe2\x94\x80");

std::string section_header(const std::string& title, std::size_t width = 72);

std::string hbar(double value, double max_value, std::size_t width);

std::string sparkline(const std::vector<double>& values, std::size_t max_width = 60);

struct hbin { std::string label; double value; };

std::vector<hbin> equal_width_bins(const std::vector<double>& values, std::size_t bins);

std::string horizontal_histogram(const std::vector<hbin>& bins,
                                 std::size_t bar_width = 30);

std::string table(const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows,
                  const std::vector<align>& alignments = {});

std::string fmt_signed_pct(double fraction, int precision = 2);
std::string fmt_pct(double fraction, int precision = 2);
std::string fmt_money(double v, int precision = 2);
std::string fmt_signed(double v, int precision = 2);

}
