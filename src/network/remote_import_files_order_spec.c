#define _GNU_SOURCE
#include "remote_import_files.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

bool file_op_external_begin(void) { return true; }
void file_op_external_end(void) {}
bool usb_mode_control_storage_write_begin(void) { return true; }
void usb_mode_control_storage_write_end(void) {}

static void create_file(const char *name) {
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
}

static void create_dir(const char *name) {
    assert(mkdir(name, 0700) == 0);
}

typedef struct {
    int fd;
    const char *query;
} request_t;

static void *serve_page(void *opaque) {
    request_t *request = opaque;
    assert(remote_import_files_handle(request->fd, "GET", request->query, 0, false));
    close(request->fd);
    return NULL;
}

static char *get_page(const char *query) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    request_t request = {sockets[0], query};
    pthread_t server;
    assert(pthread_create(&server, NULL, serve_page, &request) == 0);

    size_t cap = 4096, used = 0;
    char *response = malloc(cap);
    assert(response);
    for (;;) {
        if (used + 1024 + 1 > cap) {
            cap *= 2;
            response = realloc(response, cap);
            assert(response);
        }
        ssize_t n = read(sockets[1], response + used, cap - used - 1);
        assert(n >= 0);
        if (n == 0) break;
        used += (size_t)n;
    }
    close(sockets[1]);
    assert(pthread_join(server, NULL) == 0);
    response[used] = '\0';
    char *body = strstr(response, "\r\n\r\n");
    assert(body);
    body += 4;

    char *json = malloc(used + 1);
    assert(json);
    size_t json_size = 0;
    while (*body) {
        char *line_end = strstr(body, "\r\n");
        assert(line_end);
        unsigned long chunk_size = strtoul(body, NULL, 16);
        body = line_end + 2;
        if (chunk_size == 0) break;
        assert(strlen(body) >= chunk_size + 2);
        memcpy(json + json_size, body, chunk_size);
        json_size += chunk_size;
        body += chunk_size;
        assert(body[0] == '\r' && body[1] == '\n');
        body += 2;
    }
    json[json_size] = '\0';
    free(response);
    return json;
}

static void assert_order(const char *json, const char *const *names, size_t count) {
    const char *cursor = json;
    for (size_t i = 0; i < count; i++) {
        char needle[300];
        snprintf(needle, sizeof(needle), "\"name\":\"%s\"", names[i]);
        const char *found = strstr(cursor, needle);
        assert(found);
        cursor = found + strlen(needle);
    }
    assert(!strstr(cursor, "\"name\":\""));
}

static void create_generated_files(unsigned count) {
    assert(mkdir("many", 0700) == 0);
    assert(chdir("many") == 0);
    for (unsigned i = count; i > 0; i--) {
        char name[32];
        snprintf(name, sizeof(name), "item%04u", i - 1);
        create_file(name);
    }
    create_file(".hidden");
    assert(symlink("item0000", "linked-item") == 0);
    assert(chdir("..") == 0);
}

static void assert_generated_page(const char *json, unsigned first, unsigned count) {
    const char *cursor = json;
    for (unsigned i = 0; i < count; i++) {
        char needle[64], name[32];
        snprintf(name, sizeof(name), "item%04u", first + i);
        snprintf(needle, sizeof(needle), "\"name\":\"%s\"", name);
        const char *found = strstr(cursor, needle);
        assert(found);
        cursor = found + strlen(needle);
    }
    assert(!strstr(cursor, "\"name\":\""));
}

int main(void) {
    assert(mkdir("music", 0700) == 0);
    assert(chdir("music") == 0);
    create_file("zeta");
    create_file("alpha");
    create_file("Beta");
    create_file("ALPHA");
    create_file("éclair");
    create_dir("beta");
    create_dir("Alpha");
    assert(chdir("..") == 0);

    assert(remote_import_files_start());
    char *first = get_page("/api/import/files?path=&offset=0&limit=3");
    const char *first_names[] = {"Alpha", "beta", "ALPHA"};
    assert_order(first, first_names, 3);
    assert(strstr(first, "\"total\":7"));
    assert(strstr(first, "\"next_offset\":3"));
    free(first);

    char *second = get_page("/api/import/files?path=&offset=3&limit=3");
    const char *second_names[] = {"alpha", "Beta", "zeta"};
    assert_order(second, second_names, 3);
    assert(strstr(second, "\"next_offset\":6"));
    free(second);

    char *third = get_page("/api/import/files?path=&offset=6&limit=3");
    const char *third_names[] = {"éclair"};
    assert_order(third, third_names, 1);
    assert(strstr(third, "\"next_offset\":null"));
    free(third);

    char *past_end = get_page("/api/import/files?path=&offset=100&limit=3");
    assert(strstr(past_end, "\"entries\":[]"));
    assert(strstr(past_end, "\"next_offset\":null"));
    free(past_end);

    /* Cross the fixed 1024-entry selection window and verify pages join
     * without gaps or duplicates. */
    assert(chdir("music") == 0);
    create_generated_files(1205);
    assert(chdir("..") == 0);
    char *deep_first = get_page("/api/import/files?path=many&offset=1000&limit=100");
    assert_generated_page(deep_first, 1000, 100);
    assert(strstr(deep_first, "\"total\":1205"));
    assert(strstr(deep_first, "\"next_offset\":1100"));
    free(deep_first);
    char *deep_second = get_page("/api/import/files?path=many&offset=1100&limit=100");
    assert_generated_page(deep_second, 1100, 100);
    assert(strstr(deep_second, "\"next_offset\":1200"));
    free(deep_second);
    char max_offset[160];
    snprintf(max_offset, sizeof(max_offset), "/api/import/files?path=many&offset=%lu&limit=100", ULONG_MAX);
    char *overflow_page = get_page(max_offset);
    assert(strstr(overflow_page, "\"entries\":[]"));
    assert(strstr(overflow_page, "\"next_offset\":null"));
    free(overflow_page);
    remote_import_files_stop();
    puts("remote import file ordering: ok");
    return 0;
}
