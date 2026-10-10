/* Loopback-only slow HTTP fixture for device progress UI validation.
 * Run from persistent SD storage; stop the server and remove its files after QA. */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool send_all(int fd, const void * data, size_t size) {
    const char * p = data;
    while (size) {
        ssize_t n = send(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= (size_t) n;
    }
    return true;
}

static void serve(int fd) {
    char request[4096];
    size_t used = 0;
    while (used < sizeof(request) - 1) {
        ssize_t n = recv(fd, request + used, sizeof(request) - used - 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        used += (size_t) n; request[used] = '\0';
        if (strstr(request, "\r\n\r\n")) break;
    }
    bool known = strncmp(request, "GET /known ", 11) == 0;
    bool unknown = strncmp(request, "GET /unknown ", 13) == 0;
    if (!known && !unknown) {
        const char response[] = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1); return;
    }
    const char * header = known
        ? "HTTP/1.0 200 OK\r\nContent-Length: 262144\r\nContent-Type: application/octet-stream\r\n\r\n"
        : "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: application/octet-stream\r\n\r\n";
    if (!send_all(fd, header, strlen(header))) return;
    char data[4096]; memset(data, 'x', sizeof(data));
    for (int i = 0; i < 64; i++) {
        if (unknown && !send_all(fd, "1000\r\n", 6)) return;
        if (!send_all(fd, data, sizeof(data))) return;
        if (unknown && !send_all(fd, "\r\n", 2)) return;
        usleep(500000);
    }
    if (unknown) send_all(fd, "0\r\n\r\n", 5);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {perror("socket"); return 1;}
    int reuse = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_port = htons(18765);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *) &address, sizeof(address)) || listen(server, 4)) {
        perror("bind/listen"); close(server); return 1;
    }
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) {if (errno == EINTR) continue; perror("accept"); break;}
        pid_t child = fork();
        if (child == 0) {close(server); serve(fd); close(fd); _exit(0);}
        if (child < 0) perror("fork");
        close(fd);
    }
    close(server); return 1;
}
