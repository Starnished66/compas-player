# Source layout

`src/` groups the player, device services, and bootloader by responsibility. Within a group, folders keep closely related implementation files, headers, and small component checks together. Public interfaces and behavior are unchanged by this layout.

## Main areas

- `audio/` contains the playback engine, output paths, timing and visualization helpers, effects, and format-specific decoders, demuxers, readers, and probes.
- `library/` contains file browsing and path handling, metadata parsing/catalog/database/scan code, tagcache, artwork, playlists and queue persistence, remote library state, and supporting workers.
- `network/` contains HTTP and streaming clients, Wi-Fi and Bluetooth, AirPlay, remote control and import, device services, firmware and plugin stores, and the remote-control web app source.
- `hardware/` contains input handling, device configuration, indicators, power management, and USB audio/DAC control.
- `core/` contains shared settings and persistence, device information, process helpers, storage and updates, lifecycle and logging, time, text utilities, themes, and UI support code used outside the screens.
- `ui/` contains screens and widgets, grouped by appearance, home, library, network, player, plugins, settings, shell, storage, text, and updates. `ui/player/` separates artwork, layout, lyrics, queue, screen, settings, and transport code.
- `plugins/` contains plugin management and lifecycle, Lua runtime and JSON support, storage, and API bindings grouped by audio, I/O, library, and UI.
- `bootloader/` contains the selector, display and startup code, installation, scanning, storage, and Rockbox integration. Selector rendering, controller logic, and generated assets have separate folders.
- `tools/sdtool/` contains the SD card indexing utility and its CLI, parsing, indexing, media, output, compatibility, and time helpers.
- `app/main.c` is the application entry point. `ui/application/gui.c` coordinates the screens; source files are kept inside feature subfolders.

## Working in the tree

Keep each project-owned source folder to at most ten direct files. Split by responsibility when a group grows; do not create numbered overflow folders. Vendor trees and generated build directories are outside this convention.

Keep a `.c` implementation beside its corresponding `.h` when they form a module. Quoted includes between files in different subfolders use relative paths; update those paths when moving a file. The Makefile has explicit source lists for the application, bootloader, and selector simulator, so adding or moving a translation unit requires updating the relevant list. Do not assume source files are discovered automatically.

Small module checks and specifications are generally colocated with the implementation they exercise. Broader integration tests and their runners live under the repository's `tests/` and `scripts/` trees. When relocating a test, update its runner and any source/object paths it names.

Some files are generated or selected by build rules. For example, `scripts/generators/webapp/generate_remote_webapp.py` builds the remote-control header from `network/webapp/`; selector logos, icons, and fonts are generated or verified by scripts under `scripts/`. Their input and output paths, and any board-specific source selections in the Makefile, must stay aligned with the tree.
