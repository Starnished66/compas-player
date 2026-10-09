#ifndef COMPAS_OGG_CONFIG_TYPES_H
#define COMPAS_OGG_CONFIG_TYPES_H

/* C99 integer types shared by Linux host and musl/MIPS builds. This is the
 * only generated libogg header needed by the direct-source build. */
#include <stdint.h>

typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;

#endif
