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
/* Bit by bit, for the last bytes of the frame and reads past its end. */
static unsigned int get_bits_tail(struct dst_bits *g, int n) {
  unsigned int v = 0;
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
/* 32 bits MSB first starting at byte p; the caller keeps p + 4 in bounds. */
static inline uint32_t dst_load_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
static inline unsigned int get_bits(struct dst_bits *g, int n) {
  if (n < 0 || n > 24) {
    g->error = 1;
    return 0;
  }
  if (n == 0)
    return 0;
  if (g->pos + 32 > g->bits)
    return get_bits_tail(g, n);
  uint32_t word = dst_load_be32(g->data + (g->pos >> 3)) << (g->pos & 7);
  g->pos += (size_t)n;
  return word >> (32 - n);
}
static int get_bits1(struct dst_bits *g) { return (int)get_bits(g, 1); }
static int get_sbits(struct dst_bits *g, int n) {
  unsigned int v = get_bits(g, n);
  if (n > 0 && (v & (1u << (n - 1))))
    v |= ~((1u << n) - 1);
  return (int)v;
}
static inline int av_log2(unsigned int v) {
  return v ? 31 - __builtin_clz(v) : -1;
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

#define DST_CHANNELS DST_MAX_CHANNELS
#define DST_ELEMENTS DST_MAX_CHANNELS
#define DST_FRAME_BYTES DST_CHANNEL_BYTES
/* One prediction table: 16 groups of 8 history bits, 256 int16 sums each. */
#define DST_FILTER_ALIGN 512
#if defined(__GNUC__) && !defined(DST_UNROLL)
#define DST_UNROLL _Pragma("GCC unroll 8")
#elif !defined(DST_UNROLL)
#define DST_UNROLL
#endif
typedef int16_t dst_filter_t[16][256];

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
  unsigned int channels;
  struct dst_bits gb;
  struct dst_arith ac;
  struct dst_table fsets;
  struct dst_table probs;
  uint32_t status[DST_CHANNELS][4];
  /* Stereo kernel: each channel's probabilities, 1..128, saturated to 128
   * entries so the index clamp is a constant. */
  uint8_t prob_sat[2 * 128];
  dst_filter_t *filter; /* DST_ELEMENTS tables, DST_FILTER_ALIGN aligned */
};

dst_decoder_t *dst_decoder_create(unsigned int channels) {
  if (channels == 0 || channels > DST_MAX_CHANNELS)
    return NULL;
  dst_decoder_t *decoder = calloc(1, sizeof(struct dst_decoder) +
                                         sizeof(dst_filter_t) * DST_ELEMENTS +
                                         DST_FILTER_ALIGN - 1);
  if (!decoder)
    return NULL;
  decoder->channels = channels;
  uintptr_t tables = (uintptr_t)(decoder + 1) + DST_FILTER_ALIGN - 1;
  tables &= ~(uintptr_t)(DST_FILTER_ALIGN - 1);
  decoder->filter = (dst_filter_t *)tables;
  return decoder;
}
void dst_decoder_destroy(dst_decoder_t *d) { free(d); }
static const int8_t dst_fset_pred[3][3] = {{-8}, {-16, 8}, {-9, -5, 6}};

static const int8_t dst_prob_pred[3][3] = {{-8}, {-16, 8}, {-24, 24, -8}};

static int dst_read_map(struct dst_bits *gb, struct dst_table *table,
                        unsigned int map[DST_CHANNELS], unsigned int channels) {
  table->elements = 1;
  map[0] = 0;
  if (!get_bits1(gb)) {
    for (unsigned int ch = 1; ch < channels; ++ch) {
      int bits = av_log2(table->elements) + 1;
      map[ch] = get_bits(gb, bits);
      if (map[ch] == table->elements) {
        if (++table->elements > DST_ELEMENTS)
          return -1;
      } else if (map[ch] > table->elements)
        return -1;
    }
  } else
    memset(map, 0, sizeof(unsigned int) * channels);
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
    int16_t (*filter)[256] = decoder->filter[i];
    for (int j = 0; j < 16; ++j) {
      int total = MIN(MAX(length - j * 8, 0), 8);
      int64_t base = 0;
      for (int bit = 0; bit < total; ++bit)
        base -= decoder->fsets.coeff[i][j * 8 + bit];
      if (base < INT16_MIN || base > INT16_MAX)
        return -1;
      filter[j][0] = (int16_t)base;
      for (unsigned int value = 1; value < 256; ++value) {
        unsigned int bit = (unsigned int)__builtin_ctz(value);
        int64_t sum = filter[j][value & (value - 1)];
        if (bit < (unsigned int)total)
          sum += (int64_t)2 * decoder->fsets.coeff[i][j * 8 + bit];
        if (sum < INT16_MIN || sum > INT16_MAX)
          return -1;
        filter[j][value] = (int16_t)sum;
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

/* MIPS32r2 (o32) lookup chains; the C versions below serve every other
 * build, including the host player. */
#if defined(__mips__) && defined(__mips_isa_rev) && __mips_isa_rev >= 2 && \
    __mips_isa_rev < 6 && defined(_ABIO32) && _MIPS_SIM == _ABIO32 &&       \
    !defined(DST_NO_ASM)
#define DST_MIPS_PREDICT 1
/*
 * One table lookup per 8 history bits. The table base is DST_FILTER_ALIGN
 * aligned, so `ins` writes byte * 2 straight into the address register and
 * `lh` adds the group offset: 3 or 4 instructions per group instead of 5.
 * The previous group's add sits between a `lh` and its use to cover the
 * load latency (about 4 cycles on the XBurst1); with two channels the
 * chains are interleaved.
 */
#define DST_LK_FIRST(c, w, off)                                               \
  "ins %[b" c "], %[" w "], 1, 8\n\tlh %[t" c "], " #off "(%[b" c "])\n\t"
#define DST_LK_0(c, w, off)                                                   \
  "ins %[b" c "], %[" w "], 1, 8\n\taddu %[a" c "], %[a" c "], %[t" c      \
  "]\n\tlh %[t" c "], " #off "(%[b" c "])\n\t"
#define DST_LK_S(c, w, sh, off)                                               \
  "srl %[s], %[" w "], " #sh "\n\tins %[b" c "], %[s], 1, 8\n\taddu %[a" c   \
  "], %[a" c "], %[t" c "]\n\tlh %[t" c "], " #off "(%[b" c "])\n\t"
/* Second group: the first lookup starts the sum instead of being added. */
#define DST_LK_SECOND(c, w, sh, off)                                          \
  "srl %[s], %[" w "], " #sh "\n\tins %[b" c "], %[s], 1, 8\n\tmove %[a" c   \
  "], %[t" c "]\n\tlh %[t" c "], " #off "(%[b" c "])\n\t"
#define DST_LK_LAST(c) "addu %[a" c "], %[a" c "], %[t" c "]\n\t"
#define DST_LK_WORD(c, w, base)                                               \
  DST_LK_0(c, w, base) DST_LK_S(c, w, 8, base + 512)                          \
  DST_LK_S(c, w, 16, base + 1024) DST_LK_S(c, w, 24, base + 1536)

static inline int dst_predict(const int16_t filter[16][256],
                              const uint32_t words[4]) {
  uintptr_t b0 = (uintptr_t)filter;
  int a0, t0, s0;
  __asm__(DST_LK_FIRST("0", "w0", 0) DST_LK_SECOND("0", "w0", 8, 512)
          DST_LK_S("0", "w0", 16, 1024) DST_LK_S("0", "w0", 24, 1536)
          DST_LK_WORD("0", "w1", 2048) DST_LK_WORD("0", "w2", 4096)
          DST_LK_WORD("0", "w3", 6144) DST_LK_LAST("0")
          : [b0] "+&r"(b0), [a0] "=&r"(a0), [t0] "=&r"(t0), [s] "=&r"(s0)
          : [w0] "r"(words[0]), [w1] "r"(words[1]), [w2] "r"(words[2]),
            [w3] "r"(words[3]), "m"(*(const int16_t (*)[16][256])filter));
  return a0;
}

/* The table a lookup base points into: `ins` leaves the low bits non-zero. */
#define DST_TABLE_OF(base) ((base) & ~(uintptr_t)(DST_FILTER_ALIGN - 1))

/* Both stereo predictions at once, the two lookup chains interleaved. The
 * base registers persist across calls: `ins` only rewrites bits 1..8. */
static inline void dst_predict2(uintptr_t *b0, uintptr_t *b1, uint32_t w0,
                                uint32_t w1, uint32_t w2, uint32_t w3,
                                uint32_t v0, uint32_t v1, uint32_t v2,
                                uint32_t v3, int *p0, int *p1) {
  int a0, t0, a1, t1, s;
  __asm__(DST_LK_FIRST("0", "w0", 0) DST_LK_FIRST("1", "v0", 0)
          DST_LK_SECOND("0", "w0", 8, 512) DST_LK_SECOND("1", "v0", 8, 512)
          DST_LK_S("0", "w0", 16, 1024) DST_LK_S("1", "v0", 16, 1024)
          DST_LK_S("0", "w0", 24, 1536) DST_LK_S("1", "v0", 24, 1536)
          DST_LK_0("0", "w1", 2048) DST_LK_0("1", "v1", 2048)
          DST_LK_S("0", "w1", 8, 2560) DST_LK_S("1", "v1", 8, 2560)
          DST_LK_S("0", "w1", 16, 3072) DST_LK_S("1", "v1", 16, 3072)
          DST_LK_S("0", "w1", 24, 3584) DST_LK_S("1", "v1", 24, 3584)
          DST_LK_0("0", "w2", 4096) DST_LK_0("1", "v2", 4096)
          DST_LK_S("0", "w2", 8, 4608) DST_LK_S("1", "v2", 8, 4608)
          DST_LK_S("0", "w2", 16, 5120) DST_LK_S("1", "v2", 16, 5120)
          DST_LK_S("0", "w2", 24, 5632) DST_LK_S("1", "v2", 24, 5632)
          DST_LK_0("0", "w3", 6144) DST_LK_0("1", "v3", 6144)
          DST_LK_S("0", "w3", 8, 6656) DST_LK_S("1", "v3", 8, 6656)
          DST_LK_S("0", "w3", 16, 7168) DST_LK_S("1", "v3", 16, 7168)
          DST_LK_S("0", "w3", 24, 7680) DST_LK_S("1", "v3", 24, 7680)
          DST_LK_LAST("0") DST_LK_LAST("1")
          : [b0] "+&r"(*b0), [a0] "=&r"(a0), [t0] "=&r"(t0),
            [b1] "+&r"(*b1), [a1] "=&r"(a1), [t1] "=&r"(t1), [s] "=&r"(s)
          : [w0] "r"(w0), [w1] "r"(w1), [w2] "r"(w2), [w3] "r"(w3),
            [v0] "r"(v0), [v1] "r"(v1), [v2] "r"(v2), [v3] "r"(v3),
            "m"(*(const int16_t (*)[16][256])DST_TABLE_OF(*b0)),
            "m"(*(const int16_t (*)[16][256])DST_TABLE_OF(*b1)));
  *p0 = a0;
  *p1 = a1;
}
#else
static inline int dst_predict(const int16_t filter[16][256],
                              const uint32_t words[4]) {
  int prediction = 0;
  for (unsigned int i = 0; i < 16; ++i) {
    unsigned int byte = (words[i / 4] >> ((i % 4) * 8)) & 0xff;
    prediction += filter[i][byte];
  }
  return prediction;
}
static inline void dst_predict2(uintptr_t *b0, uintptr_t *b1, uint32_t w0,
                                uint32_t w1, uint32_t w2, uint32_t w3,
                                uint32_t v0, uint32_t v1, uint32_t v2,
                                uint32_t v3, int *p0, int *p1) {
  const uint32_t w[4] = {w0, w1, w2, w3}, v[4] = {v0, v1, v2, v3};
  *p0 = dst_predict((const int16_t (*)[256])*b0, w);
  *p1 = dst_predict((const int16_t (*)[256])*b1, v);
}
#endif

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

/* A bit decoded while the channel's filter history is still filling: half
 * probability channels use 128 until their filter length is reached. */
static inline int dst_decode_start(struct dst_decoder *decoder, unsigned int ch,
                                   unsigned int felem, unsigned int pelem,
                                   unsigned int half_prob, unsigned int sample) {
  uint32_t *status = decoder->status[ch];
  int predict = dst_predict(decoder->filter[felem], status);
  int probability = 128;
  if (!half_prob || sample >= decoder->fsets.length[felem]) {
    unsigned int index = (unsigned int)abs(predict) >> 3;
    index = MIN(index, decoder->probs.length[pelem] - 1);
    probability = decoder->probs.coeff[pelem][index];
  }
  int residual = dst_ac_get(&decoder->ac, &decoder->gb, probability);
  int value = ((predict >> 15) ^ residual) & 1;
  dst_push_status(status, value);
  return value;
}

/* Smallest of two unsigned values without a branch. */
static inline unsigned int dst_min_u(unsigned int a, unsigned int b) {
#ifdef DST_MIPS_PREDICT
  unsigned int lt;
  __asm__("sltu %[lt], %[a], %[b]\n\tmovz %[a], %[b], %[lt]"
          : [a] "+r"(a), [lt] "=&r"(lt)
          : [b] "r"(b));
  return a;
#else
  return MIN(a, b);
#endif
}

/* Arithmetic decoder step with its state in locals; same maths as
 * dst_ac_get. Here a is always in [2048, 4095] on entry and probability in
 * [1, 128], so the new a is never 0 and the renormalisation reads 1..8 bits.
 * Those come from a 32-bit window `win` holding the next `cnt` bits of the
 * stream MSB first (zeros past its end, like get_bits_tail in padding mode);
 * cnt is at least 8 before every read, byte `next` of `data` refills it. */
#define DST_AC_STEP(prob, value)                                              \
  do {                                                                        \
    unsigned int k_ = (ac_a >> 8) | ((ac_a >> 7) & 1);                        \
    unsigned int q_ = k_ * (unsigned int)(prob);                              \
    unsigned int rem_ = ac_a - q_;                                            \
    unsigned int sel_ = ac_c < rem_;                                          \
    ac_a = sel_ ? rem_ : q_;                                                  \
    ac_c = sel_ ? ac_c : ac_c - rem_;                                         \
    (value) = sel_;                                                           \
    if (ac_a < 2048) {                                                        \
      unsigned int lz_ = (unsigned int)__builtin_clz(ac_a);                   \
      ac_a <<= lz_ - 20;                                                      \
      ac_c = (ac_c << (lz_ - 20)) | (win >> (52 - lz_));                      \
      win <<= lz_ - 20;                                                       \
      cnt -= lz_ - 20;                                                        \
      if (cnt < 8) {                                                          \
        win |= (next < end ? (unsigned int)data[next] : 0u) << (24 - cnt);    \
        ++next;                                                               \
        cnt += 8;                                                             \
      }                                                                       \
    }                                                                         \
  } while (0)

/* Stereo samples from a byte boundary at `sample` on, both channels past
 * their filter start: status words, arithmetic decoder and bit reader stay
 * in registers; one byte of each channel per outer iteration. */
static void dst_decode_stereo(struct dst_decoder *decoder,
                              const unsigned int fset_map[2],
                              const unsigned int prob_map[2], uint8_t *output,
                              unsigned int sample, unsigned int sample_count) {
  uintptr_t b0 = (uintptr_t)decoder->filter[fset_map[0]];
  uintptr_t b1 = (uintptr_t)decoder->filter[fset_map[1]];
  const uint8_t *prob = decoder->prob_sat;
  for (unsigned int ch = 0; ch < 2; ++ch) {
    const int *coeff = decoder->probs.coeff[prob_map[ch]];
    unsigned int last = decoder->probs.length[prob_map[ch]] - 1;
    for (unsigned int i = 0; i < 128; ++i)
      decoder->prob_sat[ch * 128 + i] = (uint8_t)coeff[MIN(i, last)];
  }
  struct dst_bits *gb = &decoder->gb;
  const size_t pos_start = gb->pos;
  /* Byte indices, not pointers: past the end the window reads zeros. */
  const uint8_t *data = gb->data;
  const size_t end = gb->bits >> 3;
  size_t next = pos_start >> 3;
  unsigned int ac_a = decoder->ac.a, ac_c = decoder->ac.c;
  uint32_t w0 = decoder->status[0][0], w1 = decoder->status[0][1];
  uint32_t w2 = decoder->status[0][2], w3 = decoder->status[0][3];
  uint32_t v0 = decoder->status[1][0], v1 = decoder->status[1][1];
  uint32_t v2 = decoder->status[1][2], v3 = decoder->status[1][3];
  /* Window: the four bytes at pos, shifted so the bit at pos is bit 31. */
  uint32_t win = 0;
  for (unsigned int i = 0; i < 4; ++i, ++next)
    win = (win << 8) | (next < end ? data[next] : 0u);
  win <<= pos_start & 7;
  unsigned int cnt = 32 - (pos_start & 7);
  uint8_t *const output_end = output + (sample_count - sample) / 4;

  for (; output != output_end; output += 2) {
    unsigned int packed = 0; /* channel 0 in bits 0..7, channel 1 in 16..23 */
    DST_UNROLL
    for (unsigned int k = 0; k < 8; ++k) {
      int p0, p1;
      unsigned int r0, r1;
      dst_predict2(&b0, &b1, w0, w1, w2, w3, v0, v1, v2, v3, &p0, &p1);
      /* |p| >> 3 without a branch: m is 0 or all ones. */
      unsigned int m0 = (unsigned int)(p0 >> 31), m1 = (unsigned int)(p1 >> 31);
      unsigned int i0 = dst_min_u((((unsigned int)p0 ^ m0) - m0) >> 3, 127);
      unsigned int i1 = dst_min_u((((unsigned int)p1 ^ m1) - m1) >> 3, 127);
      DST_AC_STEP(prob[i0], r0);
      DST_AC_STEP(prob[128 + i1], r1);
      unsigned int bit0 = ((unsigned int)(p0 >> 15) ^ r0) & 1;
      unsigned int bit1 = ((unsigned int)(p1 >> 15) ^ r1) & 1;
      w3 = (w3 << 1) | (w2 >> 31);
      w2 = (w2 << 1) | (w1 >> 31);
      w1 = (w1 << 1) | (w0 >> 31);
      w0 = (w0 << 1) | bit0;
      v3 = (v3 << 1) | (v2 >> 31);
      v2 = (v2 << 1) | (v1 >> 31);
      v1 = (v1 << 1) | (v0 >> 31);
      v0 = (v0 << 1) | bit1;
      packed = (packed << 1) | bit0 | (bit1 << 16);
    }
    output[0] = (uint8_t)packed;
    output[1] = (uint8_t)(packed >> 16);
  }
  /* Bit position and bits consumed past the end, as get_bits_tail counts. */
  const size_t pos = next * 8 - cnt;
  gb->pos = pos;
  if (pos > gb->bits)
    gb->overread +=
        (pos - gb->bits) - (pos_start > gb->bits ? pos_start - gb->bits : 0);
  decoder->ac.a = ac_a;
  decoder->ac.c = ac_c;
  decoder->status[0][0] = w0;
  decoder->status[0][1] = w1;
  decoder->status[0][2] = w2;
  decoder->status[0][3] = w3;
  decoder->status[1][0] = v0;
  decoder->status[1][1] = v1;
  decoder->status[1][2] = v2;
  decoder->status[1][3] = v3;
}

int dst_decoder_decode(dst_decoder_t *decoder, const uint8_t *input,
                       size_t input_size,
                       uint8_t output[DST_MAX_FRAME_BYTES]) {
  unsigned int fset_map[DST_CHANNELS];
  unsigned int prob_map[DST_CHANNELS];
  unsigned int half_prob[DST_CHANNELS];
  const unsigned int channels = decoder ? decoder->channels : 0;
  const unsigned int output_size = channels * DST_FRAME_BYTES;
  const unsigned int sample_count = DST_FRAME_BYTES * 8;

  if (!decoder || !input || !output || input_size <= 1 || input_size > 65536)
    return -1;
  dst_bits_init(&decoder->gb, input, input_size);
  if (!get_bits1(&decoder->gb)) {
    (void)get_bits1(&decoder->gb);
    if (get_bits(&decoder->gb, 6) || input_size - 1 < output_size)
      return -1;
    memcpy(output, input + 1, output_size);
    return (int)output_size;
  }

  if (!get_bits1(&decoder->gb) || !get_bits1(&decoder->gb) ||
      !get_bits1(&decoder->gb))
    return -1;
  int same_map = get_bits1(&decoder->gb);
  if (dst_read_map(&decoder->gb, &decoder->fsets, fset_map, channels) < 0)
    return -1;
  if (same_map) {
    decoder->probs.elements = decoder->fsets.elements;
    memcpy(prob_map, fset_map, sizeof(unsigned int) * channels);
  } else if (dst_read_map(&decoder->gb, &decoder->probs, prob_map, channels) < 0)
    return -1;

  for (unsigned int ch = 0; ch < channels; ++ch)
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
  memset(output, 0, output_size);
  dst_ac_get(&decoder->ac, &decoder->gb,
             dst_probability(decoder->fsets.coeff[0][0]));

  unsigned int packed[DST_CHANNELS] = {0};
  unsigned int fast_start = 0, output_pos = 0;
  for (unsigned int ch = 0; ch < channels; ++ch)
    fast_start = MAX(fast_start, decoder->fsets.length[fset_map[ch]]);

  /* Opening samples, while some channel's filter history is still filling. */
  for (unsigned int sample = 0; sample < fast_start; ++sample) {
    for (unsigned int ch = 0; ch < channels; ++ch)
      packed[ch] = (packed[ch] << 1) |
                   (unsigned int)dst_decode_start(decoder, ch, fset_map[ch], prob_map[ch],
                                                  half_prob[ch], sample);
    if ((sample & 7) == 7)
      for (unsigned int ch = 0; ch < channels; ++ch) {
        output[output_pos++] = (uint8_t)packed[ch];
        packed[ch] = 0;
      }
  }

  if (channels == 2) {
    /* fast_start is a filter length, 1..128, so the opening loop may stop
     * mid byte: finish that byte generically, then run the stereo kernel. */
    unsigned int sample = fast_start;
    for (; sample & 7; ++sample) {
      for (unsigned int ch = 0; ch < 2; ++ch)
        packed[ch] = (packed[ch] << 1) | (unsigned int)dst_decode_channel(
            decoder, decoder->filter[fset_map[ch]],
            decoder->probs.coeff[prob_map[ch]],
            decoder->probs.length[prob_map[ch]], decoder->status[ch]);
      if ((sample & 7) == 7) {
        output[output_pos++] = (uint8_t)packed[0];
        output[output_pos++] = (uint8_t)packed[1];
        packed[0] = packed[1] = 0;
      }
    }
    dst_decode_stereo(decoder, fset_map, prob_map, output + output_pos, sample,
                      sample_count);
  } else {
    for (unsigned int sample = fast_start; sample < sample_count; ++sample) {
      for (unsigned int ch = 0; ch < channels; ++ch)
        packed[ch] = (packed[ch] << 1) |
                     (unsigned int)dst_decode_channel(decoder, decoder->filter[fset_map[ch]],
                                                      decoder->probs.coeff[prob_map[ch]],
                                                      decoder->probs.length[prob_map[ch]],
                                                      decoder->status[ch]);
      if ((sample & 7) == 7)
        for (unsigned int ch = 0; ch < channels; ++ch) {
          output[output_pos++] = (uint8_t)packed[ch];
          packed[ch] = 0;
        }
    }
  }
  return decoder->gb.error || decoder->gb.overread > 16 ? -1 : (int)output_size;
}
