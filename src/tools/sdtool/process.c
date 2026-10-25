#include "parse_pool.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "compat_internal.h"
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

/* Starting this program again as a parse worker. Two backends, no shell:
 * posix_spawn on Linux and macOS, CreateProcessW on Windows. */

struct sdtool_worker {
    FILE *output;
#ifdef _WIN32
    HANDLE process;
#else
    pid_t pid;
#endif
};

FILE *sdtool_worker_output(sdtool_worker_t *worker) { return worker->output; }

#ifdef _WIN32

sdtool_worker_t *sdtool_worker_start(const char *input_file) {
    wchar_t exe[PATH_MAX], list[PATH_MAX], command[PATH_MAX + 32];
    DWORD length = GetModuleFileNameW(NULL, exe, PATH_MAX);
    if (length == 0 || length >= PATH_MAX || !compat_path_to_wide(input_file, list, PATH_MAX) ||
        swprintf(command, sizeof(command) / sizeof(command[0]), L"\"%ls\" parse-worker", exe) < 0)
        return NULL;

    /* The child inherits exactly its three standard handles (list file, result
     * pipe, and the null device for errors) and nothing else of ours. */
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    HANDLE input = CreateFileW(list, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE null_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE read_end = NULL, write_end = NULL;
    if (input == INVALID_HANDLE_VALUE || null_out == INVALID_HANDLE_VALUE ||
        !CreatePipe(&read_end, &write_end, &inherit, 0)) {
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (null_out != INVALID_HANDLE_VALUE) CloseHandle(null_out);
        return NULL;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    HANDLE inherited[3] = { input, write_end, null_out };
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = malloc(attribute_size);
    bool listed = attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) &&
                  UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                            sizeof(inherited), NULL, NULL);
    STARTUPINFOEXW startup = { .StartupInfo = { .cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES,
                                                .hStdInput = input, .hStdOutput = write_end,
                                                .hStdError = null_out },
                               .lpAttributeList = attributes };
    PROCESS_INFORMATION info;
    BOOL started = listed && CreateProcessW(NULL, command, NULL, NULL, TRUE,
                                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                                            &startup.StartupInfo, &info);
    if (attributes) {
        if (listed) DeleteProcThreadAttributeList(attributes);
        free(attributes);
    }
    CloseHandle(null_out);
    CloseHandle(input);
    CloseHandle(write_end);
    sdtool_worker_t *worker = started ? calloc(1, sizeof(*worker)) : NULL;
    int fd = worker ? _open_osfhandle((intptr_t) read_end, _O_RDONLY | _O_BINARY) : -1;
    if (fd >= 0) worker->output = _fdopen(fd, "rb");
    if (!worker || !worker->output) {
        if (started) {
            TerminateProcess(info.hProcess, 1);
            CloseHandle(info.hProcess);
        }
        if (fd >= 0) _close(fd); else CloseHandle(read_end);
        free(worker);
        return NULL;
    }
    CloseHandle(info.hThread);
    worker->process = info.hProcess;
    return worker;
}

int sdtool_worker_finish(sdtool_worker_t *worker) {
    DWORD status = (DWORD) -1;
    fclose(worker->output);
    WaitForSingleObject(worker->process, INFINITE);
    GetExitCodeProcess(worker->process, &status);
    CloseHandle(worker->process);
    free(worker);
    return (int) status;
}

#else

static bool self_path(char *out, size_t size) {
#ifdef __APPLE__
    uint32_t length = (uint32_t) size;
    return _NSGetExecutablePath(out, &length) == 0;
#else
    ssize_t length = readlink("/proc/self/exe", out, size - 1);
    if (length <= 0) return false;
    out[length] = '\0';
    return true;
#endif
}

sdtool_worker_t *sdtool_worker_start(const char *input_file) {
    char exe[PATH_MAX];
    int pipe_fds[2];
    int input = open(input_file, O_RDONLY | O_CLOEXEC);
    if (input < 0) return NULL;
    if (!self_path(exe, sizeof(exe)) || pipe(pipe_fds) != 0) {
        close(input);
        return NULL;
    }
    fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    char *argv[] = { exe, "parse-worker", NULL };
    pid_t pid = 0;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc == 0) {
        /* dup2 onto 0 and 1 clears close-on-exec on the copies only. */
        rc = posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
        if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        if (rc == 0) rc = posix_spawn(&pid, exe, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(input);
    close(pipe_fds[1]);
    sdtool_worker_t *worker = rc == 0 ? calloc(1, sizeof(*worker)) : NULL;
    if (worker) worker->output = fdopen(pipe_fds[0], "rb");
    if (!worker || !worker->output) {
        if (rc == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }
        close(pipe_fds[0]);
        free(worker);
        return NULL;
    }
    worker->pid = pid;
    return worker;
}

int sdtool_worker_finish(sdtool_worker_t *worker) {
    int status = 0;
    fclose(worker->output);
    pid_t waited;
    do waited = waitpid(worker->pid, &status, 0); while (waited < 0 && errno == EINTR);
    free(worker);
    return waited > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif
