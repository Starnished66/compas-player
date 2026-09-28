#ifndef HTML_BLOCKS_H
#define HTML_BLOCKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTML_MAX_INPUT_BYTES (512u * 1024u)
#define HTML_MAX_BLOCKS      2000u
#define HTML_MAX_TEXT_BYTES  (256u * 1024u)

typedef enum {
    HTML_OK = 0,
    HTML_ERR_ARGS,
    HTML_ERR_TOO_LARGE,
    HTML_ERR_NOMEM
} html_status;

typedef struct {
    const char * kind; /* static "p", "h", "img", or "hr"; never NULL */
    int level;         /* 1..6 for kind "h", otherwise 0 */
    char * text;       /* malloc'd NUL-terminated; never NULL; "" if empty */
    char * alt;        /* malloc'd NUL-terminated for kind "img"; NULL otherwise */
} html_block;

typedef struct {
    html_block * blocks; /* NULL when count == 0 */
    size_t count;
    bool truncated;
} html_blocks;

const char * html_status_reason(html_status status);
html_status html_to_blocks(const void * data, size_t len, html_blocks * out);
void html_blocks_free(html_blocks * out);

#ifdef __cplusplus
}
#endif

#endif /* HTML_BLOCKS_H */
