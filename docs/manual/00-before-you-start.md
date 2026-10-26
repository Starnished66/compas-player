# Before you start

[User manual](README.md)

Compás replaces the music-player interface on supported HiBy devices. It plays music from a microSD card, browses tags and folders, and provides optional plugins for extra services and tools.

## Check your device

| Device | Guide coverage |
|---|---|
| **HiBy R1** | Supported and tested on the actual player. The device screenshots in this manual show the R1. |
| **HiBy R3 Pro II** | A version for this model is available, but it has not yet been tested on the actual player. R1 instructions and screenshots may differ. |
| **HiBy R3 II 2025** | A version for this model is available, but it has not yet been tested on the actual player. Its screen and physical controls differ from the R1. |

Use the package for your **exact model**. An R1 player update or firmware image cannot be used for an R3. This guide does not cover other models, including similarly named HiBy devices, unless the project adds them to its support list.

## What you need

- Your player, with enough charge to finish installation.
- A microSD card and a computer with a card reader or working USB file transfer.
- A USB cable that supports data transfer.
- A copy of your music and any playlists you want to keep.
- For a first install, the matching full Compás firmware image and a known-good recovery image for your model.

Back up the card and any settings you need before changing firmware. Keep the recovery image on your computer so it is available if the player cannot reach its normal menus.

## Choose a download

For the guided first installation, use [Compás Tools over USB](01-install-and-update.md#first-installation). It downloads the firmware for the selected model and handles the installation steps. The downloads below are for preparing a card yourself or choosing the right kind of update.

Use the project's [GitHub releases](https://github.com/Starnished66/compas-player/releases/latest). Read the release notes alongside the downloads.

| Download | When to use it |
|---|---|
| Full `.upt` firmware image | Your first Compás installation, or an update that includes fonts, graphics or other system files. |
| Player update file for your model | A routine player update, once Compás has been installed. |
| Plugin, theme or layout | An optional addition, usually installed through its in-player catalog. |

The full firmware package includes the bootloader, the small startup program that lets Compás install later player updates. A player-only download updates the music player application, not the bootloader or other system files. For a first installation, choose the full firmware image. See [Install and update](01-install-and-update.md) before copying an update file.

## Find your current version

Open **Settings > System > About** and read the **Compás Player** version. Include it when asking for help, along with your device model and whether you use a published release or a development build.

Continue with [Install and update](01-install-and-update.md), or [First steps](02-first-steps.md) if Compás is already running.

<!-- Verified against README.md device support/install sections and gui_settings.c build_about_screen. -->
