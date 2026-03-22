#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;
extern const char *PLUGIN_REPO_ROOT;

void obs_log(int log_level, const char *format, ...);
extern void blogva(int log_level, const char *format, va_list args);

#ifdef __cplusplus
}
#endif
