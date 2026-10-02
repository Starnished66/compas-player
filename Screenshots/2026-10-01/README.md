# Device UI walkthrough — 2026-10-01

Open `index.html` for the screenshot gallery. Device captures are actual R1 framebuffer images (480 × 800). The separate `layout-previews/` folder contains host renders for board sizing checks, clearly distinguished from device captures. `capture-log.jsonl` records capture timestamps and automated inputs; some discarded diagnostic captures still appear in the log.

## Coverage

- Quick Setup: welcome, language selection and immediate Spanish translation, timezone map, connected Wi-Fi, suggested plugins, More list, installation confirmation/progress/results, music scan, completion.
- Music: files, artists, artist albums, album songs, album artists, genres, genre songs, all songs, playlists, favorites, recently added, Now Playing, lyrics, queue and track details.
- Settings: Sound, PEQ/band/filter/profile/graph controls, Playback, Car Mode, Display/Appearance, Power, Library, System, Plugin Manager/Store, firmware update popup, support QR and developer options.
- Wireless: Wi-Fi, network information, hidden network entry, DNS, import QR, Bluetooth and its advanced/DAC/codec/sample rate menus, AirPlay, DLNA, remote, USB mode and DAC availability guard.
- Books: native text reader, favorites, Audiobooks, EPUB library/chapters/reader.
- Plugins with their own pages: Audiobooks, AutoEQ, EPUB Reader, Extended Sleep Timer, Gain Mode, Last.fm, Lock Screen, Loudness Boost, MSEB, Net Radio, Play Through, Podcasts, Qobuz, Sound Profiles, Themes and Tidal.
- Home Background and LED Volume Meter are event driven extensions without separate settings pages. They were enabled during the audit; screenshots do not validate physical LED behavior.

## Findings and fixes

- Empty Subsonic playlists incorrectly produced “Unexpected library response.” Valid empty responses now succeed; the saved server’s empty playlist screen was checked after flashing.
- EPUB page counters overlapped the home indicator. Reader height now reserves its inset; `plugins/epub-reader-footer-fixed.png` shows the corrected counter.
- Welcome overview icon/text pairs sat close to the curved left edge. Each pair is now centered within its pill, including Music, with a measured icon column, a 10 px gap and a label width capped for wrapping. Centering and bounds checks passed for all three boards in English and Spanish.
- Plugin text input cancellation now also follows removal of its navigation stack entry, while preserving an input retained by a chained prompt. AutoEQ search, results and profile details were checked on the device.

- Choose plugins rebuilt its list after each checkbox change and jumped to the top. It now restores the list offset after layout. Host checks passed for selection, deselection and fresh opening on all three board configurations; device captures show the bottom rows remain in place through repeated toggles. The eight choices selected before this update were restored and verified.

## Limits and follow-ups

- Physical testing uses R1 only. R3 Pro II and R3 2025 builds, package contents and welcome layout checks passed; no hardware screenshots were taken for those boards.
- Installed plugins include older versions. Only Gain Mode and AutoEQ were freshly installed in this setup test. This gallery documents installed versions, not a full latest-store update test.
- Qobuz, Tidal and Last.fm account-dependent screens need account credentials; their login/configuration screens were captured.
- USB DAC activation is blocked while ADB is active; the player’s guard was captured. No destructive factory reset, power-off or additional firmware update was selected during menu testing.
- No OPML file was available for podcast import. Its empty-file feedback was captured.
- The picture EPUB cover is blank in the grid; image rendering needs a separate investigation. The ordinary EPUB text reader and footer were verified.
- Several long row labels use marquee scrolling, including paragraph-like rows in Play Through help. Empty Subsonic playlists still have no explanatory empty-state message.

The existing user library and settings backups were preserved. Temporary plugin enablement and the normal 30-second screen timeout are restored. The player is left at Welcome for reviewing the overview; its existing library and network settings are preserved. No commit or push was made during this audit.

## Theme audit and approval drafts

Open [the theme gallery](themes/index.html) locally to compare all 13 existing themes plus Default and ten new approval drafts on R1, R3 Pro II and R3 2025. These are host renders using the actual Home builder and native icons; illustrative status numbers and gesture chrome are added by the fixture. R3 renders are sizing checks, not hardware captures.

The audit corrected Home height allocation, scaled widths, spacing and icons, gesture-area clearance, low-contrast theme text and chevrons, and white status text on light backgrounds. All 72 layouts passed row/label bounds checks at the small font tier. Higher font tiers retain safe minimum heights and allow scrolling when needed. Geometry results are in `themes/geometry-audit.json`; palette changes are in `themes/contrast-audit.json`. Proposals 1, 2, 3, 4, 6, 7, 8 and 10 have since passed physical R1 comparisons and were published individually in Themes 3.9–3.16. [Actual device captures and release records](themes/README.md) are available. Proposals 5 and 9 remain drafts; existing-theme palette edits remain pending locally.
