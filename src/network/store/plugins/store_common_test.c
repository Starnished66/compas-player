#include "store_common.h"
#include <assert.h>
#include <stdio.h>

const char *i18n_tr(const char *text) { return text; }

/* The network half of store_common.c is not exercised here. */
void http_response_free(http_response_t *response) { (void) response; }
bool http_request_ex(const http_request_t *request, http_cancel_token_t *cancel, http_response_t *response) {
    (void) request; (void) cancel; (void) response;
    return false;
}
bool http_get_to_file_redirects(const char *url, bool verify_tls, const char *dest_path, size_t max_body_size,
                                http_progress_cb_t progress_cb, void *progress_user_data,
                                uint32_t connect_timeout_ms, uint32_t read_timeout_ms,
                                http_cancel_token_t *cancel, int max_redirects, int *out_status) {
    (void) url; (void) verify_tls; (void) dest_path; (void) max_body_size; (void) progress_cb;
    (void) progress_user_data; (void) connect_timeout_ms; (void) read_timeout_ms; (void) cancel;
    (void) max_redirects; (void) out_status;
    return false;
}

static void valid(const char *dest) { assert(store_dest_path_valid(dest)); }
static void invalid(const char *dest) { assert(!store_dest_path_valid(dest)); }

int main(void) {
    valid(".plugins/weather.lua");
    valid(".plugins/data/my file.txt");
    valid("a.b/c-d_e");
    invalid("");
    invalid("/abs");
    invalid("../x");
    invalid("a//b");
    invalid("a/./b");
    /* vfat and Windows strip a trailing dot or space from a name, so these
     * would land on protected paths. */
    invalid(".compas./compas_player");
    invalid(".plugins./.store_installed");
    invalid("r1.upt.");
    invalid(".plugins/x.lua ");
    invalid(".plugins /x.lua");
    invalid(".plugins/dir./x.lua");
    invalid("a/b.");
    puts("store destination paths: PASS");
    return 0;
}
