#ifndef AUDIO_DR_CONFIG_H
#define AUDIO_DR_CONFIG_H

/* Every translation unit that sees dr_flac types must include this before
 * dr_flac.h: the value changes the layout of struct drflac. */
/* Default (4096 bytes) is dr_flac's own general-purpose tuning -- its header
 * comment notes "diminishing returns after about 4KB" for a typical fast
 * onRead() implementation, but local playback here reads from an SD card,
 * where each refill is a real synchronous read on the audio thread, not a
 * memory copy. A 24-bit/96kHz FLAC can run 2.5-4Mbps compressed vs ~1Mbps
 * for 16-bit/44.1kHz, so the same 4KB cache holds proportionally less
 * playback time and empties (forcing a refill) proportionally more often
 * for exactly the high-resolution files a real user reported stutter on.
 * Quadrupling to 16KB trades a modest per-open-decoder RAM cost for fewer
 * mid-playback SD reads on the highest-bitrate files. Must be a multiple of
 * 8 per dr_flac.h's own requirement. */
#define DR_FLAC_BUFFER_SIZE 16384

#endif
