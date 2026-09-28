#include "html_blocks.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

static void expect(int condition, const char * what) {
    if (condition) return;
    fprintf(stderr, "html blocks: %s\n", what);
    failures++;
}

static double elapsed_sec(struct timespec t0, struct timespec t1) {
    return (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
}

int main(void) {
    /* Self-closing script/style/head must not swallow the rest of the chapter */
    {
        const char in[] = "<html><head/><body><script src=\"x.js\"/><style type='t'/>"
                          "<p>Visible</p><script>x</script><p>Tail</p></body></html>";
        html_blocks out;
        html_status st = html_to_blocks(in, sizeof(in) - 1, &out);
        expect(st == HTML_OK, "self-closing: HTML_OK");
        expect(out.count == 2, "self-closing: two blocks");
        if (out.count == 2) {
            expect(strcmp(out.blocks[0].text, "Visible") == 0, "self-closing: first block text");
            expect(strcmp(out.blocks[1].text, "Tail") == 0, "self-closing: second block text");
        }
        if (st == HTML_OK) html_blocks_free(&out);
    }

    /* Comparisons inside script/style are raw text, not nested tags. */
    {
        const char in[] = "<script>if(a<b) f();</script><p>Visible</p>";
        html_blocks out;
        html_status st = html_to_blocks(in, sizeof(in) - 1, &out);
        expect(st == HTML_OK, "script compare: HTML_OK");
        expect(out.count == 1, "script compare: one block");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].kind, "p") == 0, "script compare: kind p");
            expect(strcmp(out.blocks[0].text, "Visible") == 0, "script compare: text");
        }
        if (st == HTML_OK) html_blocks_free(&out);
    }
    {
        const char in[] = "<style>a<b {color:red}</style><p>Visible</p>";
        html_blocks out;
        html_status st = html_to_blocks(in, sizeof(in) - 1, &out);
        expect(st == HTML_OK, "style compare: HTML_OK");
        expect(out.count == 1, "style compare: one block");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "Visible") == 0, "style compare: text");
        }
        if (st == HTML_OK) html_blocks_free(&out);
    }
    {
        const char in[] = "<head><script>if(a<b) f();</SCRIPT></head><p>Visible</p>";
        html_blocks out;
        html_status st = html_to_blocks(in, sizeof(in) - 1, &out);
        expect(st == HTML_OK, "head script compare: HTML_OK");
        expect(out.count == 1, "head script compare: one block");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "Visible") == 0, "head script compare: text");
        }
        if (st == HTML_OK) html_blocks_free(&out);
    }
    /* XHTML CDATA hides end tags. A raw </script> outside CDATA still ends the body. */
    {
        const char in[] = "<script><![CDATA[var s = \"</script><head>\";]]></script><p>Visible</p>";
        html_blocks out;
        html_status st = html_to_blocks(in, sizeof(in) - 1, &out);
        expect(st == HTML_OK, "script cdata: HTML_OK");
        expect(out.count == 1, "script cdata: one block");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "Visible") == 0, "script cdata: text");
        }
        if (st == HTML_OK) html_blocks_free(&out);
    }
    {
        const char in[] = "<style><![CDATA[a</style>b]]></style><p>Visible</p>";
        html_blocks out;
        html_status st = html_to_blocks(in, sizeof(in) - 1, &out);
        expect(st == HTML_OK, "style cdata: HTML_OK");
        expect(out.count == 1, "style cdata: one block");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "Visible") == 0, "style cdata: text");
        }
        if (st == HTML_OK) html_blocks_free(&out);
    }
    {
        const char in[] = "<head><script><![CDATA[\"</script><head>\"]]></script></head><p>Visible</p>";
        html_blocks out;
        html_status st = html_to_blocks(in, sizeof(in) - 1, &out);
        expect(st == HTML_OK, "head script cdata: HTML_OK");
        expect(out.count == 1, "head script cdata: one block");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "Visible") == 0, "head script cdata: text");
        }
        if (st == HTML_OK) html_blocks_free(&out);
    }
    {
        const char in[] = "<script>var s=\"</script><p>Visible</p>";
        html_blocks out;
        html_status st = html_to_blocks(in, sizeof(in) - 1, &out);
        expect(st == HTML_OK, "script end outside cdata: HTML_OK");
        expect(out.count == 1, "script end outside cdata: one block");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "Visible") == 0, "script end outside cdata: text");
        }
        if (st == HTML_OK) html_blocks_free(&out);
    }

    /* 1. Chapter test: yields exactly 10 blocks in order, memcmp UTF-8 bytes */
    {
        const char input1[] =
            "<!DOCTYPE html><html><head><title>Ignored</title><style>p{color:red}</style></head><body>\n"
            "<!-- secret -->\n"
            "<h1>Chapter&nbsp;1</h1>\n"
            "<p>Tom &amp; Jerry&mdash;really&#8230;</p>\n"
            "<p>Line<br/>break</p>\n"
            "<div>Div text</div>\n"
            "<blockquote>Quote</blockquote>\n"
            "<ul><li>Item</li></ul>\n"
            "<img src=\"img/a&amp;b.png\" alt=\"A picture\"/>\n"
            "<hr/>\n"
            "<script>alert(1)</script>\n"
            "<p>After</p>\n"
            "<p><![CDATA[raw <tag>]]></p>\n"
            "<?pi ignore?>\n"
            "</body></html>";

        html_blocks out;
        html_status st = html_to_blocks(input1, sizeof(input1) - 1, &out);
        expect(st == HTML_OK, "case 1: html_to_blocks returned HTML_OK");
        expect(!out.truncated, "case 1: truncated is false");
        expect(out.count == 10, "case 1: exactly 10 blocks");

        if (out.count >= 10) {
            /* 0: h level 1 text "Chapter 1" */
            expect(strcmp(out.blocks[0].kind, "h") == 0, "case 1: block 0 kind is h");
            expect(out.blocks[0].level == 1, "case 1: block 0 level is 1");
            expect(memcmp(out.blocks[0].text, "Chapter 1", 9) == 0 && out.blocks[0].text[9] == '\0',
                   "case 1: block 0 text is 'Chapter 1'");
            expect(out.blocks[0].alt == NULL, "case 1: block 0 alt is NULL");

            /* 1: p text Tom & Jerry + E2 80 94 + really + E2 80 A6 */
            const uint8_t expected_b1[] = {
                'T','o','m',' ','&',' ','J','e','r','r','y',
                0xE2, 0x80, 0x94,
                'r','e','a','l','l','y',
                0xE2, 0x80, 0xA6
            };
            expect(strcmp(out.blocks[1].kind, "p") == 0, "case 1: block 1 kind is p");
            expect(out.blocks[1].level == 0, "case 1: block 1 level is 0");
            expect(strlen(out.blocks[1].text) == sizeof(expected_b1), "case 1: block 1 text len match");
            expect(memcmp(out.blocks[1].text, expected_b1, sizeof(expected_b1)) == 0,
                   "case 1: block 1 UTF-8 bytes match");
            expect(out.blocks[1].alt == NULL, "case 1: block 1 alt is NULL");

            /* 2: p text "Line\nbreak" */
            expect(strcmp(out.blocks[2].kind, "p") == 0, "case 1: block 2 kind is p");
            expect(strcmp(out.blocks[2].text, "Line\nbreak") == 0, "case 1: block 2 text is Line\\nbreak");

            /* 3: p text "Div text" */
            expect(strcmp(out.blocks[3].kind, "p") == 0, "case 1: block 3 kind is p");
            expect(strcmp(out.blocks[3].text, "Div text") == 0, "case 1: block 3 text is Div text");

            /* 4: p text "Quote" */
            expect(strcmp(out.blocks[4].kind, "p") == 0, "case 1: block 4 kind is p");
            expect(strcmp(out.blocks[4].text, "Quote") == 0, "case 1: block 4 text is Quote");

            /* 5: p text "Item" */
            expect(strcmp(out.blocks[5].kind, "p") == 0, "case 1: block 5 kind is p");
            expect(strcmp(out.blocks[5].text, "Item") == 0, "case 1: block 5 text is Item");

            /* 6: img text "img/a&b.png" alt "A picture" */
            expect(strcmp(out.blocks[6].kind, "img") == 0, "case 1: block 6 kind is img");
            expect(out.blocks[6].level == 0, "case 1: block 6 level is 0");
            expect(strcmp(out.blocks[6].text, "img/a&b.png") == 0, "case 1: block 6 text is img/a&b.png");
            expect(out.blocks[6].alt != NULL && strcmp(out.blocks[6].alt, "A picture") == 0,
                   "case 1: block 6 alt is A picture");

            /* 7: hr text "" alt NULL level 0 */
            expect(strcmp(out.blocks[7].kind, "hr") == 0, "case 1: block 7 kind is hr");
            expect(out.blocks[7].level == 0, "case 1: block 7 level is 0");
            expect(strcmp(out.blocks[7].text, "") == 0, "case 1: block 7 text is empty");
            expect(out.blocks[7].alt == NULL, "case 1: block 7 alt is NULL");

            /* 8: p text "After" */
            expect(strcmp(out.blocks[8].kind, "p") == 0, "case 1: block 8 kind is p");
            expect(strcmp(out.blocks[8].text, "After") == 0, "case 1: block 8 text is After");

            /* 9: p text "raw <tag>" */
            expect(strcmp(out.blocks[9].kind, "p") == 0, "case 1: block 9 kind is p");
            expect(strcmp(out.blocks[9].text, "raw <tag>") == 0, "case 1: block 9 text is raw <tag>");
        }
        html_blocks_free(&out);
    }

    /* 2. Each named entity above, plus &#x2014;, in its own <p> */
    {
        const char entities_input[] =
            "<p>&amp;</p>"
            "<p>&lt;</p>"
            "<p>&gt;</p>"
            "<p>&quot;</p>"
            "<p>&apos;</p>"
            "<p>a&nbsp;b</p>"
            "<p>&mdash;</p>"
            "<p>&ndash;</p>"
            "<p>&hellip;</p>"
            "<p>&lsquo;</p>"
            "<p>&rsquo;</p>"
            "<p>&ldquo;</p>"
            "<p>&rdquo;</p>"
            "<p>&#x2014;</p>";

        html_blocks out;
        html_status st = html_to_blocks(entities_input, strlen(entities_input), &out);
        expect(st == HTML_OK, "case 2: html_to_blocks returned HTML_OK");
        expect(out.count == 14, "case 2: exactly 14 blocks");

        if (out.count >= 14) {
            expect(memcmp(out.blocks[0].text, "&", 2) == 0, "case 2: &amp;");
            expect(memcmp(out.blocks[1].text, "<", 2) == 0, "case 2: &lt;");
            expect(memcmp(out.blocks[2].text, ">", 2) == 0, "case 2: &gt;");
            expect(memcmp(out.blocks[3].text, "\"", 2) == 0, "case 2: &quot;");
            expect(memcmp(out.blocks[4].text, "'", 2) == 0, "case 2: &apos;");
            expect(memcmp(out.blocks[5].text, "a b", 4) == 0, "case 2: a&nbsp;b");

            const uint8_t mdash_bytes[] = { 0xE2, 0x80, 0x94, 0x00 };
            const uint8_t ndash_bytes[] = { 0xE2, 0x80, 0x93, 0x00 };
            const uint8_t hellip_bytes[] = { 0xE2, 0x80, 0xA6, 0x00 };
            const uint8_t lsquo_bytes[] = { 0xE2, 0x80, 0x98, 0x00 };
            const uint8_t rsquo_bytes[] = { 0xE2, 0x80, 0x99, 0x00 };
            const uint8_t ldquo_bytes[] = { 0xE2, 0x80, 0x9C, 0x00 };
            const uint8_t rdquo_bytes[] = { 0xE2, 0x80, 0x9D, 0x00 };

            expect(memcmp(out.blocks[6].text, mdash_bytes, 4) == 0, "case 2: &mdash;");
            expect(memcmp(out.blocks[7].text, ndash_bytes, 4) == 0, "case 2: &ndash;");
            expect(memcmp(out.blocks[8].text, hellip_bytes, 4) == 0, "case 2: &hellip;");
            expect(memcmp(out.blocks[9].text, lsquo_bytes, 4) == 0, "case 2: &lsquo;");
            expect(memcmp(out.blocks[10].text, rsquo_bytes, 4) == 0, "case 2: &rsquo;");
            expect(memcmp(out.blocks[11].text, ldquo_bytes, 4) == 0, "case 2: &ldquo;");
            expect(memcmp(out.blocks[12].text, rdquo_bytes, 4) == 0, "case 2: &rdquo;");
            expect(memcmp(out.blocks[13].text, mdash_bytes, 4) == 0, "case 2: &#x2014;");
        }
        html_blocks_free(&out);
    }

    /* 3. <P>Hi</P> -> one p Hi; <p>hello<br>world</p> -> hello\nworld;
     *    <p>  hello   \n  world  </p> -> hello world; <p></p> and <div>   </div> emit nothing */
    {
        html_blocks out;
        html_status st;

        st = html_to_blocks("<P>Hi</P>", 9, &out);
        expect(st == HTML_OK && out.count == 1, "case 3: <P>Hi</P> count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].kind, "p") == 0, "case 3: kind p");
            expect(strcmp(out.blocks[0].text, "Hi") == 0, "case 3: text Hi");
        }
        html_blocks_free(&out);

        st = html_to_blocks("<p>hello<br>world</p>", 21, &out);
        expect(st == HTML_OK && out.count == 1, "case 3: <br> count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "hello\nworld") == 0, "case 3: text hello\\nworld");
        }
        html_blocks_free(&out);

        st = html_to_blocks("<p>hello<br>\nworld</p>", 22, &out);
        expect(st == HTML_OK && out.count == 1, "case 3: <br>\\n count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "hello\nworld") == 0, "case 3: text hello\\nworld after br newline");
        }
        html_blocks_free(&out);

        const char spaces_in[] = "<p>  hello   \n  world  </p>";
        st = html_to_blocks(spaces_in, strlen(spaces_in), &out);
        expect(st == HTML_OK && out.count == 1, "case 3: spaces collapsed count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "hello world") == 0, "case 3: text hello world");
        }
        html_blocks_free(&out);

        st = html_to_blocks("<p></p>", 7, &out);
        expect(st == HTML_OK && out.count == 0 && out.blocks == NULL, "case 3: <p></p> emits nothing");
        html_blocks_free(&out);

        st = html_to_blocks("<div>   </div>", 14, &out);
        expect(st == HTML_OK && out.count == 0 && out.blocks == NULL, "case 3: <div>   </div> emits nothing");
        html_blocks_free(&out);
    }

    /* 4. <p>1 < 2</p> -> 1 < 2; <p>&amp</p> -> &amp (no semicolon, literal) */
    {
        html_blocks out;
        html_status st;

        st = html_to_blocks("<p>1 < 2</p>", 12, &out);
        expect(st == HTML_OK && out.count == 1, "case 4: <p>1 < 2</p> count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "1 < 2") == 0, "case 4: text 1 < 2");
        }
        html_blocks_free(&out);

        st = html_to_blocks("<p>&amp</p>", 11, &out);
        expect(st == HTML_OK && out.count == 1, "case 4: <p>&amp</p> count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "&amp") == 0, "case 4: text &amp");
        }
        html_blocks_free(&out);
    }

    /* 5. &#0;, &#xd800;, &#x110000; each become ? */
    {
        html_blocks out;
        html_status st;

        st = html_to_blocks("<p>&#0;</p>", 11, &out);
        expect(st == HTML_OK && out.count == 1, "case 5: &#0; count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "?") == 0, "case 5: &#0; becomes ?");
        }
        html_blocks_free(&out);

        st = html_to_blocks("<p>&#xd800;</p>", 15, &out);
        expect(st == HTML_OK && out.count == 1, "case 5: &#xd800; count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "?") == 0, "case 5: &#xd800; becomes ?");
        }
        html_blocks_free(&out);

        st = html_to_blocks("<p>&#x110000;</p>", 17, &out);
        expect(st == HTML_OK && out.count == 1, "case 5: &#x110000; count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "?") == 0, "case 5: &#x110000; becomes ?");
        }
        html_blocks_free(&out);
    }

    /* 6. Leading BOM EF BB BF before <p>hi</p> -> hi */
    {
        const char bom_in[] = "\xEF\xBB\xBF<p>hi</p>";
        html_blocks out;
        html_status st = html_to_blocks(bom_in, sizeof(bom_in) - 1, &out);
        expect(st == HTML_OK && out.count == 1, "case 6: BOM count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "hi") == 0, "case 6: text is hi");
        }
        html_blocks_free(&out);
    }

    /* 7. Input byte FF inside <p> becomes ?; NUL byte inside <p> becomes ? */
    {
        const char ff_in[] = "<p>a\xFF" "b</p>";
        html_blocks out;
        html_status st = html_to_blocks(ff_in, sizeof(ff_in) - 1, &out);
        expect(st == HTML_OK && out.count == 1, "case 7: FF count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "a?b") == 0, "case 7: FF becomes ?");
        }
        html_blocks_free(&out);

        const char nul_in[] = "<p>a\0b</p>";
        st = html_to_blocks(nul_in, 8, &out);
        expect(st == HTML_OK && out.count == 1, "case 7: NUL count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "a?b") == 0, "case 7: NUL becomes ?");
        }
        html_blocks_free(&out);
    }

    /* 8. Unterminated <p>hello<tag -> one block hello; unterminated <!-- hides the rest;
     *    unterminated attribute quote does not loop forever */
    {
        html_blocks out;
        html_status st;

        st = html_to_blocks("<p>hello<tag", 12, &out);
        expect(st == HTML_OK && out.count == 1, "case 8: unterminated tag count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "hello") == 0, "case 8: text is hello");
        }
        html_blocks_free(&out);

        const char unclosed_comment[] = "<p>hello</p><!-- secret <p>world</p>";
        st = html_to_blocks(unclosed_comment, strlen(unclosed_comment), &out);
        expect(st == HTML_OK && out.count == 1, "case 8: unclosed comment count 1");
        if (out.count == 1) {
            expect(strcmp(out.blocks[0].text, "hello") == 0, "case 8: unclosed comment hides rest");
        }
        html_blocks_free(&out);

        const char unclosed_quote[] = "<p><span attr=\"unterminated value without end quote";
        st = html_to_blocks(unclosed_quote, strlen(unclosed_quote), &out);
        expect(st == HTML_OK, "case 8: unclosed quote terminates cleanly");
        html_blocks_free(&out);
    }

    /* 9. 300 <div> wrappers around x (open, then x, then close) -> one p block x */
    {
        size_t cap = 300 * 5 + 1 + 300 * 6 + 1;
        char * buf = (char *) malloc(cap);
        expect(buf != NULL, "case 9: malloc buffer");
        if (buf) {
            char * p = buf;
            for (int i = 0; i < 300; i++) {
                memcpy(p, "<div>", 5);
                p += 5;
            }
            *p++ = 'x';
            for (int i = 0; i < 300; i++) {
                memcpy(p, "</div>", 6);
                p += 6;
            }
            *p = '\0';
            size_t buflen = (size_t)(p - buf);

            html_blocks out;
            html_status st = html_to_blocks(buf, buflen, &out);
            expect(st == HTML_OK, "case 9: html_to_blocks returned HTML_OK");
            expect(out.count == 1, "case 9: count is 1");
            if (out.count == 1) {
                expect(strcmp(out.blocks[0].kind, "p") == 0, "case 9: kind is p");
                expect(strcmp(out.blocks[0].text, "x") == 0, "case 9: text is x");
            }
            html_blocks_free(&out);
            free(buf);
        }
    }

    /* 10. 2001 copies of <p>a</p> -> count 2000, truncated true, every emitted text is a */
    {
        size_t buflen = 2001 * 8;
        char * buf = (char *) malloc(buflen + 1);
        expect(buf != NULL, "case 10: malloc buffer");
        if (buf) {
            for (size_t i = 0; i < 2001; i++) {
                memcpy(buf + i * 8, "<p>a</p>", 8);
            }
            buf[buflen] = '\0';

            html_blocks out;
            html_status st = html_to_blocks(buf, buflen, &out);
            expect(st == HTML_OK, "case 10: html_to_blocks returned HTML_OK");
            expect(out.count == 2000, "case 10: count is exactly 2000");
            expect(out.truncated, "case 10: truncated is true");

            bool all_a = true;
            for (size_t i = 0; i < out.count; i++) {
                if (strcmp(out.blocks[i].text, "a") != 0) {
                    all_a = false;
                    break;
                }
            }
            expect(all_a, "case 10: every emitted text is 'a'");
            html_blocks_free(&out);
            free(buf);
        }
    }

    /* 11. A <p> whose text is 300000 bytes of a (input under 512 KiB) ->
     *     HTML_OK, text length HTML_MAX_TEXT_BYTES, truncated true */
    {
        size_t buflen = 3 + 300000 + 4;
        char * buf = (char *) malloc(buflen + 1);
        expect(buf != NULL, "case 11: malloc buffer");
        if (buf) {
            memcpy(buf, "<p>", 3);
            memset(buf + 3, 'a', 300000);
            memcpy(buf + 300003, "</p>", 4);
            buf[buflen] = '\0';

            html_blocks out;
            html_status st = html_to_blocks(buf, buflen, &out);
            expect(st == HTML_OK, "case 11: html_to_blocks returned HTML_OK");
            expect(out.truncated, "case 11: truncated is true");
            expect(out.count == 1, "case 11: count is 1");
            if (out.count == 1) {
                expect(strlen(out.blocks[0].text) == HTML_MAX_TEXT_BYTES,
                       "case 11: text length is exactly HTML_MAX_TEXT_BYTES");
            }
            html_blocks_free(&out);
            free(buf);
        }
    }

    /* 12. len == HTML_MAX_INPUT_BYTES + 1 -> input_too_large, count 0, even if pointer is non-NULL.
     *     Pass 1-byte buffer with lying length without reading past check */
    {
        char dummy = 'x';
        html_blocks out;
        html_status st = html_to_blocks(&dummy, HTML_MAX_INPUT_BYTES + 1, &out);
        expect(st == HTML_ERR_TOO_LARGE, "case 12: returned HTML_ERR_TOO_LARGE");
        expect(strcmp(html_status_reason(st), "input_too_large") == 0,
               "case 12: reason is input_too_large");
        expect(out.count == 0, "case 12: count is 0");
        expect(out.blocks == NULL, "case 12: blocks is NULL");
        html_blocks_free(&out);
    }

    /* 13. NULL out, and NULL data with positive len, return bad_args and do not crash.
     *     html_blocks_free(NULL) does not crash */
    {
        html_blocks out;
        html_status st = html_to_blocks("foo", 3, NULL);
        expect(st == HTML_ERR_ARGS, "case 13: NULL out returns HTML_ERR_ARGS");
        expect(strcmp(html_status_reason(st), "bad_args") == 0, "case 13: reason is bad_args");

        st = html_to_blocks(NULL, 10, &out);
        expect(st == HTML_ERR_ARGS, "case 13: NULL data with pos len returns HTML_ERR_ARGS");
        expect(out.count == 0, "case 13: out count zeroed");
        expect(out.blocks == NULL, "case 13: out blocks zeroed");

        html_blocks_free(NULL);

        st = html_to_blocks(NULL, 0, &out);
        expect(st == HTML_OK, "case 13: NULL data with len 0 returns HTML_OK");
        expect(out.count == 0 && out.blocks == NULL && !out.truncated,
               "case 13: NULL data with len 0 is empty success");
        html_blocks_free(&out);
    }

    /* 14. Timing using clock_gettime(CLOCK_MONOTONIC): each returns in under 1.0 second */
    {
        struct timespec t0, t1;

        /* Subcase A: 400000 bytes of 'a' in a single <p>...</p> */
        {
            size_t sz = 3 + 400000 + 4;
            char * buf = (char *) malloc(sz);
            expect(buf != NULL, "case 14a: malloc");
            if (buf) {
                memcpy(buf, "<p>", 3);
                memset(buf + 3, 'a', 400000);
                memcpy(buf + 400003, "</p>", 4);

                html_blocks out;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                html_status st = html_to_blocks(buf, sz, &out);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double el = elapsed_sec(t0, t1);
                expect(st == HTML_OK, "case 14a: HTML_OK");
                expect(el < 1.0, "case 14a: 400000 bytes 'a' under 1.0s");
                html_blocks_free(&out);
                free(buf);
            }
        }

        /* Subcase B: 400000 bytes of 'a' inside huge unquoted attribute: <p><span + 400000 'x' + >hi</p> */
        {
            size_t sz = 9 + 400000 + 7;
            char * buf = (char *) malloc(sz);
            expect(buf != NULL, "case 14b: malloc");
            if (buf) {
                memcpy(buf, "<p><span ", 9);
                memset(buf + 9, 'x', 400000);
                memcpy(buf + 400009, ">hi</p>", 7);

                html_blocks out;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                html_status st = html_to_blocks(buf, sz, &out);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double el = elapsed_sec(t0, t1);
                expect(st == HTML_OK, "case 14b: HTML_OK");
                expect(el < 1.0, "case 14b: huge unquoted attribute under 1.0s");
                expect(out.count == 1, "case 14b: count 1");
                if (out.count == 1) {
                    expect(strcmp(out.blocks[0].text, "hi") == 0, "case 14b: text is 'hi'");
                }
                html_blocks_free(&out);
                free(buf);
            }
        }

        /* Subcase C: 100000 copies of &amp; inside one <p> */
        {
            size_t sz = 3 + 100000 * 5 + 4;
            char * buf = (char *) malloc(sz);
            expect(buf != NULL, "case 14c: malloc");
            if (buf) {
                memcpy(buf, "<p>", 3);
                for (size_t i = 0; i < 100000; i++) {
                    memcpy(buf + 3 + i * 5, "&amp;", 5);
                }
                memcpy(buf + 3 + 500000, "</p>", 4);

                html_blocks out;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                html_status st = html_to_blocks(buf, sz, &out);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double el = elapsed_sec(t0, t1);
                expect(st == HTML_OK, "case 14c: HTML_OK");
                expect(el < 1.0, "case 14c: 100000 &amp; under 1.0s");
                html_blocks_free(&out);
                free(buf);
            }
        }
    }

    if (failures) {
        fprintf(stderr, "html blocks: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("html blocks: PASS");
    return 0;
}
