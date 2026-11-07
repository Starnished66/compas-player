#define _FILE_OFFSET_BITS 64
#include "src/library/metadata/parsing/metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void install_thread_crash_altstack(void) {}
bool main_loop_discard_pointer_input(void) { return false; }
void main_loop_discard_elapsed_time(void) {}
static void le32(FILE *f, unsigned n) {
  for (int i = 0; i < 4; i++)
    fputc(n >> (8 * i), f);
}
static void chunk(FILE *f, const char *id, const void *p, unsigned n) {
  fwrite(id, 1, 4, f);
  le32(f, n);
  fwrite(p, 1, n, f);
  if (n & 1)
    fputc(0, f);
}
static void info(FILE *f, const char *title) {
  long pos = ftell(f);
  fwrite("LIST", 1, 4, f);
  le32(f, 0);
  fwrite("INFO", 1, 4, f);
  chunk(f, "INAM", title, (unsigned)strlen(title) + 1);
  chunk(f, "IART", "Kh?c Vi?t", 10);
  chunk(f, "IPRD", "M?t C?m", 8);
  chunk(f, "ITRK", "2", 2);
  long end = ftell(f);
  fseek(f, pos + 4, SEEK_SET);
  le32(f, (unsigned)(end - pos - 8));
  fseek(f, end, SEEK_SET);
}
static void frame(FILE *f, const char *id, const unsigned short *text,
                  size_t n) {
  fwrite(id, 1, 4, f);
  unsigned len = (unsigned)n * 2 + 3;
  fputc(len >> 24, f);
  fputc(len >> 16, f);
  fputc(len >> 8, f);
  fputc(len, f);
  fputc(0, f);
  fputc(0, f);
  fputc(1, f);
  fputc(255, f);
  fputc(254, f);
  for (size_t i = 0; i < n; i++) {
    fputc(text[i], f);
    fputc(text[i] >> 8, f);
  }
}
#define TEXT(f, id, ...)                                                       \
  do {                                                                         \
    const unsigned short t[] = {__VA_ARGS__};                                  \
    frame(f, id, t, sizeof(t) / sizeof(*t));                                   \
  } while (0)
static void id3(FILE *f, int partial) {
  long pos = ftell(f);
  fwrite(partial ? "id3 " : "ID3 ", 1, 4, f);
  le32(f, 0);
  fwrite("ID3\3\0\0\0\0\0\0", 1, 10, f);
  TEXT(f, "TIT2", 0x65e5, 0x672c, 0x8a9e);
  if (!partial) {
    TEXT(f, "TPE1", 'K', 'h', 0x1eaf, 'c', ' ', 'V', 'i', 0x1ec7, 't');
    TEXT(f, "TALB", 0x97f3, 0x697d);
    TEXT(f, "TPE2", 0x5408, 0x4f5c);
    TEXT(f, "TXXX", 'R', 'E', 'P', 'L', 'A', 'Y', 'G', 'A', 'I', 'N', '_', 'T',
         'R', 'A', 'C', 'K', '_', 'G', 'A', 'I', 'N', 0, '-', '6', '.', '0');
    TEXT(f, "TCON", 'F', 'o', 'l', 'k');
    TEXT(f, "TRCK", '7');
    TEXT(f, "TPOS", '3');
    TEXT(f, "TYER", '2', '0', '2', '4');
  }
  long end = ftell(f);
  unsigned size = (unsigned)(end - pos - 18);
  fseek(f, pos + 14, SEEK_SET);
  for (int i = 3; i >= 0; i--)
    fputc((size >> (i * 7)) & 127, f);
  fseek(f, pos + 4, SEEK_SET);
  le32(f, (unsigned)(end - pos - 8));
  fseek(f, end, SEEK_SET);
  if ((end - pos) & 1)
    fputc(0, f);
}
static void fixture(const char *path, int mode) {
  FILE *f = fopen(path, "wb");
  assert(f);
  fwrite(mode == 6 ? "RF64" : "RIFF", 1, 4, f);
  le32(f, 0);
  fwrite("WAVE", 1, 4, f);
  const unsigned char fmt[] = {1,    0,    1, 0, 0x44, 0xac, 0,  0,
                               0x88, 0x58, 1, 0, 2,    0,    16, 0};
  if (mode == 6) {
    unsigned char ds64[28] = {0};
    ds64[8] = 2;
    ds64[16] = 1;
    chunk(f, "ds64", ds64, sizeof(ds64));
  }
  chunk(f, "fmt ", fmt, sizeof(fmt));
  if (mode == 6) {
    fwrite("data", 1, 4, f);
    le32(f, ~0u);
    fputc(0, f);
    fputc(0, f);
  } else
    chunk(f, "data", "\0\0", 2);
  if (mode == 1)
    id3(f, 0);
  if (mode != 4)
    info(f, mode == 2   ? "bad\xff"
            : mode == 7 ? "\xff\xfe\x01\xd8"
            : mode == 8 ? "\xff\xfe\xe5\x65\x2c\x67\x9e\x8a"
            : mode == 3 ? "日本語"
                        : "? title");
  if (mode == 0 || mode == 4 || mode == 6)
    id3(f, 0);
  if (mode == 5)
    id3(f, 1);
  long end = ftell(f);
  fseek(f, 4, SEEK_SET);
  le32(f, mode == 6 ? ~0u : (unsigned)(end - 8));
  if (mode == 6) {
    fseek(f, 20, SEEK_SET);
    le32(f, (unsigned)(end - 8));
  }
  fclose(f);
}
int main(void) {
  char path[] = "/tmp/compas-wav-XXXXXX.wav";
  int fd = mkstemps(path, 4);
  assert(fd >= 0);
  close(fd);
  for (int mode = 0; mode < 9; mode++) {
    fixture(path, mode);
    track_metadata_t m;
    metadata_read_without_artwork(path, &m);
    if (mode == 2 || mode == 7) {
      assert(!m.has_title);
      assert(m.title[0] == 0);
    } else {
      assert(m.has_title);
      assert(!strcmp(m.title, "日本語"));
    }
    if (mode == 0 || mode == 1 || mode == 4 || mode == 6) {
      assert(!strcmp(m.artist, "Khắc Việt"));
      assert(!strcmp(m.album, "音楽"));
      assert(!strcmp(m.album_artist, "合作"));
      assert(m.has_replaygain && m.replaygain_gain_db == -6.0);
      assert(!strcmp(m.genre, "Folk"));
      assert(m.track_number == 7);
      assert(m.disc_number == 3);
      assert(m.release_year == 2024);
    } else {
      assert(!strcmp(m.artist, "Kh?c Vi?t"));
      assert(!strcmp(m.album, "M?t C?m"));
      assert(m.track_number == 2);
    }
    free(m.picture_data);
    free(m.lyrics);
  }
  unlink(path);
  puts("WAV metadata fixtures passed");
  return 0;
}
