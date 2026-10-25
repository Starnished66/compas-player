#include "sdtool.h"
#include "json_out.h"
#include "parse_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char usage[] =
    "usage: compas-sdtool info <card-root>\n"
    "       compas-sdtool scan <card-root> [--full] [--mtime-offset SECONDS]\n"
    "       compas-sdtool art <card-root> [--board r1|r3proii|r3ii_2025] [--mtime-offset SECONDS]\n";

static int usage_error(const char *message) {
    json_error("usage", message);
    fputs(usage, stderr);
    return SDTOOL_USAGE;
}

int main(int argc, char **argv) {
    /* Internal: started by the scan to parse tags in a process of its own. */
    if (argc == 2 && strcmp(argv[1], "parse-worker") == 0) return sdtool_parse_worker();

    if (argc < 3) return usage_error("a command and a card root are required");
    const char *command = argv[1];
    sdtool_options_t options = { .card_root = argv[2] };
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0 && strcmp(command, "scan") == 0) {
            options.full = true;
        } else if (strcmp(argv[i], "--mtime-offset") == 0 && strcmp(command, "info") != 0 && i + 1 < argc) {
            char *end;
            long seconds = strtol(argv[++i], &end, 10);
            if (*end || end == argv[i]) return usage_error("--mtime-offset needs a number of seconds");
            sdtool_mtime_set_manual(seconds);
        } else if (strcmp(argv[i], "--board") == 0 && strcmp(command, "art") == 0 && i + 1 < argc) {
            options.board = argv[++i];
        } else {
            return usage_error("unknown or misplaced option");
        }
    }
    if (!sdtool_root_set(options.card_root)) return usage_error("the card root is not a usable path");

    if (strcmp(command, "info") == 0) return sdtool_cmd_info(&options);
    if (strcmp(command, "scan") == 0) return sdtool_cmd_scan(&options);
    if (strcmp(command, "art") == 0) return sdtool_cmd_art(&options);
    return usage_error("unknown command");
}
