#include "dst_decoder.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 0x8a5cd789u;

static uint32_t random_u32(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static void test_compressed_file(dst_decoder_t *decoder, const char *path) {
  FILE *file = fopen(path, "rb");
  assert(file);
  uint8_t input[65536];
  size_t size = fread(input, 1, sizeof(input), file);
  assert(!ferror(file) && size > 1);
  assert(fclose(file) == 0);
  uint8_t output[DST_STEREO_FRAME_BYTES];
  assert(dst_decoder_decode(decoder, input, size, output) ==
         DST_STEREO_FRAME_BYTES);
  if (size > 64) {
    const size_t truncated_sizes[] = {1, 8, 32, size / 2};
    for (size_t i = 0; i < sizeof(truncated_sizes) / sizeof(truncated_sizes[0]);
         ++i) {
      size_t truncated_size = truncated_sizes[i];
      if (truncated_size > 1 && truncated_size < size)
        assert(dst_decoder_decode(decoder, input, truncated_size, output) ==
               -1);
    }
  }
}

int main(int argc, char **argv) {
  dst_decoder_t *d = dst_decoder_create();
  uint8_t *in = calloc(1, DST_STEREO_FRAME_BYTES + 1);
  uint8_t out[DST_STEREO_FRAME_BYTES];
  assert(d && in);
  in[0] = 0; /* raw (uncompressed) frame flag and reserved bits */
  for (size_t i = 1; i < DST_STEREO_FRAME_BYTES + 1; ++i)
    in[i] = (uint8_t)i;
  assert(dst_decoder_decode(d, in, DST_STEREO_FRAME_BYTES + 1, out) ==
         DST_STEREO_FRAME_BYTES);
  assert(memcmp(out, in + 1, sizeof(out)) == 0);
  assert(dst_decoder_decode(d, in, DST_STEREO_FRAME_BYTES, out) == -1);
  in[0] = 0x02; /* nonzero reserved bit in raw frame header */
  assert(dst_decoder_decode(d, in, DST_STEREO_FRAME_BYTES + 1, out) == -1);
  memset(in, 0xff, 64); /* truncated compressed header/tables */
  for (size_t size = 1; size <= 64; ++size)
    assert(dst_decoder_decode(d, in, size, out) == -1);

  /* Deterministic malformed compressed inputs for bounds/UB coverage. */
  for (unsigned int iteration = 0; iteration < 256; ++iteration) {
    size_t size = 2 + random_u32() % 1023;
    for (size_t i = 0; i < size; ++i)
      in[i] = (uint8_t)random_u32();
    in[0] |= 0xf0; /* enter compressed-frame parsing */
    (void)dst_decoder_decode(d, in, size, out);
  }
  for (int i = 1; i < argc; ++i)
    test_compressed_file(d, argv[i]);
  dst_decoder_destroy(d);
  free(in);
  return 0;
}
