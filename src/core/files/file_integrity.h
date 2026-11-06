#ifndef FILE_INTEGRITY_H
#define FILE_INTEGRITY_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
/* Hash from the current offset to EOF; the stream remains open. */
bool file_sha256_stream(FILE *file, uint64_t *size, char hex[65]);
bool file_sha256(const char *path, uint64_t *size, char hex[65]);
bool filesystem_available_bytes(const char *path, uint64_t *bytes);
/* Unsupported directory sync succeeds with durable=false; real errors fail. */
bool filesystem_sync_directory(int fd, bool *durable);
bool filesystem_sync_parent(const char *path);
#endif
