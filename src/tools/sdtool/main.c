#include "sdtool.h"
#include "json_out.h"
#include <stdio.h>
#include <string.h>

static const char usage[] =
    "usage: compas-sdtool info <card-root>\n"
    "       compas-sdtool scan <card-root> [--full]\n"
    "       compas-sdtool art <card-root> [--board r1|r3proii|r3ii_2025]\n";

static int usage_error(const char *message) {
    json_error("usage", message);
    fputs(usage, stderr);
    return SDTOOL_USAGE;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage_error("a command and a card root are required");
    const char *command = argv[1];
    sdtool_options_t options = { .card_root = argv[2] };
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0 && strcmp(command, "scan") == 0) {
            options.full = true;
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
