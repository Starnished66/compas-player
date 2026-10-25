# Settings reference

[User manual](README.md)

Use this page to find a setting by name. Installed plugins can add extra options.

## Settings map

| Category | Page or setting | What it does |
|---|---|---|
| **Sound** | Equalizer | Opens equalizer controls and its profile picker. |
| **Sound** | Crossfeed | Mixes a filtered part of each stereo channel into the other. |
| **Sound** | Plugin audio options | Installed audio plugins can add settings here; effect and downloadable profile groups appear when a plugin adds them. |
| **Sound** | Startup Volume | Sets whether playback starts at a fixed volume and the selected level. |
| **Sound** | Max Volume | Sets the maximum volume allowed from the player, buttons, remote controls, and plugins. |
| **Sound** | ReplayGain | Adjusts playback loudness using information saved in your music files. |
| **Sound** | Local DAC Settings | Appears on models whose audio hardware supports these controls. |
| **Playback & Controls** | Resume Last Track | Chooses how playback resumes. |
| **Playback & Controls** | Crossfade | Fades between tracks when supported by the active playback path. |
| **Playback & Controls** | Gapless | Plays supported tracks without an intentional gap. |
| **Playback & Controls** | Car Mode | Sets Car Mode, its volume, auto-resume on power-on, and Gain when that plugin option is available. |
| **Playback & Controls** | Buttons & Remote | Opens button and remote settings (see below). |
| **Playback & Controls > Buttons & Remote** | Button Mapping | Select an available physical button, then assign its tap, double-tap, or hold action where supported. **Reset All** restores default mappings. The button list depends on the detected player model. |
| **Playback & Controls > Buttons & Remote** | Volume Lock (Screen Off) | Locks volume changes while the screen is off. |
| **Playback & Controls > Buttons & Remote** | In-line Remote | Enables or disables support for an in-line remote control. |
| **Playback & Controls > Buttons & Remote** | Plugin rows | Some plugins add remote or control options here. |
| **Display** | Screen Timeout | Enable automatic screen-off and choose its inactivity delay. |
| **Display** | Screen Dimming | Enable dimming before screen-off and choose the dimming delay. |
| **Display > Appearance** | Accent Color | Changes the interface highlight color. |
| **Display > Appearance** | Font | Selects a font from the SD card's `Fonts` folder for Latin-alphabet text, or restores the built-in default. |
| **Display > Appearance** | Font Size | Changes app text size. |
| **Display > Appearance** | Keyboard | Selects the on-screen keyboard layout. |
| **Display > Appearance** | Quick Drawer | Controls drawer visibility, item visibility, and item order. |
| **Display > Appearance** | Music menu | Controls visible Music menu entries and their order. |
| **Display > Appearance** | Top Bar | Sets visible status items, battery percentage, and clock position. |
| **Display > Appearance** | Artist Images | Shows or hides artist images in supported library views. |
| **Display > Appearance** | Themes | Chooses an installed theme. Theme files in `SD/Themes/` and its subfolders are available to the picker. |
| **Display > Player Layout** | Layout | Chooses the Player screen layout; **Download** opens the player-layout catalog. |
| **Display > Player Layout** | Lyrics | Enables or disables lyrics display. |
| **Display > Player Layout** | Lyrics Text Size | Changes lyrics text size. |
| **Display > Player Layout** | Hide Player/Lyrics Top Bar | Hides the status bar on the Player and Lyrics screens. |
| **Display > Gestures & Orientation** | Swipe Up for Home | Enables or disables the swipe gesture to return Home. |
| **Display > Gestures & Orientation** | Animation Speed | Changes interface transition speed. |
| **Display > Gestures & Orientation** | Upside Down Screen | Rotates the interface orientation. |
| **Power** | Sleep Timer | Arms a playback countdown, sets its duration, and shows remaining time while active. The Quick Drawer also has a timer control. |
| **Power** | Idle Shutdown | Enables an idle timeout and selects **Power Off** or **Suspend to RAM**. |
| **Power** | Charging | Opens charge limit, safe charging, LED, and (on some models) USB DAC charging preferences. |
| **Power** | Plugin rows | Timer plugins or other installed plugins may add settings here. |
| **Power > Charging** | Charge Limit (85%) | Enables the charge limit at the indicated level. |
| **Power > Charging** | Safe Charging (500mA) | Enables reduced-current charging. |
| **Power > Charging** | LED charge indicator | Controls the charge indicator light. |
| **Power > Charging** | Disable charging in USB DAC mode | Available on some models; controls charging while USB DAC mode is active. |
| **Library** | Update Music Database | Scans for added, removed, or changed music files. |
| **Library** | Refresh All Metadata | Re-reads music tags across the library. |
| **Library** | Refresh All Covers | Refreshes cover artwork across the library. |
| **Library > Sorting** | Files | Sorts folders and files by name or newest modification date. Folders remain first. |
| **Library > Sorting** | Albums, Artists, Album Artist | Sets list order by name, recently added, or release year when that information is saved in your music. |
| **Library** | Plugin rows | Library plugins may add their own settings. |
| **System** | USB Mode | Chooses the USB connection mode. |
| **System > Clock** | Automatic | Uses automatic clock setting. Turn this off to set the time manually. |
| **System > Clock** | Set Time | Sets the device time when automatic setting is off. |
| **System > Clock** | 24-Hour Clock | Changes the clock display format. |
| **System > Clock** | Time Zone | Selects the device time zone. |
| **System** | Language | Selects the interface language. |
| **System** | Plugin Manager | Manages installed plugins. |
| **System > Maintenance** | Hostname | Changes the device hostname. |
| **System > Maintenance** | Factory Reset | Clears internal user data and returns to setup after confirmation. |
| **System** | About | Shows player information and help links. |
| **System > About** | Developer Options | Opens ADB, debug logging, screenshot shortcut, and reboot-to-bootloader controls. |
| **System** | Additional Tools | Appears when installed plugins register system tools. |
| **Updates** | Firmware Update | Checks for or installs firmware updates. |
| **Updates** | Player Update | Checks for or installs a player update. |
| **Updates** | Plugins & Layouts | Opens plugin and player-layout update options. |

## Storage locations

- Music files are browsed from the SD card's music storage. The music database indexes supported audio files and their tags.
- Common audio formats recognized by the file browser include MP3, FLAC, WAV, AIFF, AAC, M4A, M4B, Ogg, Opus, DSF, DFF, WV, CAF, APE, and WMA. Playback support can depend on the player build and audio path.
- Saved playlists use the SD card's `Playlists/` folder. The playlist editor creates and edits playlists separately from the music files; copied playlist files can also be listed there. The current Queue holds the playback order.
- **Books** on Home reads `.txt` files from `Books/` on the SD card.
- The Audiobooks plugin stores books under `Audiobooks/`; the Podcasts plugin stores its library under `Podcasts/` and uses OPML subscription files. These folders and features require their respective plugins.
- Screenshots are saved as PNG files in `Screenshots/` on the SD card. Enable the screenshot shortcut in **Settings > System > About > Developer Options**; the button combination label reflects the player model.
- Theme files use `SD/Themes/`. Custom fonts use `SD/Fonts/`.
- Manually installed plugins go in `SD/.plugins/`; companion files go in the destinations listed by that plugin's package. Plugin Store installation can manage these files for you.
- Player layouts can be installed through the layout catalog. Layout packages and plugin-provided layouts are managed separately from music playlists.
- Saved playlists are separate from the audio files. The queue holds the current playback order; **Queue** and saved playlists are found in the Music library's playlist area. **Favorites**, **Most Played**, and **Recently Added** are library collections rather than ordinary saved playlists.

**More** also contains **File Manager** for browsing and managing SD card files and **Emulators** for installed emulator content. To see the built-in books reader, open **Books** from Home. Audiobooks and Podcasts are plugin features and appear when their plugins are installed.

For installation details, see the project README and the relevant plugin or player-layout instructions. Extra rows contributed by plugins may appear in Sound, Playback & Controls, Display, Power, Library, System, or Updates. For example, the Lock Screen companion plugin adds **Lock Screen** under **Settings > Display**.

## Files and games

**More > File Manager** works directly on SD-card files. Select entries and use its menu for **Copy**, **Move**, **Rename**, **Delete**, or **New folder**. After copying or moving, open the destination and choose **Paste … here**. Deleting here removes files from storage; deleting an entry inside a music playlist removes a playlist entry. Refresh the music database after reorganizing your music folders.

To use **More > Emulators**:

1. Connect to Wi-Fi and open **Download emulators** to choose a compatible package.
2. Open the installed emulator and select its system.
3. Put additional game files in the **Games/<system>** folder named by that system's empty-state screen, then return to its game list.
4. Choose a game and follow the emulator's controls. Controls and supported formats depend on the package and device.

Emulator packages run on the player, rather than in the desktop player simulator. Updating or removing a package can replace its bundled games and saves; review the confirmation. Games you keep under **SD/Games** are kept by that package-removal operation. Hardware support still follows the device notes at the beginning of this manual.

<!-- Sources: src/ui/gui_settings.c (build_settings_screen; build_music_audio_screen; build_music_playback_screen; build_music_controls_screen; build_settings_display_screen; build_settings_appearance_screen; build_settings_player_layout_screen; build_settings_gestures_screen; build_settings_power_screen; build_settings_charging_screen; build_library_category_screen; build_sorting_screen; build_settings_system_screen; build_clock_screen; build_about_screen; build_dev_options_screen; build_settings_updates_screen); src/ui/gui_library.c (playlist path, collections and queue UI); src/library/file_browser.c (audio extensions); src/ui/gui_books.c (Books folder and .txt); src/core/screenshot.c (PNG screenshot folder); ../compas-plugins/plugins/Audiobooks/Audiobooks.lua; ../compas-plugins/plugins/Podcasts/Podcasts.lua; ../compas-plugins/plugins/LockScreen/LockScreen.lua; src/ui/gui_settings.c (More native entries); README.md (plugin installation, Themes folder, update paths); src/ui/gui_shell.c (Quick Drawer); docs/PLAYER_LAYOUTS.md. -->
