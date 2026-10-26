#ifndef APE_CORE_H
#define APE_CORE_H

#include <stdbool.h>
#include <stdint.h>

// The Monkey's Audio frame decoder: the codec half of FFmpeg's apedec.c with
// the libavcodec plumbing taken out. File versions 3.80 to 3.99, 8, 16 and 24
// bit, mono and stereo, every compression level.
//
// One core per open file, and nothing global: two files decode at once on two
// threads, or interleaved on one.
//
// A frame is handed over as bytes already swapped into big-endian 32-bit words
// (ape_core_swap_words()), which is how the bitstream is read. Frames from file
// version 3.93 on are decoded in chunks, and their bytes may arrive a window at
// a time: when the range decoder reaches the end of what it was given it asks
// the refill callback for more. Older frames are decoded whole, from one buffer
// holding the entire frame.

typedef struct ape_core ape_core_t;

// Asked for more bytes of the current frame. The callback reads the window with
// ape_core_window(), moves or extends it, installs the result with
// ape_core_set_window(), and returns false when the frame has no more bytes.
typedef bool (*ape_refill_fn)(ape_core_t *core, void *arg);

// NULL for a stream this core cannot decode: file version outside 3800..3990,
// a compression level that is not 1000..5000, more than two channels, or a
// depth other than 8, 16 or 24 bits.
ape_core_t *ape_core_new(int fileversion, int compression, int flags, int bps, int channels);
void ape_core_free(ape_core_t *core);

// Blocks one ape_core_decode() call may be asked for: the whole frame for files
// before 3.93, whose coefficients are not interleaved, a fixed chunk otherwise.
bool ape_core_whole_frames(const ape_core_t *core);
#define APE_CORE_CHUNK 4608

// Starts a frame of `nblocks` blocks. `data` holds its swapped bytes from the
// aligned start; `skip` is the frame's offset into them (bytes from 3.90, bits
// before). 0, or negative when the frame header cannot be read.
int ape_core_start_frame(ape_core_t *core, const uint8_t *data, const uint8_t *end, uint32_t skip,
						 uint32_t nblocks, ape_refill_fn refill, void *arg);

// Decodes the next `count` blocks of the frame. On 0 the samples are in *left
// and *right (the same buffer twice for mono), right-justified at the stream's
// depth, 8-bit ones signed. Negative when the bitstream is broken; the rest of
// the frame is then lost.
int ape_core_decode(ape_core_t *core, int count, const int32_t **left, const int32_t **right);

// The byte window of the current frame, for the refill callback.
void ape_core_window(const ape_core_t *core, const uint8_t **ptr, const uint8_t **end);
void ape_core_set_window(ape_core_t *core, const uint8_t *ptr, const uint8_t *end);

// Reverses the byte order of each 32-bit word, in place. `bytes` is a multiple
// of four.
void ape_core_swap_words(uint8_t *buf, uint32_t bytes);

#endif /* APE_CORE_H */
