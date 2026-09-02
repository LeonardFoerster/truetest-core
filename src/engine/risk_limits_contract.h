#pragma once

// Engine-owned dependency seam for cold-path composition code. Venue-neutral
// simulation may persist the existing risk_limits value type, but it must not
// acquire a direct dependency on the risk implementation layer.
#include "risk/risk_manager.h"
