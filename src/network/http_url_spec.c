#include "http_url.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void parse_ok(const char * url, bool expected_https, const char * expected_host,
                     const char * expected_port, const char * expected_path) {
    bool https = !expected_https;
    char host[64] = "unchanged";
    char port[16] = "unchanged";
    char path[128] = "unchanged";
    assert(http_conn_parse_url(url, &https, host, sizeof(host), port, sizeof(port),
                               path, sizeof(path)));
    assert(https == expected_https);
    assert(strcmp(host, expected_host) == 0);
    assert(strcmp(port, expected_port) == 0);
    assert(strcmp(path, expected_path) == 0);
}

int main(void) {
    parse_ok("HTTP://Example.COM", false, "Example.COM", "80", "/");
    parse_ok("https://example.test:8443/a?q=1#ignored", true,
             "example.test", "8443", "/a?q=1");
    parse_ok("https://example.test/#fragment", true, "example.test", "443", "/");

    bool https = false;
    char host[8], port[8], path[8];
    assert(!http_conn_parse_url("ftp://example.test/", &https, host, sizeof(host),
                                port, sizeof(port), path, sizeof(path)));
    assert(!http_conn_parse_url("http:/example.test/", &https, host, sizeof(host),
                                port, sizeof(port), path, sizeof(path)));
    assert(!http_conn_parse_url("http:///path", &https, host, sizeof(host),
                                port, sizeof(port), path, sizeof(path)));
    assert(!http_conn_parse_url("http://toolong.example/", &https, host, 4,
                                port, sizeof(port), path, sizeof(path)));
    assert(!http_conn_parse_url("http://host:12345/", &https, host, sizeof(host),
                                port, 4, path, sizeof(path)));

    /* Preserve the existing snprintf truncation behavior for path/default port. */
    parse_ok("http://host/long/path", false, "host", "80", "/long/path");
    char small_path[5];
    assert(http_conn_parse_url("http://host/abcdef", &https, host, sizeof(host),
                               port, sizeof(port), small_path, sizeof(small_path)));
    assert(strcmp(small_path, "/abc") == 0);
    char tiny_port[2];
    assert(http_conn_parse_url("http://host", &https, host, sizeof(host),
                               tiny_port, sizeof(tiny_port), path, sizeof(path)));
    assert(strcmp(tiny_port, "8") == 0);
    puts("http_url_spec: ok");
    return 0;
}
