// Entry point for engine_backtest.
//
// Step 10 requires each binary under src/bin/<target>/ to own its own
// translation unit so per-binary target_link_libraries can diverge. Today
// all three binaries compile the same implementation (src/bin/main.inc)
// with a distinct TT_TARGET define set by the CMake build.
//
// As deepdive phases land, code that is *live-only* (REST executor,
// credential store, LibTorch) will migrate out of the shared inc and into
// src/bin/engine_live/ directly, making this wrapper diverge.
#include "../main.inc"
