# What's New: Next Release

- **Menu customization:** Appearance now includes a Music menu editor with the same drag handles and visibility switches as Quick Drawer. Choices persist independently of Kids Mode and other plugin overrides.

- **Hardware button lock:** the Lock Screen plugin can enable a Power + Volume Up chord that locks the physical buttons. The lock is native, so the same chord still unlocks if the plugin is stuck. Short Power still sleeps and wakes; a long Power press still opens the power menu. The option is independent of the visual lock screen.

Development notes accumulated for v1.2.0. See WHATS_NEW.md for the release announcement.

- **All Songs:** a Play All row at the top plays the whole library, in order or shuffled.
- **Sorting:** choose independent album orders for Albums, Artists, and Album Artist collections.
- **Upside Down Screen:** Settings > Display turns the screen around, for using the player with the headphone jack on top.
- **Search:** Favorites, Most Played, playlists, Recently Added and album song lists can now be searched.
- **Subsonic:** downloaded albums now keep their cover art in the music library.
- **Genres:** a new Genres section in Music lists your genres; tap one to play or search its songs.
- **Settings:** add an optional, separate startup volume that applies only when the player powers on via Car Mode.
- **Player layouts:** Settings > Display > Player Layout > Layout switches the Player screen between the standard look and layouts loaded from XML files (LVGL XML format, see docs/reference/PLAYER_LAYOUTS.md). Plugins can supply one with `plugin.set_player_layout{xml = ...}`. The standard layout is unchanged.
- **Player layouts:** layouts can place the lyrics in their own area and color them (`lyrics_area`, `lyrics_active`, `lyrics_toggle`), and load their own images from a folder next to the XML file. A new example plugin, HiByStockPlayer.lua, adds a layout in the style of the stock HiBy OS player.
- **Plugins:** plugins can now read back their own stored secrets (such as login tokens) with `plugin.secrets.get()`.
- **Language:** Settings > System > Language switches the interface language: English, Spanish, Portuguese (Brazil), Italian and French. The translations are machine drafts awaiting native review.
- **Keyboard:** Settings > Display > Appearance > Keyboard switches text entry and search between the T9 keypad and a full QWERTY keyboard with numbers, symbols and accented letters. Number fields keep the numeric keypad.

- **Bluetooth DAC:** LDAC is now enabled automatically with the rebuilt decoder; the experimental Developer Options toggle has been removed.

- **Button mapping:** assign single, double and long presses in Playback & Controls > Buttons & Remote. Reset All restores the native controls; the R3 2025 wheel supports single actions. Existing Play/Pause double-press configurations keep their 700 ms window; newly assigned doubles use 300 ms.
- **Volume Lock:** optionally prevent volume changes with the screen off, while keeping other remapped actions available.
- **Firmware updates:** offline SD updates now hand the model-named image to stock recovery for package validation; OTA downloads still validate the package before publishing it.

- **Player updates:** Settings > Updates > Player Update installs just the player from the SD card or the latest GitHub release, without flashing the firmware. The renamed `compas_loader` also retains automatic SD player updates at startup.
- **Updates:** the Firmware Update and Player Update menus now describe what each one replaces: firmware flashes the bootloader, player, kernel and drivers, while a player update keeps the current drivers, kernel and bootloader.

- **Boot menu:** R1 and R3 Pro II use the combined Compás/Rockbox loader by default. When a compatible Rockbox installation is present, choose between Compás and Rockbox or open recovery tools. R3II 2025 retains the standalone Compás loader.

- **Startup image:** choose a custom Compás splash through the Startup Image plugin, or tint the default splash with your accent color from Appearance > Accent Color. Images are fitted to each device’s screen and apply on the next boot.

- **Kids Mode plugin:** choose which Home entries, Music menus, Music shortcuts and Quick Drawer controls appear. Reorder items with the native hold-and-drag handles and visibility switches. Parent Mode remains available with PIN access even when all other Home entries are hidden. These controls simplify navigation; they do not lock the device or turn off hidden features.
- **Plugin creation:** plugins can open the native drag-and-toggle item editor through `plugin.show_order_editor()` when the player advertises `ui.order_editor`.
- **Emulators:** More > Emulators downloads emulators and plays their games from the Games folder on the SD card, starting with NES.
- **Touch:** the R1 accepts two simultaneous fingers and the R3 Pro II accepts five, so emulator controls can be pressed together. The R3 II 2025 is unchanged. The R3 Pro II driver source is at https://github.com/Starnished66/hiby-custom-kernel/tree/4c7d47ede0bfa4b63528574817becc79d327bf9d/boards/r3proii/modules/gt9xx_touch (GPL-2.0).

- **Dutch:** Books folder instructions now retain the actual folder name `Books` while the menu remains translated as Boeken.
- **MSEB:** directly under Settings > Sound, with all ten stock controls on one scrolling page; requires the new release.
