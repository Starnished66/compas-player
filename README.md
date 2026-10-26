# Compás Player

<p align="center">
  <strong>A community-built music player for HiBy devices, with room to make it your own.</strong><br>
  Open source · Touch-friendly · Extendable with Lua plugins
</p>

<p align="center">
  <a href="LICENSE"><img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-blue.svg"></a>
  <a href="https://github.com/Starnished66/compas-player/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Starnished66/compas-player"></a>
  <a href="https://github.com/Starnished66/compas-plugins"><img alt="Lua plugins" src="https://img.shields.io/badge/plugins-Lua-7f52ff.svg"></a>
</p>

<p align="center">
  <a href="#see-the-player">Screenshots</a> ·
  <a href="https://github.com/Starnished66/compas-page/blob/480680bfab5d158f0bc064316dd1f15e2fd3907e/manual/README.md">User manual</a> ·
  <a href="#quick-setup">Quick setup</a> ·
  <a href="#plugins">Plugins</a> ·
  <a href="#install-and-update">Installation</a> ·
  <a href="#build-from-source">Build from source</a>
</p>

Compás Player is an open-source replacement for HiBy OS's closed-source `hiby_player`, written in C with [LVGL](https://lvgl.io/). It is usable on real HiBy R1 hardware. Builds also exist for the R3 Pro II and R3 II 2025, but those builds have not been tested on their target hardware.

## Get started

1. Check the [device support notes](#device-support) and make sure the build is for your exact model.
2. If this is your first install, follow [First install](#first-install-full-firmware) to flash the full firmware package once.
3. For later player updates, download the matching player binary from [the latest release](https://github.com/Starnished66/compas-player/releases/latest) and copy it to `SD/.compas/compas_player`. See [Player-only update](#player-only-update) for details.
4. Browse and install add-ons in **Settings → System → Plugin Manager → Plugin Store**, or visit the [compas-plugins catalog](https://github.com/Starnished66/compas-plugins).

The first full firmware install replaces part of the device firmware. Keep a known-good recovery image and read the device-specific notes before flashing.

## See the player

Captured on a HiBy R1 running the current development build; published releases may have a different interface.

Browse the [full UI walkthrough](https://github.com/Starnished66/compas-page/blob/480680bfab5d158f0bc064316dd1f15e2fd3907e/images/screenshots/2026-10-01/README.md) for coverage and findings. For the complete gallery, clone the [documentation repository](https://github.com/Starnished66/compas-page/tree/480680bfab5d158f0bc064316dd1f15e2fd3907e), check out that commit, and open `images/screenshots/2026-10-01/index.html` locally.

**Welcome & plugin selection**

<p>
  <img src="https://raw.githubusercontent.com/Starnished66/compas-page/480680bfab5d158f0bc064316dd1f15e2fd3907e/images/readme/welcome.png" width="210" alt="Welcome and initial setup screen">
  <img src="https://raw.githubusercontent.com/Starnished66/compas-page/480680bfab5d158f0bc064316dd1f15e2fd3907e/images/readme/plugin-selection.png" width="210" alt="Initial plugin selection screen">
</p>

**Now Playing · Lyrics · Settings**

<p>
  <img src="https://raw.githubusercontent.com/Starnished66/compas-page/480680bfab5d158f0bc064316dd1f15e2fd3907e/images/readme/now-playing.png" width="210" alt="Now Playing screen">
  <img src="https://raw.githubusercontent.com/Starnished66/compas-page/480680bfab5d158f0bc064316dd1f15e2fd3907e/images/readme/lyrics.png" width="210" alt="Synchronized lyrics screen">
  <img src="https://raw.githubusercontent.com/Starnished66/compas-page/480680bfab5d158f0bc064316dd1f15e2fd3907e/images/readme/settings.png" width="210" alt="Settings screen">
</p>

## Quick setup

A fresh install or factory reset opens a guided setup, starting with a short musical welcome animation.

1. **Language:** choose the language used throughout setup and the player.
2. **Time zone:** select your region with a map preview.
3. **Wi-Fi:** pick an available network or add a hidden network.
4. **Plugins:** select suggested Gain Mode and AutoEQ plugins, or open **More** to browse the live Plugin Store catalog and its descriptions. Confirm your selection and follow installation progress.
5. **Your music:** scan the SD card to build your library, then continue to the main screen.

Wi-Fi and plugins can be skipped and configured later. Failed plugin installations get one retry, with any remaining failures shown before setup finishes.

## What it can do

- **Play music:** FLAC, MP3, WAV, AIFF, DSD, AAC, ALAC, APE, Opus, WavPack, CAF and more; gapless playback, crossfade, ReplayGain, a 10-band PEQ, queue controls, hardware buttons, Bluetooth audio and USB DAC mode.
- **Organize a library:** browse files, artists, albums and playlists; scan incrementally; use Rockbox tagcache; read synced lyrics and plain-text books; handle SD card removal and insertion. Audiobook features are available as a plugin.
- **Connect and stream:** Subsonic-compatible streaming over HTTPS, downloads, DLNA renderer, AirPlay through stock protocols where possible, Wi-Fi and Bluetooth remote control, and Wi-Fi music import.
- **Tune the device:** charge limit and Safe Charging, suspend or idle shutdown, car mode, USB Storage/DAC/ADB selector, timezone and charge LED controls.
- **Shape the interface:** swipe navigation, quick controls, built-in Themes picker, a customizable Home screen, a More screen (Plugins, Themes, Now Playing layouts, USB DAC, Bluetooth DAC), and non-Latin text including Cyrillic, Japanese, Korean and Thai.

Android client authors can use the [Remote Control API v1 contract](https://github.com/Starnished66/compas-page/blob/480680bfab5d158f0bc064316dd1f15e2fd3907e/docs/REMOTE_CONTROL_API.md).

Desktop tools can prepare a card with `compas-sdtool`, which builds the music database and album covers on a computer using the player's own library code; see [compas-sdtool and the card files](https://github.com/Starnished66/compas-page/blob/480680bfab5d158f0bc064316dd1f15e2fd3907e/docs/SDTOOL.md).

## Themes

Compás includes a built-in system theme picker accessible from **More → Themes**. Themes use the existing `.theme` key-value format and can be placed in `SD/Themes/` (including subfolders); no `Themes.lua` plugin is required to use them. Theme packs can also be browsed and installed through the Plugin Store catalog.

## Plugins

Plugins add screens, settings, streaming sources and other features using Lua. Install a ready-made plugin through the on-device Plugin Store or download it from the [compas-plugins repository](https://github.com/Starnished66/compas-plugins). The Store catalog is published as [`index.json`](https://github.com/Starnished66/compas-plugins/releases/latest/download/index.json); packaged plugin downloads are attached to [the latest catalog release](https://github.com/Starnished66/compas-plugins/releases/latest).

Start with **AutoEQ** for downloadable headphone EQ profiles, **Gain Mode** for low/high gain controls, **Audiobooks** for bookmarks and listening progress, or **Podcasts** and **EPUB Reader** for more ways to enjoy your library. Browse the repository for the full catalog and each plugin's requirements.

For manual installation, follow the [plugins repository's installation instructions](https://github.com/Starnished66/compas-plugins#installing-by-hand): copy the `.lua` file to `SD/.plugins/` and any companion files to the destinations listed in its `store.json`. Then choose **Refresh Plugins** in **Settings → System → Plugin Manager** or restart the player. Plugins can be enabled or disabled without removing their files. No C toolchain, player rebuild or firmware reflash is needed.

Developers can start with the [plugin API guide](https://github.com/Starnished66/compas-page/blob/480680bfab5d158f0bc064316dd1f15e2fd3907e/docs/PLUGINS.md), browse [published plugins](https://github.com/Starnished66/compas-plugins/tree/main/plugins), and browse [standalone examples](https://github.com/Starnished66/compas-plugins/tree/main/examples). The guide documents the versioned `plugin.*` API for UI extensions, playback, library access, HTTP, themes and PEQ. For Now Playing layout customization, see [Player layouts](https://github.com/Starnished66/compas-page/blob/480680bfab5d158f0bc064316dd1f15e2fd3907e/docs/PLAYER_LAYOUTS.md).

Plugin implementations, plugin examples and their assets belong in `compas-plugins`; this repository keeps the player and its plugin API.

## Device support

| Device | Build target | Screen | Status |
| --- | --- | ---: | --- |
| HiBy R1 | `r1` (default) | 480×800 | Supported and actively tested on real hardware, including display, touch, audio, buttons, Bluetooth and Wi-Fi. |
| HiBy R3 Pro II | `r3proii` | 480×720 | Board-specific build exists; not tested on R3 Pro II hardware. Not an R1 binary. |
| HiBy R3 II 2025 | `r3ii_2025` | 320×480 | Board-specific build exists; not tested on R3 II 2025 hardware. Not an R1 binary. |

Other HiBy models are not supported unless explicitly listed and tested. Build output names for R3 models have a board suffix so they do not overwrite R1 binaries.

## Install and update

### First install: full firmware

The full `.upt` package installs the open-source bootloader and the complete firmware image. This step is required once before player-only updates will work. On stock firmware, the SD card update path is not read.

Download the R1 `r1.upt` from [releases](https://github.com/Starnished66/compas-player/releases/latest), keep a known-good recovery image, and install the package using the device's recovery updater. Use only an image intended for your model. The full package includes repository-tracked UI assets, fonts and `firmware/overlay/` files, along with the firmware files from its approved base image.

### Full firmware updates in Compás

Open **Settings → Updates → Firmware Update**. Online updates use only GitHub's designated latest supported published release, with no fallback to older releases. A saved online download requires Wi-Fi when installing so the player can check that it still matches the latest release.

For an offline update, place the firmware image in the SD card root using the filename for your model: `r1.upt`, `r3proii.upt`, or `r3ii_2025.upt`. Choose **Install from SD card**. Compás checks that the named file is a regular file, then moves other root `.upt` files aside so recovery sees only the selected image. Stock recovery checks the package contents and whether the image is compatible with the player; naming another model's image does not make it compatible. OTA downloads are checked for package structure and chunk checksums before they are placed on the card.

Preparation runs in the background and records its phases and helper output in `SD/.compas/ota/update.log`. If preparation takes longer than expected, keep the player powered and wait: flash helpers are allowed to finish rather than being interrupted. Recovery performs the actual firmware installation after reboot.

### Player-only update

After the open-source bootloader is installed, use this for normal player updates. Download the player binary for your exact board from [the latest release](https://github.com/Starnished66/compas-player/releases/latest), or build it yourself, then copy it to this exact SD-card path:

```text
SD/.compas/compas_player
```

Keep the existing `.compas` directory and place the binary inside it. Open **Settings > Updates > Player Update > Install from SD card** to replace the player in internal storage and restart. The minimal `compas_loader` also retains automatic startup installation when an update is placed at `SD/.compas/compas_player`. You can also use **Check for online update** in that menu to download the matching binary from the latest GitHub release. **A player-only binary update does not include updated assets, fonts or firmware overlay files.** If an update requires those files, install a new full firmware image.

SD updates require a matching device-model marker in the binary and cannot be older than the player packaged in the firmware. Rejected startup updates are preserved with a `.rejected` suffix so they are not retried on every boot.

## Build from source

The Makefile builds the player and bootloader for R1, R3 Pro II, and R3 II 2025. Target builds need the MIPS musl toolchain (`mipsel-linux-musl-gcc`); `make` bootstraps the pinned vendor sources and LVGL as needed.

Install Git, Make, a native C/C++ compiler, CMake, Python 3, patch, curl, xz and pkg-config. Put the cross-toolchain on `PATH`; the verified installer can prepare it:

```sh
toolchain_dir=$(scripts/tools/toolchain/install_mips_toolchain.sh "$HOME/.cache/compas-toolchain")
export PATH="$toolchain_dir/bin:$PATH"
```

Build both executables for the selected board:

```sh
make target BOARD=r1
make bootloader BOARD=r1

make target BOARD=r3proii
make bootloader BOARD=r3proii

make target BOARD=r3ii_2025
make bootloader BOARD=r3ii_2025
```

R1 and R3 Pro II use the combined Compás/Rockbox bootloader. R3 II 2025 uses the standalone Compás loader. R3 outputs include the board name in their filenames, so they do not overwrite R1 outputs.

An optional host build uses SDL2 (`libsdl2-dev` on Debian/Ubuntu) and the native C/C++ toolchain:

```sh
make host
./compas_player_host
```

The host executable needs runtime fonts and theme assets supplied separately from a firmware resource tree; this repository no longer contains those runtime resources. Executable builds do not create firmware packages.

<details>
<summary>Firmware images and releases</summary>

Firmware image packaging and early-splash builds now live in a separate private firmware repository. It consumes the player/bootloader binaries built here, an early-splash executable, and an approved external `.upt` base image for the matching board. Proprietary base images are not included in this repository.

The scheduled daily GitHub Actions workflow builds player and bootloader executables for all three boards and uploads them as workflow artifacts. It does not package firmware images or use base-image secrets. There is no scheduled weekly release workflow; production releases are published manually after validation.

Keep a known-good recovery image and use binaries and firmware packages only for the exact device model. R3 Pro II and R3 II 2025 builds have not been validated on target hardware.

</details>

## Repository layout

| Folder | Contents |
| --- | --- |
| `src/` | Player and bootloader executable source, including generated runtime C data. |
| `scripts/` | Executable build helpers, toolchain setup, and source generators. |
| `patches/` | Tracked LVGL and codec dependency patches and generated-font goldens. |
| `firmware/bootloader/` | Rockbox integration patch used by the combined bootloader build. |
| `licenses/` | Preserved license and attribution files for assets moved to the firmware repository. |
| `*_vendor_config/` | Configuration headers for vendored dependencies. |
| Vendor source directories | Third-party sources compiled into the player and bootloader. |

## Support the project

Help improve Compás by testing builds, reporting reproducible bugs, contributing code or documenting hardware behavior. Donations help fund development and device testing: [Donate via PayPal](https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=josegarita%40protonmail.com&currency_code=USD).

## Acknowledgments

Compás Player builds on the HiBy modding community's earlier work.

- [hiby-modding/hiby_os_crack](https://github.com/hiby-modding/hiby_os_crack) provided the `.upt` unpacking and repacking tools, QEMU setup, device layouts and reverse-engineering work that made modified firmware possible. This project was originally developed alongside that toolkit.
- [bidhata/Hiby-R1-Mod](https://github.com/bidhata/Hiby-R1-Mod) provided an unpacked mirror of R1 stock firmware used locally as the host simulator's `theme2` asset source and to identify stock runtime fonts.

Thanks to everyone who has contributed to the HiBy reverse-engineering ecosystem.

## License

This project is licensed under the [GNU General Public License, version 3](LICENSE). The project statically links [FAAD2](https://github.com/knik0/faad2) for AAC decoding, which is GPLv2-licensed. Other major dependencies, including `dr_libs`, LVGL and tinyalsa, use permissive licenses. A deployment that needs to avoid this copyleft combination would need to remove AAC support or replace FAAD2 with a compatible decoder; isolating the existing FAAD2 code does not change the resulting binary's license obligations.

WavPack uses the BSD-licensed [libwavpack](https://github.com/dbry/WavPack), and CAF uses LGPL-licensed [libsndfile](https://github.com/libsndfile/libsndfile). The APE decoder includes an adapted [FFmpeg](https://ffmpeg.org/) decoder; its attribution is preserved in the source and its LGPL license is included in [APE_FFMPEG_CORE_LICENSE.txt](src/audio/decoders/ape/APE_FFMPEG_CORE_LICENSE.txt).
