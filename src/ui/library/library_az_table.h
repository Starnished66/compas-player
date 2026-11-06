#pragma once
/* The A-Z jump table of an in-memory list. Pure, so it tests on the host.
 * Mirrors metadata_db_get_az_table(): a letter is the first byte of the
 * name, ASCII case-folded, and '#' is anything that is not a-z. */

#define LIBRARY_AZ_LETTERS 27 /* A..Z, then '#' */

/* 0..25 for a-z or A-Z, 26 for anything else (digits, punctuation, empty, NULL). */
int library_az_letter(const char * name);

/* Name of row `index`; may be NULL. */
typedef const char * (*library_az_name_of_t)(int index, void * ctx);

/* Fills `jump[27]` with the first row of each letter, or of the next letter
 * that has one (A..Z only; '#' never borrows), or -1 when none is left. The
 * rows need not be sorted: the lowest index wins. */
void library_az_table_build(library_az_name_of_t name_of, void * ctx, int count, int jump[LIBRARY_AZ_LETTERS]);
