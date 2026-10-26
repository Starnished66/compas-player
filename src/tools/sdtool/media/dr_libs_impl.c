/* The FLAC and WAV tag readers in metadata.c use dr_libs, whose single-file
 * implementations the player compiles into audio.c. */
#define DR_FLAC_BUFFER_SIZE 16384
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
