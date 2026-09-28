#ifndef ZIP_READER_H
#define ZIP_READER_H

#include <stddef.h>
#include <stdint.h>

#define ZIP_MAX_UNCOMPRESSED_BYTES (256u * 1024u)
#define ZIP_MAX_COMPRESSED_BYTES   (256u * 1024u)
#define ZIP_MAX_CD_BYTES           (256u * 1024u)
#define ZIP_MAX_ENTRIES            2000u

typedef enum {
    ZIP_OK = 0,
    ZIP_ERR_IO,
    ZIP_ERR_CORRUPT,
    ZIP_ERR_NOT_FOUND,
    ZIP_ERR_ENCRYPTED,
    ZIP_ERR_ZIP64,
    ZIP_ERR_UNSUPPORTED_METHOD,
    ZIP_ERR_CRC,
    ZIP_ERR_TOO_LARGE,
    ZIP_ERR_NOMEM
} zip_status;

typedef struct {
    uint16_t method;       /* 0 stored, 8 deflate */
    uint32_t compressed;
    uint32_t uncompressed;
} zip_entry_info;

const char * zip_status_reason(zip_status status);

/* Reads an entry from the ZIP archive. On success, *out is allocated with exactly *out_len bytes.
 * Empty entry: *out is set to NULL with *out_len 0. Caller frees *out with free(). */
zip_status zip_read_entry(const char * path, const char * entry_name,
                          unsigned char ** out, size_t * out_len, zip_entry_info * info);

/* Same, with caller-chosen size limits (for large members such as images).
 * zip_read_entry() uses ZIP_MAX_COMPRESSED_BYTES / ZIP_MAX_UNCOMPRESSED_BYTES. */
zip_status zip_read_entry_limited(const char * path, const char * entry_name,
                                  uint32_t max_compressed, uint32_t max_uncompressed,
                                  unsigned char ** out, size_t * out_len, zip_entry_info * info);

zip_status zip_list_entries(const char * path, char *** names, size_t * count);
void zip_free_names(char ** names, size_t count);

/* Standard CRC-32 (ISO-HDLC / PNG / ZIP). Pass crc = 0 for fresh checksum. */
uint32_t zip_crc32(uint32_t crc, const void * data, size_t len);

#endif /* ZIP_READER_H */
