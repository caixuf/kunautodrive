#ifndef FLOWENGINE_PLATFORM_PATHS_H
#define FLOWENGINE_PLATFORM_PATHS_H

/* Cross-platform runtime paths.  Callers must not assume that POSIX /tmp is
 * meaningful on Windows: MinGW resolves it against the current drive, which
 * makes cooperating processes disagree when launched from different drives. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline const char* flow_temp_dir(void) {
    const char* configured = getenv("FLOWENGINE_TEMP_DIR");
    if (configured && configured[0]) return configured;
#if defined(_WIN32)
    static char path[MAX_PATH];
    static int initialized = 0;
    if (!initialized) {
        DWORD n = GetTempPathA((DWORD)sizeof(path), path);
        if (n == 0 || n >= sizeof(path)) snprintf(path, sizeof(path), ".");
        while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/')) path[--n] = '\0';
        initialized = 1;
    }
    return path;
#else
    return "/tmp";
#endif
}

static inline int flow_temp_path(char* out, size_t out_size, const char* name) {
    if (!out || out_size == 0 || !name || !name[0]) return -1;
    const char* dir = flow_temp_dir();
    size_t len = strlen(dir);
    const char sep = (len > 0 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) ? '\0' : '/';
    int n = sep ? snprintf(out, out_size, "%s%c%s", dir, sep, name)
                : snprintf(out, out_size, "%s%s", dir, name);
    return (n >= 0 && (size_t)n < out_size) ? 0 : -1;
}

static inline const char* flow_path_basename(const char* path) {
    if (!path) return "";
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* last = slash;
    if (!last || (backslash && backslash > last)) last = backslash;
    return last ? last + 1 : path;
}

#ifdef __cplusplus
}
#endif

#endif /* FLOWENGINE_PLATFORM_PATHS_H */