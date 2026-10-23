/*
 * Direct Stream Transfer decoder adapted from FFmpeg dstdec.c.
 * Copyright (c) 2014 Peter Ross <pross@xvid.org>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * The LGPL 2.1 license text is included in APE_FFMPEG_CORE_LICENSE.txt.
 */
#include "dst_decoder.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
struct dst_bits {
  const uint8_t *data;
  size_t bits, pos, overread;
  int error;
  bool allow_padding;
};
static void dst_bits_init(struct dst_bits *g, const uint8_t *p, size_t n) {
  g->data = p;
  g->bits = n * 8;
  g->pos = 0;
  g->overread = 0;
  g->error = 0;
  g->allow_padding = false;
}
static int get_bits_left(struct dst_bits *g) {
  return g->error || g->pos > g->bits ? 0 : (int)(g->bits - g->pos);
}
static unsigned int get_bits(struct dst_bits *g, int n) {
  unsigned int v = 0;
  if (n < 0 || n > 24) {
    g->error = 1;
    return 0;
  }
  while (n--) {
    if (g->pos < g->bits)
      v = (v << 1) | ((g->data[g->pos >> 3] >> (7 - (g->pos & 7))) & 1);
    else {
      v <<= 1;
      ++g->overread;
      if (!g->allow_padding)
        g->error = 1;
    }
    ++g->pos;
  }
  return v;
}
static int get_bits1(struct dst_bits *g) { return (int)get_bits(g, 1); }
static int get_sbits(struct dst_bits *g, int n) {
  unsigned int v = get_bits(g, n);
  if (n > 0 && (v & (1u << (n - 1))))
    v |= ~((1u << n) - 1);
  return (int)v;
}
static int av_log2(unsigned int v) {
  int n = -1;
  while (v) {
    ++n;
    v >>= 1;
  }
  return n;
}
/* JPEG-LS signed Rice code used by DST: unary quotient followed by k bits. */
static int get_ur_golomb_jpegls(struct dst_bits *g, unsigned int k, int limit) {
  if (limit <= 0 || k > 24)
    return -1;
  unsigned int q = 0;
  while (q < (unsigned int)limit && get_bits_left(g) > 0 && !get_bits1(g))
    ++q;
  if (g->error)
    return -1;
  if (q < (unsigned int)limit - 1) {
    unsigned int remainder = get_bits(g, (int)k);
    if (g->error || q > (unsigned int)(INT_MAX >> k))
      return -1;
    return (int)((q << k) | remainder);
  }
  return -1;
}

#define DST_CHANNELS 2
#define DST_ELEMENTS DST_CHANNELS
#define DST_FRAME_BYTES DST_STEREO_CHANNEL_BYTES

struct dst_arith {
  unsigned int a;
  unsigned int c;
};

struct dst_table {
  unsigned int elements;
  unsigned int length[DST_ELEMENTS];
  int coeff[DST_ELEMENTS][128];
};

struct dst_decoder {
  struct dst_bits gb;
  struct dst_arith ac;
  struct dst_table fsets;
  struct dst_table probs;
  uint32_t status[DST_CHANNELS][4];
  int16_t filter[DST_ELEMENTS][16][256];
};

dst_decoder_t *dst_decoder_create(void) {
  return calloc(1, sizeof(struct dst_decoder));
}
void dst_decoder_destroy(dst_decoder_t *d) { free(d); }
static const int8_t dst_fset_pred[3][3] = {{-8}, {-16, 8}, {-9, -5, 6}};

static const int8_t dst_prob_pred[3][3] = {{-8}, {-16, 8}, {-24, 24, -8}};

static int dst_read_map(struct dst_bits *gb, struct dst_table *table,
                        unsigned int map[DST_CHANNELS]) {
  table->elements = 1;
  map[0] = 0;
  if (!get_bits1(gb)) {
    for (int ch = 1; ch < DST_CHANNELS; ++ch) {
      int bits = av_log2(table->elements) + 1;
      map[ch] = get_bits(gb, bits);
      if (map[ch] == table->elements) {
        if (++table->elements > DST_ELEMENTS)
          return -1;
      } else if (map[ch] > table->elements)
        return -1;
    }
  } else
    memset(map, 0, sizeof(unsigned int) * DST_CHANNELS);
  return 0;
}

static int dst_signed_rice(struct dst_bits *gb, unsigned int k) {
  int value = get_ur_golomb_jpegls(gb, k, get_bits_left(gb));
  if (value < 0)
    return INT_MIN;
  if (value && get_bits1(gb))
    value = -value;
  return value;
}

static int dst_read_plain(struct dst_bits *gb, int *dst, unsigned int count,
                          int bits, bool is_signed, int offset) {
  if (get_bits_left(gb) < (int)(count * bits))
    return -1;
  for (unsigned int i = 0; i < count; ++i)
    dst[i] =
        (is_signed ? get_sbits(gb, bits) : (int)get_bits(gb, bits)) + offset;
  return 0;
}

static int dst_read_table(struct dst_bits *gb, struct dst_table *table,
                          const int8_t predictor[3][3], int length_bits,
                          int coeff_bits, bool is_signed, int offset) {
  for (unsigned int i = 0; i < table->elements; ++i) {
    if (get_bits_left(gb) < length_bits + 1)
      return -1;
    table->length[i] = get_bits(gb, length_bits) + 1;
    if (table->length[i] > 128)
      return -1;
    if (!get_bits1(gb)) {
      if (dst_read_plain(gb, table->coeff[i], table->length[i], coeff_bits,
                         is_signed, offset) < 0)
        return -1;
      continue;
    }

    int method = get_bits(gb, 2);
    if (method == 3 || dst_read_plain(gb, table->coeff[i], method + 1,
                                      coeff_bits, is_signed, offset) < 0)
      return -1;
    int lsb_size = get_bits(gb, 3);
    for (unsigned int j = method + 1; j < table->length[i]; ++j) {
      int64_t predicted = 0;
      for (int k = 0; k < method + 1; ++k)
        predicted += (int64_t)predictor[method][k] * table->coeff[i][j - k - 1];
      int value = dst_signed_rice(gb, lsb_size);
      if (value == INT_MIN)
        return -1;
      int64_t adjusted = value;
      if (predicted >= 0)
        adjusted -= (predicted + 4) / 8;
      else
        adjusted += (-predicted + 3) / 8;
      if (adjusted < INT_MIN || adjusted > INT_MAX)
        return -1;
      value = (int)adjusted;
      if (!is_signed && (value < offset || value >= offset + (1 << coeff_bits)))
        return -1;
      table->coeff[i][j] = value;
    }
  }
  return 0;
}

static void dst_ac_init(struct dst_arith *ac, struct dst_bits *gb) {
  ac->a = 4095;
  ac->c = get_bits(gb, 12);
}

static inline int dst_ac_get(struct dst_arith *ac, struct dst_bits *gb,
                             int probability) {
  unsigned int k = (ac->a >> 8) | ((ac->a >> 7) & 1);
  unsigned int q = k * probability;
  unsigned int remaining = ac->a - q;
  int value = ac->c < remaining;
  if (value)
    ac->a = remaining;
  else {
    ac->a = q;
    ac->c -= remaining;
  }
  if (ac->a < 2048) {
    int bits = 11 - av_log2(ac->a);
    ac->a <<= bits;
    ac->c = (ac->c << bits) | get_bits(gb, bits);
  }
  return value;
}

static unsigned char dst_probability(int value) {
  unsigned int bits = value & 127;
  bits = ((bits & 0x55) << 1) | ((bits >> 1) & 0x55);
  bits = ((bits & 0x33) << 2) | ((bits >> 2) & 0x33);
  bits = ((bits & 0x0f) << 4) | ((bits >> 4) & 0x0f);
  return (bits >> 1) + 1;
}

static int dst_build_filter(struct dst_decoder *decoder) {
  for (unsigned int i = 0; i < decoder->fsets.elements; ++i) {
    int length = decoder->fsets.length[i];
    for (int j = 0; j < 16; ++j) {
      int total = MIN(MAX(length - j * 8, 0), 8);
      int64_t base = 0;
      for (int bit = 0; bit < total; ++bit)
        base -= decoder->fsets.coeff[i][j * 8 + bit];
      if (base < INT16_MIN || base > INT16_MAX)
        return -1;
      decoder->filter[i][j][0] = (int16_t)base;
      for (unsigned int value = 1; value < 256; ++value) {
        unsigned int bit = (unsigned int)__builtin_ctz(value);
        int64_t sum = decoder->filter[i][j][value & (value - 1)];
        if (bit < (unsigned int)total)
          sum += (int64_t)2 * decoder->fsets.coeff[i][j * 8 + bit];
        if (sum < INT16_MIN || sum > INT16_MAX)
          return -1;
        decoder->filter[i][j][value] = (int16_t)sum;
      }
    }
  }
  return 0;
}

static inline void dst_push_status(uint32_t status[4], int value) {
  status[3] = (status[3] << 1) | (status[2] >> 31);
  status[2] = (status[2] << 1) | (status[1] >> 31);
  status[1] = (status[1] << 1) | (status[0] >> 31);
  status[0] = (status[0] << 1) | value;
}

static inline int dst_predict(const int16_t filter[16][256],
                              const uint32_t words[4]) {
  int prediction = 0;
  for (unsigned int i = 0; i < 16; ++i) {
    unsigned int byte = (words[i / 4] >> ((i % 4) * 8)) & 0xff;
    prediction += filter[i][byte];
  }
  return prediction;
}

static inline int dst_decode_channel(struct dst_decoder *decoder,
                                     const int16_t filter[16][256],
                                     const int *probabilities,
                                     unsigned int probability_count,
                                     uint32_t status[4]) {
  int predict = dst_predict(filter, status);
  unsigned int index = abs(predict) >> 3;
  index = MIN(index, probability_count - 1);
  int residual = dst_ac_get(&decoder->ac, &decoder->gb, probabilities[index]);
  int value = ((predict >> 15) ^ residual) & 1;
  dst_push_status(status, value);
  return value;
}

int dst_decoder_decode(dst_decoder_t *decoder, const uint8_t *input,
                       size_t input_size,
                       uint8_t output[DST_STEREO_FRAME_BYTES]) {
  unsigned int fset_map[DST_CHANNELS];
  unsigned int prob_map[DST_CHANNELS];
  unsigned int half_prob[DST_CHANNELS];
  const unsigned int sample_count = DST_FRAME_BYTES * 8;

  if (!decoder || !input || !output || input_size <= 1 || input_size > 65536)
    return -1;
  dst_bits_init(&decoder->gb, input, input_size);
  if (!get_bits1(&decoder->gb)) {
    (void)get_bits1(&decoder->gb);
    if (get_bits(&decoder->gb, 6) || input_size - 1 < DST_FRAME_BYTES * 2)
      return -1;
    memcpy(output, input + 1, DST_FRAME_BYTES * 2);
    return DST_FRAME_BYTES * 2;
  }

  if (!get_bits1(&decoder->gb) || !get_bits1(&decoder->gb) ||
      !get_bits1(&decoder->gb))
    return -1;
  int same_map = get_bits1(&decoder->gb);
  if (dst_read_map(&decoder->gb, &decoder->fsets, fset_map) < 0)
    return -1;
  if (same_map) {
    decoder->probs.elements = decoder->fsets.elements;
    memcpy(prob_map, fset_map, sizeof(prob_map));
  } else if (dst_read_map(&decoder->gb, &decoder->probs, prob_map) < 0)
    return -1;

  for (int ch = 0; ch < DST_CHANNELS; ++ch)
    half_prob[ch] = get_bits1(&decoder->gb);
  if (dst_read_table(&decoder->gb, &decoder->fsets, dst_fset_pred, 7, 9, true,
                     0) < 0 ||
      dst_read_table(&decoder->gb, &decoder->probs, dst_prob_pred, 6, 7, false,
                     1) < 0 ||
      get_bits1(&decoder->gb) || get_bits_left(&decoder->gb) < 12)
    return -1;

  decoder->gb.allow_padding = true;
  dst_ac_init(&decoder->ac, &decoder->gb);
  if (dst_build_filter(decoder) < 0)
    return -1;
  memset(decoder->status, 0xaa, sizeof(decoder->status));
  memset(output, 0, DST_FRAME_BYTES * DST_CHANNELS);
  dst_ac_get(&decoder->ac, &decoder->gb,
             dst_probability(decoder->fsets.coeff[0][0]));

  unsigned int output_pos = 0;
  unsigned int packed[DST_CHANNELS] = {0, 0};
  unsigned int fast_start = MAX(decoder->fsets.length[fset_map[0]],
                                decoder->fsets.length[fset_map[1]]);
  for (unsigned int sample = 0; sample < fast_start; ++sample) {
    for (int ch = 0; ch < DST_CHANNELS; ++ch) {
      unsigned int felem = fset_map[ch];
      uint32_t *status = decoder->status[ch];
      int predict = dst_predict(decoder->filter[felem], status);
      int probability = 128;
      if (!half_prob[ch] || sample >= decoder->fsets.length[felem]) {
        unsigned int pelem = prob_map[ch];
        unsigned int index = abs(predict) >> 3;
        index = MIN(index, decoder->probs.length[pelem] - 1);
        probability = decoder->probs.coeff[pelem][index];
      }
      int residual = dst_ac_get(&decoder->ac, &decoder->gb, probability);
      int value = ((predict >> 15) ^ residual) & 1;
      packed[ch] = (packed[ch] << 1) | value;
      dst_push_status(status, value);
    }
    if ((sample & 7) == 7) {
      output[output_pos++] = packed[0];
      output[output_pos++] = packed[1];
      packed[0] = packed[1] = 0;
    }
  }

  const int16_t (*filter0)[256] = decoder->filter[fset_map[0]];
  const int16_t (*filter1)[256] = decoder->filter[fset_map[1]];
  const int *prob0 = decoder->probs.coeff[prob_map[0]];
  const int *prob1 = decoder->probs.coeff[prob_map[1]];
  unsigned int prob0_count = decoder->probs.length[prob_map[0]];
  unsigned int prob1_count = decoder->probs.length[prob_map[1]];
  uint32_t *status0 = decoder->status[0];
  uint32_t *status1 = decoder->status[1];

  for (unsigned int sample = fast_start; sample < sample_count; ++sample) {
    packed[0] = (packed[0] << 1) | dst_decode_channel(decoder, filter0, prob0,
                                                      prob0_count, status0);
    packed[1] = (packed[1] << 1) | dst_decode_channel(decoder, filter1, prob1,
                                                      prob1_count, status1);
    if ((sample & 7) == 7) {
      output[output_pos++] = packed[0];
      output[output_pos++] = packed[1];
      packed[0] = packed[1] = 0;
    }
  }
  return decoder->gb.error || decoder->gb.overread > 16
             ? -1
             : (int)(DST_FRAME_BYTES * DST_CHANNELS);
}
