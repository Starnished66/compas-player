# Troubleshooting

[User manual](README.md)

Start with the section that matches the problem. Note your device model and **Settings > System > About** version before changing settings.

## Music is missing, duplicated, or named incorrectly

1. Open **Music > Files** and check that the files are visible on the card.
2. If files are present but library views are out of date, run **Settings > Library > Update Music Database** and let it finish.
3. If the titles or artists are wrong, check the files' tags on a computer. After correcting them, use **Settings > Library > Refresh All Metadata**.
4. For changed album artwork, use **Refresh All Covers** on the same page.

Compás reuses artwork for album browsing and playback. Keep an album's artwork consistent when preparing your music; different covers for individual songs within the same album are not supported. If the information or artwork is missing from the files, add it on your computer or use a suitable fetching plugin before refreshing.

## Lyrics do not appear

1. Turn on **Settings > Display > Player Layout > Lyrics**.
2. In Now Playing, tap the cover to open the lyrics view.
3. For lyrics in a separate file, use a matching name, such as `Song.flac` and `Song.lrc`, in the same folder. The `.lrc` file needs timing information for the words to be highlighted as the song plays.
4. If you use a lyrics-fetching plugin, check its network connection and whether it found lyrics for the correct recording.

The lyrics viewer and an online lyrics fetcher are separate features. See [Playback](04-playback.md) and [Plugins](07-plugins.md).

## There is no sound

1. Check the volume and that playback is running.
2. Check the intended output: wired headphones, a connected Bluetooth device, or a DAC mode.
3. If Bluetooth is connected unintentionally, disconnect it and retry the wired output.
4. Leave USB DAC or Bluetooth DAC mode when you want the player to play its own library.
5. Test a known-good local track. If it works, check the failing file or streaming source separately.

For a quiet or unusually loud startup, review **Settings > Sound > Startup Volume**, **Max Volume**, and **ReplayGain**, plus any gain/effects plugins you enabled.

## The computer cannot see the SD card

1. Use a data-capable USB cable and try another USB port.
2. Select **Settings > System > USB Mode > Storage**.
3. If **ADB** is enabled under **Settings > System > About > Developer Options**, turn it off so storage can use the port.
4. Reconnect and wait for the computer to detect the card. A card reader is another way to copy files.

Eject storage safely before removing the cable or card.

## Wi-Fi works but downloads fail

Check **Wireless > Wi-Fi > Wi-Fi Info** for the player's connection. A phone having internet access does not confirm the player's network connection. Try the service again later and check whether the player can reach other online services. If it cannot, check the network connection and DNS settings; DNS is what lets the player find a service by its address. A guest network may also block access to a music server on your home network.

Plugin catalogs and online update checks need internet access. Local Subsonic servers, remote control, AirPlay and DLNA can instead use a reachable local network. If a network source fails while local music plays normally, collect the displayed error and which service was in use.

## A plugin, theme, or layout is missing

- For a plugin, check **Settings > System > Plugin Manager**, enable its switch, then use **Refresh Plugins** after manual file copying.
- Install all files that come with the plugin in the package, including icons and layouts. See [Plugins](07-plugins.md).
- A compatibility message such as **Needs newer firmware** means the plugin needs a newer player. Update the player or full firmware as instructed by its release notes.
- Use the relevant theme or layout picker for packages of those types. Themes and layouts can appear in their own pickers rather than as switches in Plugin Manager.

If a plugin causes a problem, disable it and test again. Record its name and version when reporting the issue.

## An update fails or the player will not start

Read the error before retrying. Confirm the model and update type, the filename and destination, available card space, and any required full-firmware update. Firmware preparation writes its log to `SD/.compas/ota/update.log`.

If a firmware installation is still writing or preparing recovery, keep the player powered and allow it to finish. If the player cannot reach its menus, use the model-specific [recovery procedure](01-install-and-update.md#recover-a-player-that-cannot-reach-home) with a known-good image.

**Factory Reset** is under **Settings > System > Maintenance**. It clears internal user data and returns to setup. Back up what you need and read the confirmation before using it; ordinary tag, artwork and connection problems should be checked with the targeted steps above first.

## Take a screenshot and report a problem

1. Open **Settings > System > About > Developer Options** and enable the **Screenshots** option. Its label shows your model's button combination.
2. On an R1, press **Power + Volume Down** together to capture the current screen. The implemented R3 II 2025 shortcut is **Power + Previous**; that model has not been tested on the actual hardware.
3. Find the saved image in `SD/Screenshots/`.
4. Open a [GitHub issue](https://github.com/Starnished66/compas-player/issues) with the device, player version, steps to reproduce, expected result and actual result. Add the screenshot and the exact error text.

For audio problems, include the format and sample rate, output connection, whether a plugin is involved, and whether it happens with a known-good local file. Share logs if a maintainer requests them. Remove account credentials, connection PINs and other private details from attachments.

<!-- Verified against gui_settings.c developer/maintenance/library paths, screenshot.c, firmware_update.c, gui_network.c and current player/plugin workflows. No recovery/reset performed to write this guide. -->
