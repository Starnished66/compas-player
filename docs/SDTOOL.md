# compas-sdtool and the files tools may write on the card

`compas-sdtool` builds the music database and album-cover caches on a computer,
so a large card can be prepared in seconds instead of minutes on the player.
It is the player's own library code (tagcache, metadata, albumart, cover_decode)
built as a command-line program with no UI, audio or device dependencies, so a
card it prepares is read by the player exactly as if the player had scanned it.

This page also documents the small files on the card that external tools may
write: `.compas/device.json` (written by the player, read by tools) and the plugin
files `.plugins/.store_installed` and `.plugins/.disabled`.

## Building

```sh
make sdtool                          # build_sdtool/compas-sdtool (Linux, macOS)
make sdtool SDTOOL_TARGET=windows    # build_sdtool_windows/compas-sdtool.exe
make test-sdtool                     # end-to-end test on a generated card
```

The native build needs the system `cc`, `make`, `cmake` (for the bundled
WavPack and libsndfile) and the vendored libraries the main Makefile fetches.
The Windows build is a static MinGW-w64 executable; it needs
`x86_64-w64-mingw32-gcc` and a cmake that targets it. For example, in a
container: `gcc-mingw-w64-x86-64`, `make`, `cmake`, `python3` on Ubuntu, then
`make sdtool SDTOOL_TARGET=windows`. The macOS build is written for clang and
needs only two names the library lacks there (`src/tools/sdtool/compat.h`); it
has not been run on a Mac.

The code that differs per system is confined to `src/tools/sdtool/compat*.{h,c}`,
which the build force-includes on Windows and macOS only. Linux builds use the
player's sources unchanged.

## Commands

```
compas-sdtool info <card-root>
compas-sdtool scan <card-root> [--full] [--mtime-offset SECONDS]
compas-sdtool art  <card-root> [--board r1|r3proii|r3ii_2025] [--mtime-offset SECONDS]
```

`<card-root>` is the folder the card is mounted at, or a drive such as `E:\`.
It plays the part of `/data/mnt/sd_0` on the player. The database stores the
player's paths (`/data/mnt/sd_0/Artist/Album/01.flac`), never the computer's.

Standard output is JSON, one object per line. Human-readable logs go to
standard error. A failure prints one `error` line and exits nonzero.

| Command | Output |
| --- | --- |
| `info` | One object: `{"sdtool_version":"1.0.0","database_format":5,"generation":12,"songs":4032}`. A card with no database reports `generation` 0 and `songs` 0; that is not an error, and nothing is created. |
| `scan` | `{"event":"progress","phase":"walk"\|"tags"\|"save","done":N,"total":N}` lines, then `{"event":"done","command":"scan","files":N,"parsed":N,"unchanged":N,"failed":N,"skipped":N,"songs":N,"generation":N,"written":true\|false}`. |
| `art` | `{"event":"progress","phase":"art","done":N,"total":N}` lines, then `{"event":"done","command":"art","board":"r1","albums":N,"written":N,"unchanged":N,"no_art":N}`. |
| any failure | `{"event":"error","code":"...","message":"..."}` and exit status 1 (2 for bad arguments). |

Progress lines are limited to about one a second, plus the last one. Error
codes: `usage`, `card_not_found`, `database_open_failed`, `walk_failed`, `no_music_found`, `worker_failed`,
`write_failed`, `bad_board`, `art_failed` (a cache file could not be written; the
rest were), `out_of_memory`.

### scan

`scan` does what the player's boot scan does. It walks the card (skipping the
`Audiobooks` folder, honouring `database.ignore` and `database.unignore`
files), and when every file already has an up-to-date row and no row is left
over, it stops: `written` is `false` and the card is not touched. Otherwise it
reads the files whose size or time changed, drops rows of deleted files and
commits a new generation. `--full` re-reads every file, as the player's
"refresh all metadata" does, and commits only if some row actually changed.

A file is up to date when its size and its time (as moved into the player's
frame, see "File times") equal what was recorded.

### art

`art` builds `.compas/albumart` the way the player's cover warmer does: for each
album, a 72 px and a 480 px BMP named by the album's key
(`v4-<key>.72x72.bmp`, `v4-<key>.480x480.bmp`), from a cover file in the album
folder, else the representative track's embedded picture, else another track's.
The source file time (in the player's frame) is stored in each BMP and an
existing cache is rebuilt only when its source is newer. Albums without art or without artist and album
tags write nothing (`no_art`). Run `scan` first: `art` reads the database.

The cache sizes do not depend on the board, so `--board` only selects what is
reported in `board`; without it the model in `.compas/device.json` is used,
else `r1`. Artist thumbnails are built by the player on demand and are not
written.

## `.compas/device.json`

The player writes this file at startup, only when the contents change, with its
usual atomic sequence (temporary file, flush, rename, flush the folder). It is
skipped when no card is mounted and does nothing on a read-only card.

```json
{"schema":1,"model":"r1","player_version":"Weekly Beta 2026-09-02","plugin_api":17,"database_format":5,"art_cache":"v4"}
```

| Field | Meaning |
| --- | --- |
| `schema` | Version of this file's layout. |
| `model` | `r1`, `r3proii` or `r3ii_2025`. |
| `player_version` | The version label the About screen shows. |
| `plugin_api` | The plugin API the player implements (`PLUGIN_API_VERSION`). |
| `database_format` | Layout of the database the player writes and reads (`TAGCACHE_DATABASE_FORMAT`). A tool must write exactly this format. The value stands for every database layer (master file, generation references, each index family), and the build refuses to compile until it is reconsidered when one changes. Build `compas-sdtool` from the same source as the player on the card; `info` reports the format a build writes. |
| `art_cache` | Cache generation (`ALBUMART_CACHE_VERSION`); the `v4` in `v4-<key>.WxH.bmp`. |

Tools should treat unknown fields as additions and refuse a `schema` they do
not know.

## File times

FAT stores local wall-clock time at 2-second resolution. The player mounts the
card without a `tz=` or `time_offset=` option and never sets the kernel time
zone, so it reads the stored digits as UTC, always with one fixed offset. The
player therefore compares file times exactly and needs no zone handling.

A computer reads the same digits through its own zone, so a database written
from the times it reports would make the player see every file as changed (a
whole-library rescan at boot, and every cover cache looking stale). sdtool
moves the times it records into the player's frame:

| Where the card is read | Conversion |
| --- | --- |
| Windows, FAT or FAT32 volume | add the zone offset `FileTimeToLocalFileTime` applies |
| macOS, `msdos` volume | add the current UTC offset |
| Linux vfat mounted without `tz=` | none (the kernel already reports the raw digits) |
| exFAT, NTFS, anything else | none |

exFAT is not converted yet: how the player's kernel and each computer report
exFAT times has still to be measured on a device, and this page does not claim
that cards on exFAT work across zones. A Linux vfat mount with `tz=` or a
nonzero kernel zone is likewise not corrected automatically.

Two corrections cover what the table misses. `--mtime-offset SECONDS` (`scan`,
`art`) adds a fixed number of seconds to every card file time. And when the card
already has a database the player wrote, sdtool samples its rows, and if most
files of the same size differ from the recorded time by the same whole number
of 15 minutes (at most 26 hours) it adds that difference too. Anything less
clear is ignored.

A wrong correction costs a rescan, never stale data: file times are only ever
compared for equality with what was recorded, so an edited file differs
whatever offset is applied to both. A same-size edit is detected whatever step
its time moved by (the one exception being an edit of most of the library, all
by the same zone-sized step, in one run). Tools that write files to the card
need no zone handling.

## Reading the database

`scan` uses the player's decisions (`library_scan_decision.h`): an unchanged
library is left alone only when the database loaded normally. One that was
recovered from an older generation, or could not be read at all, is rewritten
by the next scan (and replaced only if music is found), after which later scans
leave the card untouched. `info` reads without writing: it creates no folder or
identity marker and migrates no index.

Tags are read in a separate `compas-sdtool parse-worker` process, started by
the scan on every system. A file whose parser crashes or runs past 5 seconds
is reported in the `failed` count and gets no row (an older row survives), as
in the player.

Paths are handled as UTF-8; on Windows they use the `\\?\` form, so paths over
260 characters work, and on macOS file names are normalised to NFC to match
what the player stores.

## `.plugins/.store_installed`

The Plugin Store's record of what it installed, so it can offer updates and
remove files cleanly. A tool that installs or removes store plugins may write it
in the format below; the player rejects a malformed file as a whole and then
treats nothing as installed. It is read and written by
`plugin_store_record_read()` and `record_write_path()` in
`src/network/plugin_store.c`.

```
P <id> <version>
F<TAB><dest><TAB><sha256><TAB><keep>
F<TAB>...
P <id> <version>
...
```

Lines end in `\n` (the last may omit it) and are at most 511 bytes. A `P` line
starts a plugin and is followed by one to 32 `F` lines. Fields:

- `id`: 1 to 63 characters from `a-z 0-9 _ . -`, starting with a letter or digit;
  unique in the file.
- `version`: up to 31 characters, one to four groups of digits separated by
  single dots (`1`, `1.2`, `2.0.1`).
- `dest`: the file's path from the card root, up to 255 characters of
  `A-Z a-z 0-9 . _ - space /`, relative (no leading `/`), with no empty, `.` or
  `..` segment, and no segment ending in a dot or space (vfat and Windows drop
  them, so `.compas./x` would land in `.compas`). It may not be `.compas` or below it, may not name a dot file
  directly in `.plugins` (`.plugins/.x`), and may not end in `.upt`. A
  destination appears once in the whole file, compared case-insensitively.
- `sha256`: 64 lowercase hexadecimal digits, the hash of the installed file.
- `keep`: `1` if the file is left in place when the plugin is removed (user data
  such as a configuration), otherwise `0`.

At most 400 plugins are read. `.plugins` itself must be a real folder and the
record a regular file, not links. Written files use mode 0644.

## `.plugins/.disabled`

Plugins the user switched off in **Manage Plugins**, read at every plugin start
by `plugin_disabled_list_load()` in `src/plugins/plugin_disabled_list.c`. One
plugin per line: the script's file name as it appears in `.plugins` (for example
`weather.lua`), matched case-sensitively.

```
weather.lua
clock.lua
```

A line ends in `\n` or `\r\n`; empty lines are skipped; there are no comments,
no path components and no byte-order mark. Only the first 64 names are kept and
a name must be shorter than 256 bytes (a longer line is split). A missing file
means every plugin is enabled. The player rewrites the file atomically, so an
external tool should write it the same way: a temporary file in `.plugins`, flush,
rename over `.disabled`, flush the folder.
