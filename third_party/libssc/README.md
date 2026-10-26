# Native SSC encoder

This library contains a scalar C translation of the recovered Samsung Scalable
Codec encoder and its coefficient tables. The implementation runs native host
or MIPS instructions; it does not execute the original ARM image.

The public API in `ssc_encoder.h` accepts stereo signed PCM held in `int32_t`
containers. It supports 16-bit at 44,100 or 48,000 Hz (192 kbit/s), 24-bit at
48,000 Hz (88/96/128/192 kbit/s), and 24-bit at 96,000 Hz
(152/291/328/584 kbit/s). Each call consumes exactly 864 stereo frames;
24-bit values are right-justified signed integers. The wrapper copies and
validates PCM before encoding, checks output capacity against the exact
rate/depth/bitrate frame size before advancing state, and permits one live
encoder per process because the underlying working tables are global.

The source derives from the recovered `analysis/mips_port/scratch/ssc_full.c`
and `analysis/mips_port/ssc_image_data.h` in the companion reverse-engineering
workspace. The preserved eight-byte state slots are intentional on MIPS32.
Unaligned pointer aliases and packed vector storage represent the original
unaligned accesses. Builds require GNU-compatible C, signed wrap semantics,
and disabled strict aliasing.

Corrections established against the genuine ARM encoder include contiguous
pitch autocorrelation/LPC arrays, allocation-array bounds, rounded 32-bit cosine
arithmetic, and ARM signed-subtraction overflow semantics. Regression evidence
covers silence, tones, independent stereo, impulses, noise, ramps, extremes,
and mixed sequences including swept tones at the supported modes and bitrate
tiers. Other formats and bitrates are rejected.

Build with CMake. `scripts/build/base/bluetooth/audio/build_base_bt_codecs.sh` installs the MIPS shared
library, public header, and `ssc.pc` in the existing codec developer stage.
Reference verification harnesses and vectors remain outside the repository.
