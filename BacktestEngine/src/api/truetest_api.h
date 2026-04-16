// TrueTest C API (P1)
//
// Minimal, stable C ABI for embedding the TrueTest backtesting engine
// in host languages such as Python (via ctypes/cffi) or Node.js
// (via ffi-napi). The API exposes opaque handles and JSON strings to
// keep the ABI surface tiny.
//
// Lifecycle:
//     tt_engine_handle h = tt_create_engine(config_json);
//     if (!h) { const char* err = tt_last_error(); ... }
//     int rc = tt_run(h);
//     const char* results = tt_get_results(h);
//     // ... copy results into host string ...
//     tt_free_string(results);
//     tt_destroy(h);

#ifndef TRUETEST_API_H
#define TRUETEST_API_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef TRUETEST_API_BUILDING
#    define TRUETEST_API __declspec(dllexport)
#  else
#    define TRUETEST_API __declspec(dllimport)
#  endif
#else
#  define TRUETEST_API __attribute__((visibility("default")))
#endif

typedef void* tt_engine_handle;

// Return the API / library version string ("0.1.0"). Always non-null.
TRUETEST_API const char* tt_version(void);

// Create a new engine from a JSON configuration string. Returns NULL on error
// (in which case tt_last_error() yields a description). The handle must be
// released with tt_destroy() to avoid leaks.
//
// Config schema (all fields optional unless noted):
//   {
//     "data_path":   "market_data.csv",   // REQUIRED — path to OHLCV CSV
//     "strategy":    "sma" | "mean-reversion" | "ma-crossover",
//     "initial_balance": 10000.0,
//     "seed":        0,
//     "rolling_window": 252,
//     "risk_free_rate": 0.0,
//     "market_aggression": 1.1,
//     "qty_scale":   1e8,
//     "fill_rng_seed": 42,
//     "spread_step_factor": 0.0001,
//     "params":      {"period": 20, ...}   // passed to strategy.set_param()
//   }
TRUETEST_API tt_engine_handle tt_create_engine(const char* config_json);

// Run the backtest synchronously. Returns 0 on success, non-zero on failure.
// After a non-zero return, tt_last_error() may contain additional context.
TRUETEST_API int tt_run(tt_engine_handle handle);

// Returns a heap-allocated JSON string with the analytics results.
// Caller must release with tt_free_string(). Returns NULL on error.
TRUETEST_API const char* tt_get_results(tt_engine_handle handle);

// Release a string previously returned from tt_get_results() or tt_last_error().
// Passing NULL is safe and a no-op.
TRUETEST_API void tt_free_string(const char* str);

// Release an engine handle. Passing NULL is safe and a no-op.
TRUETEST_API void tt_destroy(tt_engine_handle handle);

// Returns a pointer to a thread-local string describing the last error in
// this thread. Always non-null; empty string means no error.
TRUETEST_API const char* tt_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // TRUETEST_API_H
