# Install and update

[User manual](README.md)

A full firmware image installs Compás and its bootloader, the small startup program used to install later player updates. After the first full installation, most updates can use the smaller player-only download, which updates the player application. Use the full image when the release notes call for changes to fonts, graphics or other system files.

## First installation

1. Confirm your exact model and download its full `.upt` package from [Compás releases](https://github.com/Starnished66/compas-player/releases/latest).
2. Keep a known-good recovery image for that same model on your computer. Back up your card.
3. Copy the Compás image to the top-level folder of the microSD card (the card root), using the filename for your model: `r1.upt`, `r3proii.upt`, or `r3ii_2025.upt`. Keep only the intended update image in that folder for this installation.
4. Eject the card safely and use your model's recovery updater to install it. For an R1, the recovery entry used by Compás is **Power + Volume Up held during startup**. If you are starting from stock firmware, follow its matching recovery instructions. A player-only SD update is available only after the Compás bootloader has been installed with the full image.
5. Follow the updater's prompts and keep the player powered while it writes the firmware. Let it finish and restart.
6. Complete the setup screens, then check **Settings > System > About**.

The R3 versions have not been tested on the actual players. Confirm the recovery procedure for your exact model before attempting a first install. The R3 II 2025 recovery shortcut is **Power + Play/Pause**.

## Update the player from the SD card

1. Download the player file for your exact model.
2. On the card, open or create the hidden `.compas` folder at the card's top level. If your computer hides folders whose names start with a dot, enable showing hidden files to find it.
3. Put the downloaded player file in that folder and name it exactly `compas_player` (no file extension). Its location should be:

   ```text
   SD/.compas/compas_player
   ```

4. Safely disconnect file transfer and open **Settings > Updates > Player Update > Install from SD card**.
5. Let Compás install the file and restart. Check the version under **Settings > System > About**.

The bootloader can also install a player update from this folder automatically at startup. Keep the other contents of `.compas` when adding the file. Compás checks that the file matches your model and may reject a version older than the one included with the firmware. If rejected during startup, the file is kept with `.rejected` added to its name.

A player-only update changes the player application. If the release requires new fonts, icons or other system files, use the full firmware package specified in its release notes.

## Update over Wi-Fi

1. Connect to Wi-Fi with internet access.
2. Open **Settings > Updates > Player Update**.
3. Choose **Check for online update** and follow the download and installation prompts.

For the full firmware, use **Settings > Updates > Firmware Update**. Online firmware installation checks the latest published GitHub release, including when installing a previously downloaded online image. It needs internet access for that check.

## Install a full firmware image offline

1. Copy the matching `.upt` image to the top-level folder of the microSD card (the card root) with the model-specific filename above.
2. Open **Settings > Updates > Firmware Update > Install from SD card**.
3. Confirm the selected image and wait while Compás prepares recovery. The device then reboots into the updater.

Before restarting into the updater, Compás moves other `.upt` files from the card root into another folder. Recovery still checks the image contents; renaming an image for a different model does not make it compatible. Preparation can take a while. Keep power connected and let the updater finish.

## Recover a player that cannot reach Home

Use the known-good full firmware image for your exact model and its recovery updater. On an R1 with Compás, place the intended image in the card's top-level folder and hold **Power + Volume Up** during startup. Reinstalling a full image restores the firmware; if the normal menus still work, [collect the version and update log](11-troubleshooting.md) first.

For stock firmware downloads and manufacturer procedures, use [HiBy's support center](https://store.hiby.com/apps/help-center). Compás's release notes and model compatibility notes take precedence for its own packages.

<!-- Verified against README.md, src/core/firmware_update.h/c, src/core/player_install.c and gui_settings.c update menu. Stock and R3 hardware procedures are not newly device-tested for this edition. -->
