#pragma once

#include "types/order_id.h"

namespace truetest::engine_support {

// Engine-owned boundary for trial-local order ids. Composition roots and the
// simulation layer depend on engine, not directly on the leaf types module.
class deterministic_order_id_scope final
{
public:
    deterministic_order_id_scope() noexcept = default;

    deterministic_order_id_scope(const deterministic_order_id_scope&) = delete;
    deterministic_order_id_scope& operator=(
        const deterministic_order_id_scope&) = delete;

private:
    OrderIdGenerator::deterministic_scope scope_;
};

} // namespace truetest::engine_support
