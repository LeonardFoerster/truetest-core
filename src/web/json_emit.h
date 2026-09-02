#pragma once

#include "utils/json_emit.h"

namespace truetest::web {

// Compatibility alias for the optional web layer. The emitter itself belongs
// to the neutral utility layer so analytics does not depend upward on web.
namespace jx = ::truetest::json_emit;

} // namespace truetest::web
