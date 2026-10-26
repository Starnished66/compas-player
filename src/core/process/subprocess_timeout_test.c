#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "subprocess.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

static void child_write(const char * text) {
    size_t left = strlen(text);
    const char * p = text;
    while (left > 0) {
        ssize_t written = write(STDOUT_FILENO, p, left);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) _exit(2);
        p += written;
        left -= (size_t) written;
    }
}

static volatile sig_atomic_t term_received;

static void record_term(int signal_number) {
    (void) signal_number;
    term_received = 1;
}

static void write_path(const char * path, const char * text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) _exit(3);
    size_t length = strlen(text);
    if (write(fd, text, length) != (ssize_t) length) _exit(3);
    close(fd);
}

static void sleep_ms(unsigned int ms) {
    while (usleep(ms * 1000) < 0 && errno == EINTR) {}
}

static int child_group_trap(const char * marker_path) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = record_term;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0) return 4;

    pid_t grandchild = fork();
    if (grandchild < 0) return 5;
    if (grandchild == 0) {
        signal(SIGTERM, SIG_IGN);
        for (;;) pause();
    }
    while (!term_received) sleep_ms(10);
    write_path(marker_path, "term trap ran\n");
    (void) kill(grandchild, SIGKILL);
    while (waitpid(grandchild, NULL, 0) < 0 && errno == EINTR) {}
    return 0;
}

static int child_group_ignore(const char * parent_sentinel,
                              const char * grandchild_sentinel) {
    signal(SIGTERM, SIG_IGN);
    pid_t grandchild = fork();
    if (grandchild < 0) return 5;
    if (grandchild == 0) {
        signal(SIGTERM, SIG_IGN);
        sleep_ms(2400);
        write_path(grandchild_sentinel, "grandchild survived\n");
        _exit(0);
    }
    sleep_ms(2400);
    write_path(parent_sentinel, "child survived\n");
    return 0;
}

static int child_mode(const char * mode) {
    if (strcmp(mode, "--child-quick") == 0) {
        child_write("quick output\n");
        return 0;
    }
    if (strcmp(mode, "--child-fail") == 0) {
        child_write("failed output\n");
        return 37;
    }
    if (strcmp(mode, "--child-trickle") == 0) {
        for (int i = 0; i < 250; i++) {
            child_write("x");
            usleep(20000);
        }
        return 0;
    }
    if (strcmp(mode, "--child-sleep") == 0) {
        usleep(5000000);
        return 0;
    }
    if (strcmp(mode, "--child-complete") == 0) {
        usleep(20000);
        child_write("completed\n");
        return 0;
    }
    return -1;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int check(int condition, const char * name) {
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", name);
    return 1;
}

int main(int argc, char * argv[]) {
    if (argc == 2 && strncmp(argv[1], "--child-", 8) == 0) {
        int result = child_mode(argv[1]);
        return result < 0 ? 2 : result;
    }
    if (argc == 3 && strcmp(argv[1], "--child-group-trap") == 0)
        return child_group_trap(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--child-group-ignore") == 0)
        return child_group_ignore(argv[2], argv[3]);
    if (argc != 1) {
        fprintf(stderr, "unexpected arguments\n");
        return 2;
    }

    int failures = 0;
    char output[64];
    int exit_code;

    char * quick[] = { argv[0], (char *) "--child-quick", NULL };
    failures += check(subprocess_run_checked(quick, output, sizeof(output), 1000, &exit_code),
                      "quick child completes");
    failures += check(exit_code == 0, "quick child exit code is zero");
    failures += check(strcmp(output, "quick output\n") == 0, "quick child stdout is captured");

    char * fail[] = { argv[0], (char *) "--child-fail", NULL };
    failures += check(subprocess_run_checked(fail, output, sizeof(output), 1000, &exit_code),
                      "nonzero child completes");
    failures += check(exit_code == 37, "nonzero child exit code is preserved");
    failures += check(strcmp(output, "failed output\n") == 0, "nonzero child stdout is captured");

    char * trickle[] = { argv[0], (char *) "--child-trickle", NULL };
    int64_t started = monotonic_ms();
    exit_code = 123;
    bool trickle_result = subprocess_run_checked(trickle, output, sizeof(output), 100, &exit_code);
    int64_t trickle_elapsed = monotonic_ms() - started;
    failures += check(!trickle_result, "trickle child times out");
    failures += check(exit_code == -1, "trickle timeout leaves exit code unset");
    failures += check(trickle_elapsed >= 50 && trickle_elapsed < 500,
                      "trickle timeout honors one deadline");

    char * silent[] = { argv[0], (char *) "--child-sleep", NULL };
    started = monotonic_ms();
    exit_code = 123;
    bool silent_result = subprocess_run_checked(silent, NULL, 0, 100, &exit_code);
    int64_t silent_elapsed = monotonic_ms() - started;
    failures += check(!silent_result, "silent child times out");
    failures += check(exit_code == -1, "silent timeout leaves exit code unset");
    failures += check(silent_elapsed >= 50 && silent_elapsed < 500,
                      "silent timeout honors the deadline");

    char trap_template[] = "/tmp/subprocess-term-trap-XXXXXX";
    int marker_fd = mkstemp(trap_template);
    failures += check(marker_fd >= 0, "create TERM trap marker path");
    if (marker_fd >= 0) close(marker_fd);
    unlink(trap_template);
    char * trap_group[] = { argv[0], (char *) "--child-group-trap",
                            trap_template, NULL };
    started = monotonic_ms();
    exit_code = 123;
    bool trap_result = subprocess_run_checked_group(trap_group, NULL, 0,
                                                     100, &exit_code);
    int64_t trap_elapsed = monotonic_ms() - started;
    failures += check(!trap_result, "TERM-trapping process group times out");
    failures += check(exit_code == -1, "group timeout preserves unset status");
    failures += check(trap_elapsed >= 1400 && trap_elapsed < 3000,
                      "group timeout allows the TERM cleanup grace");
    FILE * marker = fopen(trap_template, "r");
    char marker_text[64] = {0};
    if (marker) {
        (void) fread(marker_text, 1, sizeof(marker_text) - 1, marker);
        fclose(marker);
    }
    failures += check(strcmp(marker_text, "term trap ran\n") == 0,
                      "TERM trap cleans up its child and records evidence");
    unlink(trap_template);

    char child_template[] = "/tmp/subprocess-child-sentinel-XXXXXX";
    char grandchild_template[] = "/tmp/subprocess-grandchild-sentinel-XXXXXX";
    int child_fd = mkstemp(child_template);
    int grandchild_fd = mkstemp(grandchild_template);
    failures += check(child_fd >= 0 && grandchild_fd >= 0,
                      "create process-group sentinel paths");
    if (child_fd >= 0) close(child_fd);
    if (grandchild_fd >= 0) close(grandchild_fd);
    unlink(child_template);
    unlink(grandchild_template);
    char * ignore_group[] = { argv[0], (char *) "--child-group-ignore",
                              child_template, grandchild_template, NULL };
    started = monotonic_ms();
    exit_code = 123;
    bool ignore_result = subprocess_run_checked_group(ignore_group, NULL, 0,
                                                       100, &exit_code);
    int64_t ignore_elapsed = monotonic_ms() - started;
    failures += check(!ignore_result, "TERM-ignoring process group times out");
    failures += check(exit_code == -1, "KILL timeout preserves unset status");
    failures += check(ignore_elapsed >= 1400 && ignore_elapsed < 3000,
                      "group KILL follows the configured grace");
    sleep_ms(1300); /* Pass both children’s delayed sentinel deadlines. */
    failures += check(access(child_template, F_OK) != 0 &&
                      access(grandchild_template, F_OK) != 0,
                      "KILL removes the child and grandchild before sentinels");
    unlink(child_template);
    unlink(grandchild_template);

    char * complete[] = { argv[0], (char *) "--child-complete", NULL };
    failures += check(subprocess_run_checked(complete, output, sizeof(output), 1000, &exit_code),
                      "successful child completes");
    failures += check(exit_code == 0, "successful child exit code is zero");
    failures += check(strcmp(output, "completed\n") == 0, "successful child stdout is captured");

    errno = 0;
    failures += check(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD,
                      "all children are reaped");
    if (failures != 0) return 1;
    puts("subprocess timeout tests passed");
    return 0;
}
