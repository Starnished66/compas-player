#include "../cover_flow_azmap.h"
#include <assert.h>
#include <stdio.h>

static void build(const char * const * titles, int n, int * jump, bool * has) {
    cover_flow_item_t items[32];
    assert(n <= 32);
    for (int i = 0; i < n; i++) items[i] = (cover_flow_item_t) { titles[i], "", 0 };
    cover_flow_az_build(items, n, jump, has);
}

/* The letter of one title, through the table: the only letter that has an item. */
static int letter_of(const char * title) {
    int jump[27];
    bool has[27];
    build(&title, 1, jump, has);
    int found = -1;
    for (int i = 0; i < 27; i++)
        if (has[i]) { assert(found < 0); found = i; }
    return found;
}

static void check_letters(void) {
    assert(letter_of("Abba") == 0 && letter_of("abba") == 0);
    assert(letter_of("zebra") == 25 && letter_of("Zed") == 25);
    assert(letter_of("1999") == 26 && letter_of("(What)") == 26);
    assert(letter_of("") == 26 && letter_of(NULL) == 26);
    assert(letter_of("\xc3\x89" "clair") == 26); /* a non-ASCII first byte is '#' */
}

static void check_sorted(void) {
    const char * t[] = { "10cc", "Abba", "ac/dc", "Beck", "Dylan", "Eagles", "Zappa" };
    int jump[27];
    bool has[27];
    build(t, 7, jump, has);
    assert(jump[26] == 0 && has[26]);
    assert(jump[0] == 1 && has[0]);   /* A: first of two */
    assert(jump[1] == 3 && has[1]);
    assert(jump[2] == 4 && !has[2]);  /* C has none: forward to D */
    assert(jump[3] == 4 && has[3]);
    assert(jump[4] == 5 && has[4]);
    assert(jump[5] == 6 && !has[5]);  /* F..Y forward to Z */
    assert(jump[24] == 6 && jump[25] == 6 && has[25]);
}

static void check_unsorted(void) {
    const char * t[] = { "Zappa", "beck", "Abba", "", "Beyonce", "x", "abc" };
    int jump[27];
    bool has[27];
    build(t, 7, jump, has);
    assert(jump[0] == 2);  /* lowest index wins, not the sorted first */
    assert(jump[1] == 1);
    assert(jump[26] == 3 && has[26]); /* the empty title is '#' */
    assert(jump[2] == 5 && !has[2]);  /* C forward to X, the next letter with an item */
    assert(jump[23] == 5 && has[23]);
    assert(jump[24] == 0 && !has[24] && jump[25] == 0); /* Y has none; Z is item 0 */
}

static void check_empty_tail(void) {
    const char * t[] = { "Abba", "Beck", "Dylan" };
    int jump[27];
    bool has[27];
    build(t, 3, jump, has);
    assert(jump[3] == 2 && jump[4] == -1 && jump[25] == -1); /* nothing after D */
    assert(jump[26] == -1 && !has[26]); /* no non-letter item, and '#' never borrows */
    for (int i = 4; i < 27; i++) assert(jump[i] == -1 && !has[i]);
    cover_flow_az_build(NULL, 0, jump, has);
    for (int i = 0; i < 27; i++) assert(jump[i] == -1 && !has[i]);
}

int main(void) {
    check_letters();
    check_sorted();
    check_unsorted();
    check_empty_tail();
    puts("cover_flow_azmap: PASS");
    return 0;
}
