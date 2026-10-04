# Compas v1.0

A new look for Compás, from the first welcome screen to your favorite song. This release brings downloadable Now Playing layouts, refreshed themes, guided setup, more languages, and smoother transitions throughout the player.

## Make Now Playing your own

**Player layouts now have their own download gallery**, with picture cards so you can see a design before installing it. Browse **Settings > Display > Player Layout > Layout > Download**, then choose your installed layout in the same Layout menu.

- **Gallery:** artwork-led design with frosted glass and integrated lyrics.
- **Panorama:** full-width artwork, a soft cover fade, and rounded waveform seeking.
- **Vinyl:** record-inspired artwork with an envelope waveform seek bar. Available from the layout catalog rather than preinstalled.
- **Orbit:** a large circular cover with a seek ring around it.
- **Hiby's and Hiby's Graph:** included with the firmware, featuring edge-to-edge artwork and a clearer frosted glass footer; Graph adds an envelope waveform seek bar.
- **Default:** keeps the familiar controls, with refreshed spacing and a frosted background.

Tap the cover to open or close lyrics in every layout. Layout selection remembers your choice, and switching designs preserves the current song information and cached cover art. Preview cards, controls, and display-size variants have been revised for R1, R3 Pro II, and R3II 2025. Layout packages use Plugin API 15 to keep incompatible older firmware from loading them.

## Revised themes and clearer frosted glass

The **Themes** plugin brings revised Home screen layouts and eight new looks: **Porcelain, Midnight Indigo, Cherry Noir, Brass Studio, Arctic Glass, Cobalt Signal, Rose Quartz, and Citrus Slate**. Install or update Themes from the Plugin Store, then select a theme under **Settings > Display > Appearance**. The theme files come with the plugin download.

Frosted backgrounds have cleaner color blending and better sampling of album artwork. Home screens make better use of the available space, with theme and layout sizing adapted to the three supported displays.

## Welcome to Compás

Fresh installs and factory resets open a new **Quick Setup** experience, beginning with a short animated shower of musical notes.

Choose your **language, time zone, Wi-Fi network, plugins, player layout, and music scan** in one guided flow. The time-zone screen includes a map, Wi-Fi lists nearby networks and supports hidden SSIDs, and plugin suggestions include **Gain Mode** and **AutoEQ**. Use **More** to browse the live plugin and layout catalogs.

Plugin installations and the selected layout are applied together at the end, before the library scan. Installation progress is shown, failed downloads are retried once, and any remaining failures are listed. Optional network and plugin steps can be skipped and completed later. Setup finishes on a dedicated completion screen.

## More languages

The player and Quick Setup are available in **English, Spanish, French, Italian, and Brazilian Portuguese**. Selecting a language updates setup immediately; it can also be changed under **Settings > System > Language**. Translation coverage has been expanded across the new screens. Translations are machine-generated drafts awaiting native-language review; third-party plugin text may remain in English.

**Medium** is now the default UI and lyrics text size for fresh installs and resets. Existing saved font choices are retained.

## A better Plugin Store

Plugin descriptions now come from the online catalog and appear before installation, helping you understand what each extension does. Install, update, enable, disable, and remove plugins through **Settings > System > Plugin Manager**.

Player layouts have a separate download page and are hidden from the regular plugin list. Catalog refresh, preview loading, selection scrolling, replacement installs, and failed-install handling have been improved.

Updated optional plugins available from the Store include:

- **AutoEQ:** search for headphone profiles, download them, and load them from the equalizer's profile list.
- **Audiobooks:** redesigned browsing and listening controls, bookmarks and listening progress, plus an optional **Skip by 30 seconds** setting for physical and Now Playing Next/Previous controls.
- **Podcasts:** clearer browsing, improved icons, and wrapped episode titles with enough row height to read them.
- **EPUB Reader:** clearer artwork and icons for book browsing.
- **Lock Screen:** revised setup, a larger clock, clearer swipe-to-dismiss guidance, cover/photo display options, and improved wake presentation.

These extensions are separate downloads; update installed plugins to receive their latest changes.

## Smoother interaction and a tidier interface

- Smoother Now Playing/lyrics transitions, drawer expansion, and power-menu animation, with improved frame pacing and fewer unnecessary redraws. Performance still varies with the selected layout.
- More reliable short taps under load and fewer accidental selections while scrolling layout cards.
- Larger popup touch targets, consistent ordinary row heights, better text alignment, and red Cancel actions. Wrapped podcast and plugin rows keep the height their text needs.
- Reorganized settings under **Sound, Playback & Controls, Display, Power, Library, and System**. Drawer volume and battery percentage now live in Appearance.
- Music-header shortcuts are ordered **Sound, Playback, Library**. Album/song lists have an accent-colored Play All circle and a track count; All Songs also supports Play All in order or shuffled.
- **Upside Down Screen** in Display > Gestures & Orientation rotates the interface for using the player with its headphone jack at the top.
- Improved top-bar spacing, battery icon sizing, cover/lyrics alignment, and playback-button clipping across layouts.
- The long-press power menu slides up from the bottom and provides power off, restart, screen off, and sleep-timer actions.

## Sound, car mode, and connectivity

- **Parametric EQ redesigned:** clearer Frequency, Gain, and Q controls, easier-to-grab sliders, profile selection and saving, an all-values Flat reset, and a response graph after the ten bands. Controls follow the selected accent color.
- **Car Mode:** dedicated volume, a quick toggle, long-press access to its settings, optional low/high gain when the Gain plugin is installed, and optional auto-resume when booting with external power and headphones connected.
- **Bluetooth DAC:** LDAC is offered automatically with the rebuilt decoder. Its experimental Developer Options toggle has been removed.
- Wi-Fi and Bluetooth lists have clearer refresh icons and improved row layout. Connected Wi-Fi is shown first, duplicate entries are reduced, and refreshes preserve your scroll position.
- Better Bluetooth metadata handling, wake behavior, and USB-connected suspend handling.

## Library, streaming, and reliability

- A **Genres** browser, search in more library lists, remembered navigation positions, and quicker artist/album browsing.
- **Library sorting:** newest-modified Files order, Recently Added albums, and album release-year ordering.
- **Subsonic:** streaming queue and quality fixes, album downloads with cover art, a selectable download folder, and artist/album folder organization.
- Improved song and queue rows; fixed search results being obscured after dismissing the keyboard and corrected stale album-accent highlights.
- Broader track compatibility, including tagged AAC, OGA, AIFC, RF64/Wave64, and 32-bit PCM AIFF; safer handling of corrupted MP3 tags and oversized embedded lyrics, clearer playback errors, Unicode WAV title fixes, and more album-artist tag variants.
- Fewer false SD-card warning reports when inserting a library or restarting the player.
- Firmware updating uses GitHub's designated **latest release**, with stronger package/checksum validation and clearer preparation errors.
- R3II 2025 display scaling and volume-wheel direction/step handling have been revised.

**For existing libraries:** run **Settings > Library > Refresh All Metadata** to apply corrected WAV titles and album-artist tags to cached tracks. Run **Update Music Database** to populate release years for album sorting.
