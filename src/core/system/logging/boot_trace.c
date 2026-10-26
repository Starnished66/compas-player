/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "boot_trace.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif

#define BOOT_TRACE_MAX_BYTES (64U * 1024U)
#define BOOT_TRACE_LINE_BYTES 512U
#define BOOT_TRACE_MAX_STEP 200U
#define BOOT_TRACE_ROTATIONS 8U

struct boot_trace_dir {
	int fd;
	char base[BOOT_TRACE_PATH_BYTES];
};

static int negative_errno(void)
{
	return errno ? -errno : -EIO;
}

static int add_suffix(char *out, size_t out_size, const char *base,
		      const char *suffix)
{
	int length = snprintf(out, out_size, "%s%s", base, suffix);
	if (length < 0)
		return -EIO;
	if ((size_t)length >= out_size)
		return -ENAMETOOLONG;
	return 0;
}

static int open_directory(const char *path, struct boot_trace_dir *dir)
{
	char parent[BOOT_TRACE_PATH_BYTES];
	const char *slash;
	struct statfs fs;
	struct statvfs vfs;
	size_t length;

	if (!path || path[0] != '/')
		return -EINVAL;
	length = strnlen(path, BOOT_TRACE_PATH_BYTES);
	if (length == 0 || length >= BOOT_TRACE_PATH_BYTES || path[length - 1] == '/')
		return -ENAMETOOLONG;
	slash = strrchr(path, '/');
	if (!slash || slash[1] == '\0' || strcmp(slash + 1, ".") == 0 ||
	    strcmp(slash + 1, "..") == 0)
		return -EINVAL;
	if (strlen(slash + 1) + sizeof(".boot_trace.lock") > NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(dir->base, slash + 1, strlen(slash + 1) + 1U);
	if (slash == path) {
		memcpy(parent, "/", 2U);
	} else {
		size_t parent_length = (size_t)(slash - path);
		memcpy(parent, path, parent_length);
		parent[parent_length] = '\0';
	}
	dir->fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir->fd < 0)
		return negative_errno();
	if (fstatfs(dir->fd, &fs) != 0 || fstatvfs(dir->fd, &vfs) != 0) {
		int error = negative_errno();
		close(dir->fd);
		dir->fd = -1;
		return error;
	}
	if (fs.f_type == TMPFS_MAGIC || fs.f_type == RAMFS_MAGIC ||
	    (vfs.f_flag & ST_RDONLY)) {
		close(dir->fd);
		dir->fd = -1;
		return -EROFS;
	}
	return 0;
}

static int check_regular_or_absent(int dir_fd, const char *name, int *exists)
{
	struct stat st;
	if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		if (!S_ISREG(st.st_mode))
			return -EINVAL;
		*exists = 1;
		return 0;
	}
	if (errno == ENOENT) {
		*exists = 0;
		return 0;
	}
	return negative_errno();
}

static int validate_trace_names(int dir_fd, const char *base,
				const char *lock_name, int present[9])
{
	char name[NAME_MAX + 1U];
	unsigned int i;
	int rc;
	rc = check_regular_or_absent(dir_fd, base, &present[0]);
	if (rc)
		return rc;
	for (i = 1; i <= BOOT_TRACE_ROTATIONS; ++i) {
		char suffix[16];
		(void)snprintf(suffix, sizeof(suffix), ".%u", i);
		rc = add_suffix(name, sizeof(name), base, suffix);
		if (rc)
			return rc;
		rc = check_regular_or_absent(dir_fd, name, &present[i]);
		if (rc)
			return rc;
	}
	{
		int exists;
		rc = check_regular_or_absent(dir_fd, lock_name, &exists);
	}
	return rc;
}

static int name_for_generation(char *out, size_t size, const char *base,
			       unsigned int generation)
{
	char suffix[16];
	if (generation == 0)
		return add_suffix(out, size, base, "");
	(void)snprintf(suffix, sizeof(suffix), ".%u", generation);
	return add_suffix(out, size, base, suffix);
}

#ifdef BOOT_TRACE_TEST
static int test_rename_failure = -1;
void boot_trace_test_fail_rename_after(int successful_renames)
{
	test_rename_failure = successful_renames;
}
#endif

static int trace_renameat(int dir_fd, const char *from, const char *to)
{
#ifdef BOOT_TRACE_TEST
	if (test_rename_failure == 0) {
		test_rename_failure = -1;
		errno = EIO;
		return -1;
	}
	if (test_rename_failure > 0)
		--test_rename_failure;
#endif
	return renameat(dir_fd, from, dir_fd, to);
}

static int rename_absent(int dir_fd, const char *from, const char *to)
{
	int exists;
	int rc = check_regular_or_absent(dir_fd, to, &exists);
	if (rc)
		return rc;
	if (exists)
		return -EEXIST;
	return trace_renameat(dir_fd, from, to) == 0 ? 0 : negative_errno();
}

static int rename_over_oldest(int dir_fd, const char *from, const char *to)
{
	int source_exists, target_exists;
	int rc = check_regular_or_absent(dir_fd, from, &source_exists);
	if (rc)
		return rc;
	if (!source_exists)
		return -ENOENT;
	rc = check_regular_or_absent(dir_fd, to, &target_exists);
	if (rc)
		return rc;
	(void)target_exists;
	return trace_renameat(dir_fd, from, to) == 0 ? 0 : negative_errno();
}

static void try_rename_absent(int dir_fd, const char *from, const char *to)
{
	int exists;
	if (check_regular_or_absent(dir_fd, from, &exists) != 0 || !exists ||
	    check_regular_or_absent(dir_fd, to, &exists) != 0 || exists)
		return;
	(void)trace_renameat(dir_fd, from, to);
}

/* Shift old generations first and move the live log to .1 last. Failed shifts
 * or a failed final rename leave the newest log at its original path. */
static int rotate_logs(int dir_fd, const char *base, const int present[9])
{
	char current[NAME_MAX + 1U], generations[9][NAME_MAX + 1U];
	int shifted[9] = { 0 };
	int changed = 0;
	unsigned int i;
	int rc;

	rc = name_for_generation(current, sizeof(current), base, 0);
	if (rc || !present[0])
		return rc;
	for (i = 1; i <= BOOT_TRACE_ROTATIONS; ++i) {
		rc = name_for_generation(generations[i], sizeof(generations[i]),
					 base, i);
		if (rc)
			return rc;
	}
	for (i = BOOT_TRACE_ROTATIONS; i > 1U; --i) {
		if (!present[i - 1U])
			continue;
		rc = i == BOOT_TRACE_ROTATIONS ?
			rename_over_oldest(dir_fd, generations[i - 1U], generations[i]) :
			rename_absent(dir_fd, generations[i - 1U], generations[i]);
		if (rc)
			goto rollback;
		shifted[i] = 1;
		changed = 1;
	}
	/* Persist the old-log shifts before moving the newest log out of place. */
	if (changed && fsync(dir_fd) != 0) {
		rc = negative_errno();
		goto rollback;
	}
	rc = rename_absent(dir_fd, current, generations[1]);
	if (rc)
		goto rollback;
	if (fsync(dir_fd) != 0)
		return negative_errno();
	return 0;

rollback:
	for (i = 2U; i <= BOOT_TRACE_ROTATIONS; ++i) {
		if (shifted[i])
			try_rename_absent(dir_fd, generations[i], generations[i - 1U]);
	}
	if (changed)
		(void)fsync(dir_fd);
	return rc;
}

static int write_all(int fd, const char *data, size_t length)
{
	while (length > 0U) {
		ssize_t written = write(fd, data, length);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return negative_errno();
		}
		if (written == 0)
			return -EIO;
		data += (size_t)written;
		length -= (size_t)written;
	}
	return 0;
}

static int append_record(boot_trace_t *trace, const char *line, size_t length)
{
	off_t start;
	int rc;
	if (length > BOOT_TRACE_LINE_BYTES ||
	    trace->bytes > BOOT_TRACE_MAX_BYTES - length)
		return -ENOSPC;
	start = lseek(trace->fd, 0, SEEK_END);
	if (start < 0)
		return negative_errno();
	rc = write_all(trace->fd, line, length);
	if (!rc && fdatasync(trace->fd) != 0)
		rc = negative_errno();
	if (rc) {
		(void)ftruncate(trace->fd, start);
		(void)fdatasync(trace->fd);
		return rc;
	}
	trace->bytes += length;
	return 0;
}

static int read_boot_id(char *out, size_t size)
{
	int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ssize_t n;
	if (fd < 0)
		return negative_errno();
	do {
		n = read(fd, out, size - 1U);
	} while (n < 0 && errno == EINTR);
	close(fd);
	if (n <= 0)
		return n == 0 ? -EIO : negative_errno();
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		--n;
	out[n] = '\0';
	return 0;
}

static int make_launch_id(char out[37])
{
	static const char hex[] = "0123456789abcdef";
	unsigned char bytes[16];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	size_t offset = 0;
	unsigned int i, j = 0;
	if (fd < 0)
		return negative_errno();
	while (offset < sizeof(bytes)) {
		ssize_t n = read(fd, bytes + offset, sizeof(bytes) - offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			int rc = n == 0 ? -EIO : negative_errno();
			close(fd);
			return rc;
		}
		offset += (size_t)n;
	}
	close(fd);
	bytes[6] = (unsigned char)((bytes[6] & 0x0fU) | 0x40U);
	bytes[8] = (unsigned char)((bytes[8] & 0x3fU) | 0x80U);
	for (i = 0; i < sizeof(bytes); ++i) {
		if (i == 4U || i == 6U || i == 8U || i == 10U)
			out[j++] = '-';
		out[j++] = hex[bytes[i] >> 4];
		out[j++] = hex[bytes[i] & 0x0fU];
	}
	out[j] = '\0';
	return 0;
}

static void get_clocks(struct timespec *real, struct timespec *boot)
{
	memset(real, 0, sizeof(*real));
	memset(boot, 0, sizeof(*boot));
	(void)clock_gettime(CLOCK_REALTIME, real);
#ifdef CLOCK_BOOTTIME
	(void)clock_gettime(CLOCK_BOOTTIME, boot);
#else
	(void)clock_gettime(CLOCK_MONOTONIC, boot);
#endif
}

static int append_launch_header(boot_trace_t *trace)
{
	struct timespec real, boot;
	char line[BOOT_TRACE_LINE_BYTES];
	int length;
	get_clocks(&real, &boot);
	length = snprintf(line, sizeof(line),
		"launch=%s event=start boot_id=%s pid=%ld realtime=%lld.%09ld boottime=%lld.%09ld\n",
		trace->launch_id, trace->boot_id, (long)getpid(),
		(long long)real.tv_sec, real.tv_nsec,
		(long long)boot.tv_sec, boot.tv_nsec);
	if (length < 0)
		return -EIO;
	if ((size_t)length >= sizeof(line))
		return -EOVERFLOW;
	return append_record(trace, line, (size_t)length);
}

int boot_trace_open(boot_trace_t *trace, const char *absolute_path)
{
	int incoming_errno = errno;
	struct boot_trace_dir dir = { -1, { 0 } };
	char lock_name[NAME_MAX + 1U];
	int present[9] = { 0 };
	int lock_created = 0, file_created = 0;
	int rc;
	struct stat st;

	if (!trace) {
		errno = incoming_errno;
		return -EINVAL;
	}
	if (trace->initialized || trace->fd >= 0 || trace->lock_fd >= 0) {
		errno = incoming_errno;
		return -EBUSY;
	}
	trace->fd = -1;
	trace->lock_fd = -1;
	trace->initialized = 0;
	trace->failed = 1;
	trace->busy = 0;
	trace->bytes = 0;
	trace->path[0] = '\0';
	trace->boot_id[0] = '\0';
	trace->launch_id[0] = '\0';
	rc = open_directory(absolute_path, &dir);
	if (rc)
		goto done;
	if (strlen(dir.base) + sizeof(".boot_trace.lock") > sizeof(lock_name)) {
		rc = -ENAMETOOLONG;
		goto done;
	}
	rc = add_suffix(lock_name, sizeof(lock_name), dir.base, ".boot_trace.lock");
	if (rc)
		goto done;
	rc = validate_trace_names(dir.fd, dir.base, lock_name, present);
	if (rc)
		goto done;
	if (fstatat(dir.fd, lock_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		if (errno != ENOENT) {
			rc = negative_errno();
			goto done;
		}
		lock_created = 1;
	}
	trace->lock_fd = openat(dir.fd, lock_name,
		O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (trace->lock_fd < 0) {
		rc = negative_errno();
		goto done;
	}
	if (fstat(trace->lock_fd, &st) != 0) {
		rc = negative_errno();
		goto done;
	}
	if (!S_ISREG(st.st_mode)) {
		rc = -EINVAL;
		goto done;
	}
	if (flock(trace->lock_fd, LOCK_EX | LOCK_NB) != 0) {
		rc = negative_errno();
		goto done;
	}
	/* Recheck under the lock to catch another launcher that completed between
	 * the no-side-effect preflight and lock acquisition. */
	rc = validate_trace_names(dir.fd, dir.base, lock_name, present);
	if (rc)
		goto done;
	if (lock_created && fsync(dir.fd) != 0) {
		rc = negative_errno();
		goto done;
	}
	rc = rotate_logs(dir.fd, dir.base, present);
	if (rc)
		goto done;
	if (strlen(absolute_path) >= sizeof(trace->path)) {
		rc = -ENAMETOOLONG;
		goto done;
	}
	memcpy(trace->path, absolute_path, strlen(absolute_path) + 1U);
	trace->fd = openat(dir.fd, dir.base,
		O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		0600);
	file_created = trace->fd >= 0;
	if (trace->fd < 0) {
		rc = negative_errno();
		goto done;
	}
	if (fstat(trace->fd, &st) != 0) {
		rc = negative_errno();
		goto done;
	}
	if (!S_ISREG(st.st_mode)) {
		rc = -EINVAL;
		goto done;
	}
	if (file_created && fsync(dir.fd) != 0) {
		rc = negative_errno();
		goto done;
	}
	rc = make_launch_id(trace->launch_id);
	if (rc)
		goto done;
	rc = read_boot_id(trace->boot_id, sizeof(trace->boot_id));
	if (rc)
		goto done;
	trace->initialized = 1;
	trace->failed = 0;
	rc = append_launch_header(trace);
	if (rc) {
		trace->failed = 1;
		trace->initialized = 0;
		goto done;
	}
	close(dir.fd);
	errno = incoming_errno;
	return 0;

done:
	if (dir.fd >= 0)
		close(dir.fd);
	if (trace->fd >= 0) {
		close(trace->fd);
		trace->fd = -1;
	}
	if (trace->lock_fd >= 0) {
		(void)flock(trace->lock_fd, LOCK_UN);
		close(trace->lock_fd);
		trace->lock_fd = -1;
	}
	trace->failed = 1;
	errno = incoming_errno;
	return rc;
}

static int valid_step(const char *step, size_t *length)
{
	size_t i;
	if (!step)
		return -EINVAL;
	for (i = 0; i <= BOOT_TRACE_MAX_STEP; ++i) {
		unsigned char c = (unsigned char)step[i];
		if (c == '\0') {
			if (i == 0U)
				return -EINVAL;
			*length = i;
			return 0;
		}
		if (i == BOOT_TRACE_MAX_STEP || c < 0x20U || c > 0x7eU ||
		    c == '\n' || c == '\r')
			return -E2BIG;
	}
	return -E2BIG;
}

int boot_trace_checkpoint(boot_trace_t *trace, const char *step)
{
	int incoming_errno = errno;
	size_t step_length = 0;
	struct timespec real, boot;
	char line[BOOT_TRACE_LINE_BYTES];
	int length, rc;
	if (!trace || !trace->initialized || trace->failed || trace->fd < 0) {
		rc = -EINVAL;
		goto done;
	}
	if (__sync_lock_test_and_set(&trace->busy, 1)) {
		rc = -EBUSY;
		goto done;
	}
	rc = valid_step(step, &step_length);
	if (rc)
		goto unlock;
	get_clocks(&real, &boot);
	length = snprintf(line, sizeof(line),
		"launch=%s event=checkpoint step=%s realtime=%lld.%09ld boottime=%lld.%09ld\n",
		trace->launch_id, step, (long long)real.tv_sec, real.tv_nsec,
		(long long)boot.tv_sec, boot.tv_nsec);
	if (length < 0) {
		rc = -EIO;
		goto unlock;
	}
	if ((size_t)length >= sizeof(line) || step_length > BOOT_TRACE_MAX_STEP) {
		rc = -E2BIG;
		goto unlock;
	}
	rc = append_record(trace, line, (size_t)length);
	if (rc && rc != -EINVAL)
		trace->failed = 1;
unlock:
	__sync_lock_release(&trace->busy);
done:
	errno = incoming_errno;
	return rc;
}

void boot_trace_close(boot_trace_t *trace)
{
	int incoming_errno = errno;
	if (trace) {
		if (trace->fd >= 0)
			close(trace->fd);
		if (trace->lock_fd >= 0) {
			(void)flock(trace->lock_fd, LOCK_UN);
			close(trace->lock_fd);
		}
		trace->fd = -1;
		trace->lock_fd = -1;
		trace->initialized = 0;
	}
	errno = incoming_errno;
}
