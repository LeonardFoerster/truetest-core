#pragma once

#include "providers/provider_event.h"
#include "providers/transport.h"

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
    explicit SyntheticTransport(std::vector<provider::bar> bars);

    bool open() override;
    void close() override;
    bool is_open() const override;
    bool is_streaming() const override { return false; }

    std::optional<std::string> read_line() override;

private:
    std::vector<provider::bar> bars_;
    size_t next_line_ = 0;
    bool open_ = false;
};

} // namespace truetest::simulation
