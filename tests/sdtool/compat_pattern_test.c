#include "compat_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* Windows only: the folder listing pattern for a drive root, either separator,
 * and a nested folder. Run under Wine or on Windows (make sdtool-compat-test SDTOOL_TARGET=windows). */

static int failures;

static void expect(const char *folder, const wchar_t *expected) {
    wchar_t wide[PATH_MAX], pattern[PATH_MAX];
    if (!compat_path_to_wide(folder, wide, PATH_MAX) || !compat_dir_pattern(wide, pattern, PATH_MAX) ||
        wcscmp(pattern, expected) != 0) {
        wprintf(L"FAIL %hs -> %ls (wanted %ls)\n", folder, pattern, expected);
        failures++;
    }
}

int main(void) {
    expect("E:\\", L"\\\\?\\E:\\*");
    expect("E:/", L"\\\\?\\E:\\*");
    expect("E:\\Music\\", L"\\\\?\\E:\\Music\\*");
    expect("E:/Music", L"\\\\?\\E:\\Music\\*");
    if (!failures) puts("compat folder patterns: PASS");
    return failures ? 1 : 0;
}
