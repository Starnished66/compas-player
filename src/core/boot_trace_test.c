/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include "boot_trace.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef BOOT_TRACE_TEST
void boot_trace_test_fail_rename_after(int successful_renames);
#endif

static void make_path(char *out, size_t size, const char *dir,
		      const char *name)
{
	int length = snprintf(out, size, "%s/%s", dir, name);
	assert(length > 0 && (size_t)length < size);
}

static void write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
	assert(close(fd) == 0);
}

static char *read_text(const char *path)
{
	struct stat st;
	char *buffer;
	size_t offset = 0;
	int fd = open(path, O_RDONLY);
	assert(fd >= 0);
	assert(fstat(fd, &st) == 0 && st.st_size >= 0);
	buffer = malloc((size_t)st.st_size + 1U);
	assert(buffer);
	while (offset < (size_t)st.st_size) {
		ssize_t n = read(fd, buffer + offset, (size_t)st.st_size - offset);
		assert(n > 0);
		offset += (size_t)n;
	}
	buffer[offset] = '\0';
	assert(close(fd) == 0);
	return buffer;
}

static int contains(const char *path, const char *needle)
{
	char *contents = read_text(path);
	int found = strstr(contents, needle) != NULL;
	free(contents);
	return found;
}

static void test_history_and_quota(const char *dir)
{
	char path[512], generation[512], marker[64], long_step[300];
	unsigned int i;
	boot_trace_t trace = BOOT_TRACE_INITIALIZER;
	make_path(path, sizeof(path), dir, "boot.log");
	for (i = 1; i <= 12U; ++i) {
		int length = snprintf(marker, sizeof(marker), "launch-marker-%02u", i);
		int rc;
		assert(length > 0 && (size_t)length < sizeof(marker));
		errno = EDOM;
		rc = boot_trace_open(&trace, path);
		if (rc != 0)
			fprintf(stderr, "open launch %u failed: %d\n", i, rc);
		assert(rc == 0);
		assert(errno == EDOM);
		assert(trace.bytes > 0U && trace.bytes < 512U);
		assert(trace.boot_id[0] != '\0' && strlen(trace.launch_id) == 36U);
		assert(boot_trace_checkpoint(&trace, marker) == 0);
		assert(errno == EDOM);
		boot_trace_close(&trace);
	}
	assert(contains(path, "launch-marker-12"));
	for (i = 1; i <= 8U; ++i) {
		int length = snprintf(generation, sizeof(generation), "%s.%u", path, i);
		unsigned int launch = 12U - i;
		assert(length > 0 && (size_t)length < sizeof(generation));
		length = snprintf(marker, sizeof(marker), "launch-marker-%02u", launch);
		assert(length > 0 && (size_t)length < sizeof(marker));
		assert(contains(generation, marker));
	}
	assert(snprintf(generation, sizeof(generation), "%s.9", path) > 0);
	assert(access(generation, F_OK) != 0 && errno == ENOENT);

	assert(boot_trace_open(&trace, path) == 0);
	memset(long_step, 'x', sizeof(long_step) - 1U);
	long_step[sizeof(long_step) - 1U] = '\0';
	assert(boot_trace_checkpoint(&trace, long_step) == -E2BIG);
	assert(!trace.failed);
	assert(boot_trace_checkpoint(&trace, "still-usable") == 0);
	memset(long_step, 'q', 180U);
	long_step[180] = '\0';
	for (;;) {
		int rc = boot_trace_checkpoint(&trace, long_step);
		if (rc == -ENOSPC)
			break;
		assert(rc == 0);
	}
	assert(trace.bytes <= 64U * 1024U);
	assert(trace.failed);
	boot_trace_close(&trace);
}

static void test_symlink_and_failed_rotation(const char *dir)
{
	char path[512], target[512], link_path[512], lock_path[512];
	boot_trace_t trace = BOOT_TRACE_INITIALIZER;
	make_path(path, sizeof(path), dir, "protected.log");
	make_path(target, sizeof(target), dir, "link-target");
	make_path(link_path, sizeof(link_path), dir, "protected.log.4");
	make_path(lock_path, sizeof(lock_path), dir, "protected.log.boot_trace.lock");
	write_text(path, "preserve-current\n");
	assert(symlink(target, link_path) == 0);
	assert(boot_trace_open(&trace, path) == -EINVAL);
	assert(contains(path, "preserve-current"));
	assert(access(lock_path, F_OK) != 0 && errno == ENOENT);
	assert(unlink(link_path) == 0);

	/* A symlink at the live name is rejected without opening or replacing it. */
	assert(unlink(path) == 0);
	assert(symlink(target, path) == 0);
	assert(boot_trace_open(&trace, path) == -EINVAL);
	{
		struct stat st;
		assert(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
	}
	assert(unlink(path) == 0);
}

static void test_failed_rotation_keeps_current(const char *dir)
{
	char path[512], old_path[512], shifted_path[512];
	boot_trace_t trace = BOOT_TRACE_INITIALIZER;
	make_path(path, sizeof(path), dir, "rotation-failure.log");
	make_path(old_path, sizeof(old_path), dir, "rotation-failure.log.1");
	make_path(shifted_path, sizeof(shifted_path), dir, "rotation-failure.log.2");
	assert(boot_trace_open(&trace, path) == 0);
	assert(boot_trace_checkpoint(&trace, "older-generation") == 0);
	boot_trace_close(&trace);
	assert(boot_trace_open(&trace, path) == 0);
	assert(boot_trace_checkpoint(&trace, "latest-generation") == 0);
	boot_trace_close(&trace);
	/* Let .1 -> .2 succeed, then fail the live-current -> .1 rename. */
	boot_trace_test_fail_rename_after(1);
	assert(boot_trace_open(&trace, path) == -EIO);
	assert(contains(path, "latest-generation"));
	assert(contains(old_path, "older-generation"));
	assert(access(shifted_path, F_OK) != 0 && errno == ENOENT);
}

static void test_tmpfs_rejection(void)
{
	char path[512];
	boot_trace_t trace = BOOT_TRACE_INITIALIZER;
	int length = snprintf(path, sizeof(path), "/dev/shm/compas-boot-trace-test-%ld.log",
			      (long)getpid());
	if (length <= 0 || (size_t)length >= sizeof(path) || access("/dev/shm", W_OK) != 0)
		return;
	assert(boot_trace_open(&trace, path) == -EROFS);
	assert(access(path, F_OK) != 0 && errno == ENOENT);
	{
		char lock_path[512];
		assert(snprintf(lock_path, sizeof(lock_path), "%s.boot_trace.lock", path) > 0);
		assert(access(lock_path, F_OK) != 0 && errno == ENOENT);
	}
}

static void test_single_writer_lock(const char *dir)
{
	char path[512];
	int rc;
	boot_trace_t first = BOOT_TRACE_INITIALIZER;
	boot_trace_t second = BOOT_TRACE_INITIALIZER;
	make_path(path, sizeof(path), dir, "locked.log");
	assert(boot_trace_open(&first, path) == 0);
	errno = EDOM;
	rc = boot_trace_open(&second, path);
	assert(rc == -EWOULDBLOCK || rc == -EAGAIN);
	assert(errno == EDOM);
	assert(contains(path, "event=start"));
	boot_trace_close(&first);
	assert(boot_trace_open(&second, path) == 0);
	boot_trace_close(&second);
}

static void test_reopen_active_context(const char *dir)
{
	char path[512], previous[512];
	boot_trace_t trace = BOOT_TRACE_INITIALIZER;
	make_path(path, sizeof(path), dir, "same-context.log");
	make_path(previous, sizeof(previous), dir, "same-context.log.1");
	assert(boot_trace_open(&trace, path) == 0);
	assert(boot_trace_checkpoint(&trace, "before-reopen") == 0);
	errno = EDOM;
	assert(boot_trace_open(&trace, path) == -EBUSY);
	assert(errno == EDOM);
	assert(boot_trace_checkpoint(&trace, "after-reopen-rejected") == 0);
	assert(access(previous, F_OK) != 0 && errno == ENOENT);
	boot_trace_close(&trace);
	assert(boot_trace_open(&trace, path) == 0);
	assert(contains(previous, "before-reopen"));
	assert(contains(previous, "after-reopen-rejected"));
	boot_trace_close(&trace);
}

static void test_partial_write_rollback(const char *dir)
{
	char path[512];
	boot_trace_t trace = BOOT_TRACE_INITIALIZER;
	struct stat before, after;
	struct rlimit old_limit, limited;
	void (*old_handler)(int);
	make_path(path, sizeof(path), dir, "partial.log");
	assert(boot_trace_open(&trace, path) == 0);
	assert(stat(path, &before) == 0);
	assert(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
	if (old_limit.rlim_max != RLIM_INFINITY &&
	    (uintmax_t)before.st_size + 1U >= (uintmax_t)old_limit.rlim_max) {
		boot_trace_close(&trace);
		return;
	}
	old_handler = signal(SIGXFSZ, SIG_IGN);
	assert(old_handler != SIG_ERR);
	limited = old_limit;
	limited.rlim_cur = (rlim_t)before.st_size + 1U;
	assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
	assert(boot_trace_checkpoint(&trace, "force-partial-write") < 0);
	assert(trace.failed);
	assert(stat(path, &after) == 0 && after.st_size == before.st_size);
	assert(setrlimit(RLIMIT_FSIZE, &old_limit) == 0);
	assert(signal(SIGXFSZ, old_handler) != SIG_ERR);
	boot_trace_close(&trace);
}

int main(void)
{
	char cwd[512], template[1024];
	char *dir;
	int template_length;
	assert(getcwd(cwd, sizeof(cwd)) != NULL);
	template_length = snprintf(template, sizeof(template),
				   "%s/build_ui_test/boot_trace_test.XXXXXX", cwd);
	assert(template_length > 0 && (size_t)template_length < sizeof(template));
	dir = mkdtemp(template);
	assert(dir);
	test_history_and_quota(dir);
	test_symlink_and_failed_rotation(dir);
	test_failed_rotation_keeps_current(dir);
	test_tmpfs_rejection();
	test_single_writer_lock(dir);
	test_reopen_active_context(dir);
	test_partial_write_rollback(dir);
	puts("boot_trace: PASS");
	{
		DIR *directory = opendir(dir);
		struct dirent *entry;
		assert(directory);
		while ((entry = readdir(directory)) != NULL) {
			char file[1200];
			int length;
			if (strcmp(entry->d_name, ".") == 0 ||
			    strcmp(entry->d_name, "..") == 0)
				continue;
			length = snprintf(file, sizeof(file), "%s/%s", dir,
					  entry->d_name);
			assert(length > 0 && (size_t)length < sizeof(file));
			assert(unlink(file) == 0);
		}
		assert(closedir(directory) == 0);
		assert(rmdir(dir) == 0);
	}
	return 0;
}
