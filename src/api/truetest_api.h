
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

TRUETEST_API const char* tt_version(void);

TRUETEST_API tt_engine_handle tt_create_engine(const char* config_json);

/* A handle executes at most once. Concurrent calls wait for the first attempt
 * and return its cached code/error without re-running the engine. */
TRUETEST_API int tt_run(tt_engine_handle handle);

/* Serializes with tt_run on the same handle. Each non-NULL result is a distinct
 * allocation that the caller must release with tt_free_string. */
TRUETEST_API const char* tt_get_results(tt_engine_handle handle);

TRUETEST_API void tt_free_string(const char* str);

/* Lifetime precondition: all tt_run/tt_get_results calls using this handle must
 * have returned and been joined. Concurrent destroy/use is unsupported. */
TRUETEST_API void tt_destroy(tt_engine_handle handle);

/* Thread-local diagnostic pointer, valid until the next C API call on the same
 * thread. A failing cached tt_run copies its diagnostic to the calling thread. */
TRUETEST_API const char* tt_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // TRUETEST_API_H
