# Bluetooth metadata and the firmware bridge

Compás exports standard MPRIS metadata through its own BlueZ player. Some
firmware versions also register a stock `sys_server` player. BlueZ can keep
that first registered player selected for an accessory, so both providers
must send valid metadata.

The R1 and R3 II 2025 daemons inspected for issue #135 build `xesam:artist`
as a string. BlueZ requires an array of strings: it accepts the preceding
title, rejects artist, and stops before album and duration. Their metadata
getters also build genre as a string. R3 Pro II's older daemon has no MPRIS
metadata bridge; Compás supplies its standard player directly.

## Firmware compatibility fix

`scripts/base_image/sys_server_mpris_compat.c` is a small shared library
preloaded only into `sys_server`. It preserves GLib's variant builder
behavior, converting string-valued `xesam:artist` and `xesam:genre` dictionary
entries into string arrays. Already valid arrays and other entries are
unchanged. Empty strings become empty arrays. This covers both metadata
signals and getters without changing the stock executable or socket protocol.

The library clears its preload environment after loading so hardware
utilities launched by the daemon do not inherit it. It does not replace the
daemon's radio, storage, power, or other services.

Build with the same glibc MIPS compiler and staged GLib used for the base
image (see `scripts/build_base_glib.sh`):

```sh
scripts/build_sys_server_mpris_compat.sh --stockroot /path/to/squashfs-root
scripts/prepare_sys_server_mpris_overlay.sh /path/to/squashfs-root /path/to/overlay
```

Environment overrides are `BASE_CROSS_PREFIX`, `BASE_GLIB_STAGE`,
`BASE_STOCK_ROOT`, and `BASE_SYS_SERVER_MPRIS_COMPAT_BUILD_DIR`. The output
library is under `scratch/base-upgrade/sys-server-mpris-compat/stage/usr/lib`.
The overlay helper copies the library and adjusts `S50sys_server` in the
output tree. It refuses to modify the stock tree and skips daemons without
the relevant GLib import. Both base-image and Bluetooth overlay preparation
call this helper automatically.

This fix must be included in a firmware image. Updating only the Compás
executable does not update the daemon's launch script or install this library.

## Duration and verification

Decoder startup is asynchronous. Compás republishes settled metadata and
position to the supported legacy bridge when the track snapshot changes,
including when duration becomes available. Those socket operations run on
the Bluetooth dispatch thread. Fields are bounded to the daemon's buffers,
preserving UTF-8 boundaries and replacing embedded protocol delimiters.

Protocol fixtures reproduce the title-only failure with `artist: s` and
accept all fields with `artist: as`. Shared-library checks should also cover
getters, signals, empty strings, existing arrays, unrelated variant formats,
ownership, and preload isolation. An accessory test must establish which
registered player it addresses; inspecting only Compás's correct MPRIS
object cannot prove the car reads that object. The issue reporter's car and
firmware have not been reproduced here.

## AVRCP cover art sender

The firmware carries a backport of Jan-Michael Brummer's BlueZ v3 cover-art
series ([upstream PR 2500](https://github.com/bluez/bluez/pull/2500), commit
`f564a391812ee22092d379939d4d55ca74a44afa`). It is upstream work in progress,
with local changes for this embedded runtime.

Compás owns `org.mpris.MediaPlayer2.compas` on the system bus and exposes its
player at `/org/mpris/MediaPlayer2`. Metadata includes `mpris:artUrl` only
when a JPEG is ready. The existing cover worker exports decoded artwork as a
baseline 200 × 200 YCC422 JPEG with sRGB EXIF metadata and a single segment
for each table type. Embedded images, sidecars and streamed covers use the same
path. No image encoding or filesystem writes run on the GUI thread.
Track tokens prevent a late cover from being published for another track;
layout reloads retain the current published image. Same-album changes reuse the
export by generated-cover file identity without repeating decode/reflection work.
Current no-art results clear publication with the same stale-token guard.
The private RAM cache
`/tmp/compas-bt-art` retains at most four images (aggregate limit 1 MiB).

Compás allocates unique seven-digit handles in JPEG filenames, using an atomic
RAM counter that survives player restarts within a boot. BlueZ and the responder
extract that handle from the artwork URL.
A cover-only `obexd` watches Compás metadata asynchronously and serves BIP
GetImageProperties, GetImage and GetLinkedThumbnail from that cache. It only
opens regular files in the private cache, refuses writes, and uses an
encrypted L2CAP transport. Its advertised PSM follows the actual listener,
including startup in either order and responder removal. The firmware prefers
the complete Compás player over the stock metadata fallback while Compás runs.

`scripts/build_base_bluez.sh` enables the cover-only OBEX build: unrelated
phonebook, message, file-transfer and desktop client plugins are omitted, so
no libical dependency is added. `scripts/prepare_bt_cover_art_overlay.sh`
packages the responder, system bus policy and `S81compas_obex` init script;
the base, player Bluetooth and R3 Pro II overlay recipes include this helper.
All three boards use the same MIPS32r2 hard-float runtime. Player-only updates
cannot install this feature: the patched Bluetooth stack must be in the base
firmware too.

Before release, verify with an AVRCP 1.6 cover-art capable controller:

1. Pair, play a track with artwork, and fetch image properties and its JPEG.
2. Change tracks, including a track with no art; verify no stale handle appears.
3. Reconnect and restart each daemon; verify the advertised PSM stays valid.
4. Check transport buttons, metadata, codec playback and accessory volume.

A controller that does not implement AVRCP cover art continues to receive
normal metadata and audio. Upstream's VW/Fairphone test report does not verify
these firmware adaptations on our hardware.

Thumbnail details follow [AVRCP 1.6.3 section 5.14.2.2.1](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2024/10/AVRCP_v1.6.3.pdf).
GetImage accepts native JPEG requests, empty encoding/pixel preferences, and
pixel ranges containing 200 × 200; unsatisfiable requests return Not Acceptable.
Older base images that deny the new MPRIS name retain the existing player
provider under its unique bus name; cover art requires the upgraded base.

The init hook validates startup synchronously, including a one-second grace
period to detect immediate daemon failure before reporting success.
