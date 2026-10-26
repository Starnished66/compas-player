#include "glyph5x7.h"

/* 5x7 dot-matrix font, uppercase + digits + space + ':', '-', and '_' only.
 * Deliberately not a full ASCII table -- the installer status strings and
 * the game runner's on-screen labels are hand-written and need only this
 * character set. Each glyph is 7 bytes, one per
 * row top-to-bottom, bits 4..0 = leftmost..rightmost column. The ASCII-art
 * comment above each glyph is the source of truth this was transcribed
 * from -- if a character ever looks wrong on real hardware, compare
 * against its own comment first, not just the hex. */
const uint8_t * glyph5x7_rows(char c) {
    static const uint8_t blank[7] = { 0, 0, 0, 0, 0, 0, 0 };

    switch (c) {
        /* .###.   #...#   #..##   #.#.#   ##..#   #...#   .###. */
        case '0': { static const uint8_t g[7] = { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }; return g; }
        /* ..#..   .##..   ..#..   ..#..   ..#..   ..#..   .###. */
        case '1': { static const uint8_t g[7] = { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }; return g; }
        /* .###.   #...#   ....#   ...#.   ..#..   .#...   ##### */
        case '2': { static const uint8_t g[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }; return g; }
        /* .###.   #...#   ....#   ..##.   ....#   #...#   .###. */
        case '3': { static const uint8_t g[7] = { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E }; return g; }
        /* ...#.   ..##.   .#.#.   #..#.   #####   ...#.   ...#. */
        case '4': { static const uint8_t g[7] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }; return g; }
        /* #####   #....   ####.   ....#   ....#   #...#   .###. */
        case '5': { static const uint8_t g[7] = { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }; return g; }
        /* ..##.   .#...   #....   ####.   #...#   #...#   .###. */
        case '6': { static const uint8_t g[7] = { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }; return g; }
        /* #####   ....#   ...#.   ..#..   .#...   .#...   .#... */
        case '7': { static const uint8_t g[7] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }; return g; }
        /* .###.   #...#   #...#   .###.   #...#   #...#   .###. */
        case '8': { static const uint8_t g[7] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }; return g; }
        /* .###.   #...#   #...#   .####   ....#   ...#.   .##.. */
        case '9': { static const uint8_t g[7] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }; return g; }

        /* ..#..   .#.#.   #...#   #...#   #####   #...#   #...# */
        case 'A': { static const uint8_t g[7] = { 0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11 }; return g; }
        /* ####.   #...#   #...#   ####.   #...#   #...#   ####. */
        case 'B': { static const uint8_t g[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }; return g; }
        /* .####   #....   #....   #....   #....   #....   .#### */
        case 'C': { static const uint8_t g[7] = { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F }; return g; }
        /* ####.   #...#   #...#   #...#   #...#   #...#   ####. */
        case 'D': { static const uint8_t g[7] = { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }; return g; }
        /* #####   #....   #....   ####.   #....   #....   ##### */
        case 'E': { static const uint8_t g[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }; return g; }
        /* #####   #....   #....   ####.   #....   #....   #.... */
        case 'F': { static const uint8_t g[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }; return g; }
        /* .####   #....   #....   #.###   #...#   #...#   .###. */
        case 'G': { static const uint8_t g[7] = { 0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0E }; return g; }
        /* .###.   ..#..   ..#..   ..#..   ..#..   ..#..   .###. */
        case 'I': { static const uint8_t g[7] = { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }; return g; }
        /* #...#   #..#.   #.#..   ##...   #.#..   #..#.   #...# */
        case 'K': { static const uint8_t g[7] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }; return g; }
        /* #....   #....   #....   #....   #....   #....   ##### */
        case 'L': { static const uint8_t g[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }; return g; }
        /* #...#   ##.##   #.#.#   #...#   #...#   #...#   #...# */
        case 'M': { static const uint8_t g[7] = { 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11 }; return g; }
        /* #...#   ##..#   #.#.#   #..##   #...#   #...#   #...# */
        case 'N': { static const uint8_t g[7] = { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }; return g; }
        /* .###.   #...#   #...#   #...#   #...#   #...#   .###. */
        case 'O': { static const uint8_t g[7] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }; return g; }
        /* ####.   #...#   #...#   ####.   #....   #....   #.... */
        case 'P': { static const uint8_t g[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }; return g; }
        /* ####.   #...#   #...#   ####.   #.#..   #..#.   #...# */
        case 'R': { static const uint8_t g[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }; return g; }
        /* .####   #....   #....   .###.   ....#   ....#   ####. */
        case 'S': { static const uint8_t g[7] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }; return g; }
        /* #####   ..#..   ..#..   ..#..   ..#..   ..#..   ..#.. */
        case 'T': { static const uint8_t g[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }; return g; }
        /* #...#   #...#   #...#   #...#   #...#   #...#   .###. */
        case 'U': { static const uint8_t g[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }; return g; }
        /* #...#   #...#   #...#   #...#   #...#   .#.#.   ..#.. */
        case 'V': { static const uint8_t g[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }; return g; }
        /* #...#   #...#   #...#   #.#.#   #.#.#   ##.##   #...# */
        case 'W': { static const uint8_t g[7] = { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 }; return g; }
        /* #...#   #...#   .#.#.   ..#..   .#.#.   #...#   #...# */
        case 'X': { static const uint8_t g[7] = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }; return g; }
        /* #...#   #...#   .#.#.   ..#..   ..#..   ..#..   ..#.. */
        case 'Y': { static const uint8_t g[7] = { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }; return g; }

        /* .....   .....   ..#..   .....   ..#..   .....   ..... */
        case ':': { static const uint8_t g[7] = { 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00 }; return g; }
        /* .....   .....   .....   #####   .....   .....   ..... */
        case '-': { static const uint8_t g[7] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }; return g; }
        /* .....   .....   .....   .....   .....   .....   ##### */
        case '_': { static const uint8_t g[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F }; return g; }

        case ' ': return blank;
        default: return blank; /* unsupported char -- a visible gap, not a silent shift (see fb_draw.h) */
    }
}
