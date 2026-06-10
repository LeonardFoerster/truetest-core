#pragma once

#include "providers/transport.h"
#include "simulation/monte_carlo_types.h"

#include <memory>
#include <string>
#include <vector>

namespace truetest::simulation {

/**
 * In-memory transport that serves a pre-generated SyntheticPath
 * as CSV lines consumable by the existing CsvBarParser.
 *
 * This allows the SyntheticProvider to be used with zero changes
 * to the DataBridge / bar loading paths in Phase 1.
 */
class SyntheticTransport : public IDataTransport {
public:
    explicit SyntheticTransport(SyntheticPath path);

    bool open() override;
    void close() override;
    bool is_open() const override;
    bool is_streaming() const override { return false; }

    std::optional<std::string> read_line() override;

private:
    SyntheticPath path_;
    std::vector<std::string> lines_;
    size_t next_line_ = 0;
    bool open_ = false;
};

} // namespace truetest::simulation
