#include "compat_internal.h"
#include "sys/statvfs.h"
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>

/* Windows side of compat.h: files by UTF-8 name, positional I/O, atomic
 * replacement. Written against the C runtime and Win32 only. */

#undef main
#undef open
#undef close
#undef dup
#undef fopen
#undef stat
#undef _stat64
#undef lstat
#undef fstat
#undef fsync
#undef fdatasync
#undef unlink
#undef rename
#undef access
#undef mkdir

bool compat_to_wide(const char *utf8, wchar_t *out, size_t count) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, (int) count) > 0) return true;
    errno = GetLastError() == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : EINVAL;
    return false;
}

bool compat_from_wide(const wchar_t *wide, char *out, size_t size) {
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int) size, NULL, NULL) > 0;
}

static int errno_from_windows(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ENOENT;
        case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return EACCES;
        case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
        case ERROR_DISK_FULL: return ENOSPC;
        case ERROR_WRITE_PROTECT: return EROFS;
        default: return EIO;
    }
}

int compat_open(const char *path, int flags, ...) {
    wchar_t wide[PATH_MAX];
    if (!compat_to_wide(path, wide, PATH_MAX)) return -1;
    if (flags & O_DIRECTORY) {
        struct _stat64 st;
        if (_wstat64(wide, &st) != 0) return -1;
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        /* A folder cannot be read as a file; the NUL device stands in so the
         * descriptor can be duplicated, closed and handed to the *at() calls. */
        int fd = _open("NUL", _O_RDONLY | _O_BINARY);
        if (fd >= 0 && compat_dir_fd_register(fd, path) != 0) {
            _close(fd);
            return -1;
        }
        return fd;
    }

    DWORD access = (flags & O_WRONLY) ? GENERIC_WRITE : (flags & O_RDWR) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD disposition = OPEN_EXISTING;
    if (flags & O_CREAT)
        disposition = (flags & O_EXCL) ? CREATE_NEW : (flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (flags & O_TRUNC)
        disposition = TRUNCATE_EXISTING;
    /* Sharing delete/rename lets the library replace or unlink a file it
     * still has open, as it may on Linux. */
    HANDLE handle = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = errno_from_windows(GetLastError());
        return -1;
    }
    int fd = _open_osfhandle((intptr_t) handle, (flags & O_APPEND) | _O_BINARY);
    if (fd < 0) CloseHandle(handle);
    return fd;
}

FILE *compat_fopen(const char *path, const char *mode) {
    wchar_t wide_path[PATH_MAX], wide_mode[16];
    if (!compat_to_wide(path, wide_path, PATH_MAX) || !compat_to_wide(mode, wide_mode, 16)) return NULL;
    return _wfopen(wide_path, wide_mode);
}

int compat_close(int fd) {
    compat_dir_fd_forget(fd);
    return _close(fd);
}

int compat_unlink(const char *path) {
    wchar_t wide[PATH_MAX];
    if (!compat_to_wide(path, wide, PATH_MAX)) return -1;
    if (DeleteFileW(wide)) return 0;
    errno = errno_from_windows(GetLastError());
    return -1;
}

/* Unlike the C runtime's rename(), replaces an existing target. */
int compat_rename(const char *from, const char *to) {
    wchar_t wide_from[PATH_MAX], wide_to[PATH_MAX];
    if (!compat_to_wide(from, wide_from, PATH_MAX) || !compat_to_wide(to, wide_to, PATH_MAX)) return -1;
    if (MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    errno = errno_from_windows(GetLastError());
    return -1;
}

int compat_access(const char *path, int mode) {
    wchar_t wide[PATH_MAX];
    return compat_to_wide(path, wide, PATH_MAX) ? _waccess(wide, mode & 6) : -1;
}

int compat_mkdir(const char *path) {
    wchar_t wide[PATH_MAX];
    return compat_to_wide(path, wide, PATH_MAX) ? _wmkdir(wide) : -1;
}

int compat_stat(const char *path, struct _stat64 *st) {
    wchar_t wide[PATH_MAX];
    size_t length;
    if (!compat_to_wide(path, wide, PATH_MAX)) return -1;
    /* The runtime refuses a folder name with a trailing separator. */
    for (length = wcslen(wide); length > 1 && (wide[length - 1] == L'/' || wide[length - 1] == L'\\') &&
                                !(length == 3 && wide[1] == L':'); length--)
        wide[length - 1] = L'\0';
    return _wstat64(wide, st);
}

int compat_fstat(int fd, struct _stat64 *st) {
    const char *dir = compat_dir_fd_path(fd);
    return dir ? compat_stat(dir, st) : _fstat64(fd, st);
}

/* A stand-in for a folder has nothing to flush. */
int compat_fsync(int fd) {
    return compat_dir_fd_path(fd) ? 0 : _commit(fd);
}

int compat_dup(int fd) {
    int copy = _dup(fd);
    const char *dir = compat_dir_fd_path(fd);
    if (copy >= 0 && dir && compat_dir_fd_register(copy, dir) != 0) {
        _close(copy);
        return -1;
    }
    return copy;
}

/* Reads and writes at an offset without moving the file position, so
 * concurrent users of one descriptor do not disturb each other. */
ssize_t pread(int fd, void *buffer, size_t length, off_t offset) {
    HANDLE handle = (HANDLE) _get_osfhandle(fd);
    OVERLAPPED where = {0};
    DWORD got = 0;
    where.Offset = (DWORD) (offset & 0xffffffff);
    where.OffsetHigh = (DWORD) ((uint64_t) offset >> 32);
    if (length > 0x40000000u) length = 0x40000000u;
    if (ReadFile(handle, buffer, (DWORD) length, &got, &where) || GetLastError() == ERROR_HANDLE_EOF)
        return (ssize_t) got;
    errno = errno_from_windows(GetLastError());
    return -1;
}

ssize_t pwrite(int fd, const void *buffer, size_t length, off_t offset) {
    HANDLE handle = (HANDLE) _get_osfhandle(fd);
    OVERLAPPED where = {0};
    DWORD put = 0;
    where.Offset = (DWORD) (offset & 0xffffffff);
    where.OffsetHigh = (DWORD) ((uint64_t) offset >> 32);
    if (length > 0x40000000u) length = 0x40000000u;
    if (WriteFile(handle, buffer, (DWORD) length, &put, &where)) return (ssize_t) put;
    errno = errno_from_windows(GetLastError());
    return -1;
}

long compat_sysconf(int name) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (name == _SC_PAGESIZE) return (long) info.dwPageSize;
    if (name == _SC_NPROCESSORS_ONLN) return (long) info.dwNumberOfProcessors;
    errno = EINVAL;
    return -1;
}

/* Fills the XXXXXX tail from the system's random source, like mkstemp(). */
int compat_mkstemp(char *pattern) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t length = strlen(pattern);
    if (length < 6 || strcmp(pattern + length - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned char random[6];
        if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            errno = EIO;
            return -1;
        }
        for (int i = 0; i < 6; i++) pattern[length - 6 + i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
        int fd = compat_open(pattern, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
}

int statvfs(const char *path, struct statvfs *out) {
    wchar_t wide[PATH_MAX];
    ULARGE_INTEGER available;
    if (!compat_to_wide(path, wide, PATH_MAX)) return -1;
    if (!GetDiskFreeSpaceExW(wide, &available, NULL, NULL)) {
        errno = errno_from_windows(GetLastError());
        return -1;
    }
    out->f_bsize = 4096;
    out->f_favail = 0;
    out->f_bavail = available.QuadPart / out->f_bsize;
    return 0;
}

/* The program starts with the C runtime's ANSI arguments; hand sdtool_main()
 * UTF-8 ones, and keep stdout free of CR/LF translation so each JSON line ends
 * in a plain newline. */
extern int sdtool_main(int argc, char **argv);

int main(void) {
    int argc;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = calloc((size_t) argc + 1, sizeof(*argv));
    if (!wide || !argv) return 2;
    for (int i = 0; i < argc; i++) {
        argv[i] = malloc(wcslen(wide[i]) * 4 + 1);
        if (!argv[i] || !compat_from_wide(wide[i], argv[i], wcslen(wide[i]) * 4 + 1)) return 2;
    }
    _fmode = _O_BINARY;
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    return sdtool_main(argc, argv);
}
