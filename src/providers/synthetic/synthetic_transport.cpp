#include "synthetic_transport.h"

#include <sstream>

namespace truetest::simulation {

SyntheticTransport::SyntheticTransport(SyntheticPath path)
    : path_(std::move(path)) {}

bool SyntheticTransport::open() {
    if (open_) {
        next_line_ = 0;
        return true;
    }

    lines_.clear();
    lines_.reserve(path_.bars.size() + 1);

    // Header compatible with CsvBarParser expectations
    lines_.emplace_back("date,symbol,open,high,low,close,volume");

    for (const auto& bar : path_.bars) {
        std::ostringstream oss;
        oss << bar.date << ','
            << bar.symbol << ','
            << bar.open << ','
            << bar.high << ','
            << bar.low << ','
            << bar.close << ','
            << bar.volume;
        lines_.push_back(oss.str());
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
    if (!open_ || next_line_ >= lines_.size()) {
        return std::nullopt;
    }
    return lines_[next_line_++];
}

} // namespace truetest::simulation
