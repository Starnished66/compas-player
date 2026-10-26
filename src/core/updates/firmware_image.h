#ifndef FIRMWARE_IMAGE_H
#define FIRMWARE_IMAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Validate the ISO9660 .upt envelope and the OTA chunk checksums. */
bool firmware_image_validate(const char *path, char *error, size_t error_size);

#endif
