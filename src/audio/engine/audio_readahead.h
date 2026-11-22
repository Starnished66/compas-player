#ifndef AUDIO_READAHEAD_H
#define AUDIO_READAHEAD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Read-ahead for the track that is playing. A reader is a plain FILE until
 * ra_claim(); a claimed reader serves reads from a RAM window that one
 * background fetcher keeps filled, so a stall of the card does not reach the
 * audio thread. Until the window holds the bytes, reads go straight to the
 * FILE as before, so the audio thread never waits for the fetcher. Seeks are
 * logical (they only move the position); past-EOF seeks succeed and the
 * next read returns 0, as on a regular file. */
typedef struct ra_reader ra_reader_t;

/* Starts the fetcher thread. Without it claims do nothing. Call once. */
void ra_start(void);

ra_reader_t * ra_open(const char * path);
/* Never waits for the fetcher: a fetcher stuck on a dead card is left behind. */
void ra_close(ra_reader_t * reader);
size_t ra_read(ra_reader_t * reader, void * buffer, size_t bytes);
/* 0 on success, -1 on failure with the position unchanged. */
int ra_seek(ra_reader_t * reader, int64_t offset, int whence);
int64_t ra_size(const ra_reader_t * reader);

/* Makes this the one reader the fetcher serves, sized for bytes_per_second
 * of content. Posts a request and returns; harmless to repeat. */
void ra_claim(ra_reader_t * reader, uint64_t bytes_per_second);
int ra_is_claimed(const ra_reader_t * reader);

#ifdef RA_TEST_HOOK
extern uint64_t ra_test_window_bytes;
int64_t ra_tell(const ra_reader_t * reader);
/* Replaces pread in the fetcher. */
extern ssize_t ra_test_pread(int fd, void * buf, size_t count, off_t offset);
/* Replaces malloc for the window buffer. */
extern void * ra_test_malloc(size_t bytes);
#endif

#endif
