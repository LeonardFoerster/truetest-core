#pragma once
#ifdef HAS_QUESTDB

#include <cstdint>
#include <string>

namespace truetest::questdb {

// Generates a fresh run_tag if user_override is empty. Format:
//   run_<YYYYMMDD>_<HHMMSS>_<6_char_hex>
// The hex suffix is random (or derived from test_seed when non-zero).
// If user_override is non-empty, returns it unchanged after validation
// (only [A-Za-z0-9_] permitted, max 64 chars). Throws std::invalid_argument
// on bad override.
std::string make_run_tag(const std::string& user_override,
                         std::uint64_t test_seed = 0);

// True iff tag is a valid QuestDB table-name prefix.
bool is_valid_run_tag(const std::string& tag);

}

#endif // HAS_QUESTDB
