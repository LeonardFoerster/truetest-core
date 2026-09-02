#include "synthetic_transport.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace truetest::simulation {

SyntheticTransport::SyntheticTransport(std::vector<provider::bar> bars)
    : bars_(std::move(bars)) {}

bool SyntheticTransport::open() {
    if (open_) {
        next_line_ = 0;
        return true;
    }

    open_ = true;
    next_line_ = 0;
    return true;
}

void SyntheticTransport::close() {
    open_ = false;
    next_line_ = 0;
}

bool SyntheticTransport::is_open() const {
    return open_;
}

std::optional<std::string> SyntheticTransport::read_line() {
    if (!open_) {
        return std::nullopt;
    }
    if (next_line_ == 0)
    {
        ++next_line_;
        return "date,symbol,open,high,low,close,volume";
    }
    const std::size_t bar_index = next_line_ - 1;
    if (bar_index >= bars_.size())
        return std::nullopt;

    const auto& bar = bars_[bar_index];
    std::ostringstream oss;
    // This is a semantic adapter, not a presentation formatter.
    oss.imbue(std::locale::classic());
    oss << std::setprecision(std::numeric_limits<double>::max_digits10)
        << bar.date << ',' << bar.symbol << ',' << bar.open << ','
        << bar.high << ',' << bar.low << ',' << bar.close << ','
        << bar.volume;
    ++next_line_;
    return oss.str();
}

} // namespace truetest::simulation
