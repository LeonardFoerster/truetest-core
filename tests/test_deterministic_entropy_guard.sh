#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
guard="$source_root/scripts/check-deterministic-entropy.sh"

"$guard"

test_root="$(mktemp -d)"
trap 'rm -rf -- "$test_root"' EXIT
mkdir -p "$test_root/src/simulation" "$test_root/src/execution"

printf '%s\n' '#include <random>' \
    'void bad() { std::random_device entropy; (void)entropy; }' \
    > "$test_root/src/simulation/bad.cpp"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted std::random_device" >&2
    exit 1
fi

mkdir -p "$test_root/src/engine" "$test_root/src/bin"
printf '%s\n' '#include <chrono>' \
    'auto seed = std::chrono::system_clock::now();' \
    > "$test_root/src/engine/clock_seed.cpp"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted an engine clock-derived seed" >&2
    exit 1
fi

printf '%s\n' '#include <chrono>' \
    'auto unexpected_adapter_clock = std::chrono::steady_clock::now();' \
    > "$test_root/src/execution/execution_adapter.h"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted a new LocalBookAdapter clock" >&2
    exit 1
fi
rm "$test_root/src/execution/execution_adapter.h"

printf '%s\n' '#include <functional>' \
    'auto unexpected_adapter_hash = std::hash<int>{}(42);' \
    > "$test_root/src/execution/execution_adapter.h"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted std::hash in LocalBookAdapter" >&2
    exit 1
fi
rm "$test_root/src/execution/execution_adapter.h"
rm "$test_root/src/engine/clock_seed.cpp"

printf '%s\n' '#include <chrono>' \
    'void bad() {' \
    '  DeterministicRng rng(' \
    '    std::chrono::steady_clock::now().time_since_epoch().count());' \
    '}' > "$test_root/src/bin/clock_rng.inc"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted a multiline clock-seeded RNG" >&2
    exit 1
fi
rm "$test_root/src/bin/clock_rng.inc"

printf '%s\n' '#include <cstdint>' '#include <unistd.h>' \
    'void bad() {' \
    '  DeterministicRng rng(' \
    '    static_cast<std::uint64_t>(::getpid()));' \
    '}' > "$test_root/src/bin/process_rng.inc"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted a PID-seeded RNG" >&2
    exit 1
fi
rm "$test_root/src/bin/process_rng.inc"

printf '%s\n' '#include <functional>' '#include <thread>' \
    'void bad() {' \
    '  DeterministicSeedDeriver seeds(' \
    '    std::hash<std::thread::id>{}(std::this_thread::get_id()));' \
    '}' > "$test_root/src/bin/thread_seed.inc"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted a thread-ID-derived seed" >&2
    exit 1
fi
rm "$test_root/src/bin/thread_seed.inc"

printf '%s\n' '#include <cstdint>' \
    'void bad(void* address) {' \
    '  DeterministicRng rng(' \
    '    reinterpret_cast<std::uintptr_t>(address));' \
    '}' > "$test_root/src/bin/address_rng.inc"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted an address-derived RNG" >&2
    exit 1
fi
rm "$test_root/src/bin/address_rng.inc"

# The live challenge allowlist is path-scoped. Even byte-identical entropy
# expressions in the composition root must remain forbidden.
printf '%s\n' '#include <random>' \
    'void bad() {' \
    '  std::random_device random_device;' \
    '  std::mt19937 random_engine(random_device());' \
    '  std::uniform_int_distribution<int> challenge_operand(100, 9999);' \
    '}' > "$test_root/src/bin/main.inc"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted live-allowlist code in main.inc" >&2
    exit 1
fi
rm "$test_root/src/bin/main.inc"

printf '%s\n' '#include <functional>' \
    'auto bad_hash = std::hash<int>{}(42);' \
    > "$test_root/src/simulation/bad.cpp"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted std::hash in simulation" >&2
    exit 1
fi

printf '%s\n' '#include <chrono>' \
    'auto seed = std::chrono::system_clock::now();' \
    > "$test_root/src/simulation/bad.cpp"
if TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null 2>&1; then
    echo "deterministic entropy guard accepted a clock in simulation" >&2
    exit 1
fi

printf '%s\n' '#include "reproducibility/deterministic_rng.h"' \
    'void good() {}' > "$test_root/src/simulation/good.cpp"
rm "$test_root/src/simulation/bad.cpp"
TT_DETERMINISM_AUDIT_ROOT="$test_root" "$guard" >/dev/null

echo "deterministic entropy guard self-test: OK"
