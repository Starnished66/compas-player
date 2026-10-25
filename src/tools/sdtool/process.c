#include "parse_pool.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include "compat_internal.h"
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Starting this program again as a parse worker: where the executable is and
 * how to run it with a file on its standard input and read its standard output. */

static bool self_path(char *out, size_t size) {
#if defined(__APPLE__)
    uint32_t length = (uint32_t) size;
    return _NSGetExecutablePath(out, &length) == 0;
#elif defined(_WIN32)
    wchar_t wide[PATH_MAX];
    DWORD length = GetModuleFileNameW(NULL, wide, PATH_MAX);
    return length > 0 && length < PATH_MAX && compat_from_wide(wide, out, size);
#else
    ssize_t length = readlink("/proc/self/exe", out, size - 1);
    if (length <= 0) return false;
    out[length] = '\0';
    return true;
#endif
}

FILE *sdtool_worker_open(const char *input_file) {
    char exe[PATH_MAX];
    if (!self_path(exe, sizeof(exe))) return NULL;
#ifdef _WIN32
    /* cmd.exe drops the outermost quotes of a quoted command line, so the
     * whole line is quoted once more. */
    char command[2 * PATH_MAX + 64];
    wchar_t wide[2 * PATH_MAX + 64];
    if (snprintf(command, sizeof(command), "\"\"%s\" parse-worker < \"%s\"\"", exe, input_file) >=
            (int) sizeof(command) || !compat_to_wide(command, wide, sizeof(wide) / sizeof(wide[0])))
        return NULL;
    return _wpopen(wide, L"rb");
#else
    /* Single quotes keep any character in the paths literal. */
    char command[2 * PATH_MAX + 64], quoted_exe[PATH_MAX + 64];
    size_t used = 0;
    quoted_exe[used++] = '\'';
    for (const char *p = exe; *p && used + 6 < sizeof(quoted_exe); p++) {
        if (*p == '\'') {
            memcpy(quoted_exe + used, "'\\''", 4);
            used += 4;
        } else {
            quoted_exe[used++] = *p;
        }
    }
    quoted_exe[used++] = '\'';
    quoted_exe[used] = '\0';
    if (snprintf(command, sizeof(command), "%s parse-worker < '%s'", quoted_exe, input_file) >=
        (int) sizeof(command))
        return NULL;
    return popen(command, "r");
#endif
}

void sdtool_worker_close(FILE *worker) {
#ifdef _WIN32
    _pclose(worker);
#else
    pclose(worker);
#endif
}
