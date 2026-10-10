# Compas v1.1

Compas v1.1 brings a faster, more capable player experience across the R1, R3 Pro II, and R3II (2025).

- **Custom kernels and drivers:** all three models include custom kernels and rebuilt drivers. R1 includes the custom display and radio drivers; R3 Pro II and R3II (2025) include rebuilt utility drivers and retain their vendor display and radio drivers.
- **Kernel memory support:** all three models enable 24 MiB of LZ4-compressed ZRAM swap at boot and set `vm.swappiness` to 100 to help relieve memory pressure. RAM is allocated as pages are swapped, rather than reserving the entire capacity upfront.
- **Interface and controls:** a refreshed More menu, theme improvements, expanded button controls, asynchronous screen opening, waveform improvements, and previews in the Plugin Store.
- **Audio and playback:** more reliable Opus playback and seeking, improvements to AAC, ALAC and APE, new WavPack and CAF support, and seeking for remote files, including M4A.
- **Library and books:** a new File Manager; audiobook author, series, and podcast browsing; and more reliable resume positions.
- **Plugins:** Jellyfin / Emby, Discover, ListenBrainz, lyrics and cover fetchers, Album Shuffle, radio search, Track Inspector, context and output-aware EQ, bedtime fades, volume limiting, per-album rules, A-B repeat and ABX comparison. Kids Mode and Lock Screen fixes are included here too.
- **Bluetooth and memory:** smoother Bluetooth volume control and reconnects, plus memory use reductions across long-running playback and browsing.
- **Setup and power:** new installations default to QWERTY and suspend-to-RAM after 10 minutes idle. Updates preserve saved preferences. The selected idle action now has a visible indicator.
- **Fixes:** improvements to the player, library, plugin store, setup, and connection behavior.

Download the firmware update for your device and verify it against `SHA256SUMS`.

R1 has been tested on hardware, including Bluetooth playback, volume control and reconnects. The R3 images have passed build and package checks; their new kernel and driver combinations still need hardware validation.
