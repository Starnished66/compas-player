#ifndef GLYPH5X7_H
#define GLYPH5X7_H

#include <stdint.h>

#define GLYPH5X7_COLS 5
#define GLYPH5X7_ROWS 7
#define GLYPH5X7_SPACING 1 /* blank column between characters */
#define GLYPH5X7_CELL_W (GLYPH5X7_COLS + GLYPH5X7_SPACING)

/* Returns GLYPH5X7_ROWS bytes, one per row top-to-bottom, bits 4..0 =
 * leftmost..rightmost column. Unsupported characters (including space)
 * return an all-blank glyph, never NULL. */
const uint8_t * glyph5x7_rows(char c);

#endif /* GLYPH5X7_H */
