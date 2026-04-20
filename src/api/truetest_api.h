
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

TRUETEST_API int tt_run(tt_engine_handle handle);

TRUETEST_API const char* tt_get_results(tt_engine_handle handle);

TRUETEST_API void tt_free_string(const char* str);

TRUETEST_API void tt_destroy(tt_engine_handle handle);

TRUETEST_API const char* tt_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // TRUETEST_API_H
