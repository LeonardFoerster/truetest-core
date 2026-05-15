# C API

`src/api/truetest_api.h` exposes an opaque-handle + JSON-config C ABI
for embedding the engine into Python (ctypes/cffi), Node (ffi-napi), or
any other language with a C FFI. Built as `libtruetest.so` /
`truetest.dll` with `-DBUILD_SHARED_LIB=ON`.

## Surface

| Function | Purpose |
|----------|---------|
| `tt_version()`                | Library version string. |
| `tt_create_engine(config)`    | Returns an opaque handle. `config` is a JSON string with the same schema accepted by `engine_config`. |
| `tt_run(handle)`              | Drives the engine to completion. Returns 0 on success. |
| `tt_get_results(handle)`      | JSON results blob (caller must `tt_free_string` it). |
| `tt_last_error()`             | Last error string (thread-local). |
| `tt_free_string(str)`         | Free strings returned by the API. |
| `tt_destroy(handle)`          | Tear down the engine and release resources. |

## Notes

- `nlohmann/json` is linked here (and in `src/main.cpp`) for static
  config parsing; this is the only place the API surface touches JSON.
- The shared-lib target picks up the same `ENABLE_*` build flags as the
  three engine binaries — opt-in providers and persistence backends are
  available to embedders if compiled in.
- Live-order paths are still gated by `TT_TARGET`; the shared lib is
  built with the backtest target unless explicitly overridden.
