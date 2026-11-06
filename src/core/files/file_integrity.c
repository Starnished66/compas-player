#include "file_integrity.h"
#include "mbedtls/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

bool file_sha256_stream(FILE *file, uint64_t *size, char hex[65]) {
    mbedtls_sha256_context context;
    unsigned char buffer[8192], digest[32];
    uint64_t total = 0;
    size_t count;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts(&context, 0) == 0;
    while (ok && (count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        ok = mbedtls_sha256_update(&context, buffer, count) == 0;
        total += count;
    }
    ok = ok && !ferror(file) && mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!ok) return false;
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    *size = total;
    return true;
}

bool file_sha256(const char *path, uint64_t *size, char hex[65]) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = file_sha256_stream(file, size, hex);
    fclose(file);
    return ok;
}

bool filesystem_available_bytes(const char *path, uint64_t *bytes) {
    struct statvfs fs;
    if (!path || !bytes || statvfs(path, &fs) != 0) return false;
    uint64_t unit = fs.f_frsize;
    *bytes = unit && fs.f_bavail > UINT64_MAX / unit ? UINT64_MAX : fs.f_bavail * unit;
    return true;
}

bool filesystem_sync_directory(int fd, bool *durable) {
    static atomic_bool warned = false;
    *durable = fsync(fd) == 0;
    if (*durable) return true;
    int saved = errno;
    if (saved != EINVAL && saved != ENOTSUP && saved != EOPNOTSUPP && saved != ENOSYS)
        return false;
    if (!atomic_exchange(&warned, true))
        fprintf(stderr, "filesystem: directory sync unsupported; committed files may not survive power loss\n");
    errno = saved;
    return true;
}

bool filesystem_sync_parent(const char *path) {
    char parent[PATH_MAX];
    if (!path || strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return false; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    bool durable;
    bool ok = filesystem_sync_directory(fd, &durable);
    int saved = errno;
    if (close(fd) != 0 && ok) return false;
    errno = saved;
    return ok;
}
