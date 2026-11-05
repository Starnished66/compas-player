#ifndef CAF_DEMUX_H
#define CAF_DEMUX_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal CAF packet reader for variable-size packetised audio (ALAC): reads
 * the desc, kuki, pakt and data chunks in any order and gives random access
 * to compressed packets. Fixed-size packet formats are rejected. */

typedef struct caf_demux caf_demux_t;

/* Format id of the desc chunk ("alac", "lpcm", ...), null-terminated. Cheap
 * enough to use as a routing peek. False when the file is not CAF. */
bool caf_demux_peek_codec(const char * path, char out_fourcc[5]);

caf_demux_t * caf_demux_open(const char * path);
void caf_demux_close(caf_demux_t * d);

void caf_demux_get_codec_fourcc(const caf_demux_t * d, char out_fourcc[5]);
/* The kuki chunk as stored (the ALAC magic cookie). */
const uint8_t * caf_demux_get_codec_config(const caf_demux_t * d, uint32_t * out_size);

uint32_t caf_demux_get_packet_count(const caf_demux_t * d);
uint32_t caf_demux_get_frames_per_packet(const caf_demux_t * d);
uint64_t caf_demux_get_valid_frames(const caf_demux_t * d);
uint32_t caf_demux_get_priming_frames(const caf_demux_t * d);

/* Reads packet i into buf. False when out of range or it does not fit. */
bool caf_demux_read_packet(caf_demux_t * d, uint32_t index, uint8_t * buf,
                           uint32_t buf_size, uint32_t * out_size);

#endif /* CAF_DEMUX_H */
