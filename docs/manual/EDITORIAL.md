# Editorial and website handoff

[User manual](README.md)

This page is for the owner and Gemini's website work. Exclude it from the public chapter navigation.

## What is ready

The first English draft has 13 chapters plus a contents page. Text follows the current repository interface; 27 existing R1 illustrations have stable names under `images/`. Original screenshot files and the installed player are untouched. The manual has not been published, committed or pushed.

`website-manifest.json` defines chapter order and relative asset paths. Import the public chapters into the website's documentation renderer. Keep ordinary Markdown links, tables and image alt text. Source-verification HTML comments are maintainer notes and can be stripped from rendered pages.

Give portrait screenshots a responsive width, preserve their aspect ratio, and provide access to the original image. Avoid cropping controls to fit a card. The website may style the chapter navigation and figures independently; the content does not assume a particular framework.

The neighboring `compas-page` directory was identified as an existing website workspace. No files there were changed, and no message was sent to another developer. This handoff is portable content for its maintainer to integrate.

## Existing image provenance

`screenshot-manifest.json` lists the source path, dimensions, SHA-256 and replacement note for every bundled illustration. Most sources belong to the [2026-10-01 R1 walkthrough](../../Screenshots/2026-10-01/README.md); the README images have their original source path recorded separately. These are existing captures, not new verification of the current build. The collection's R3 theme/layout renders are simulator previews and are not used as device evidence in this edition.

Some important differences are intentional:

- Old Home images show **DAC**; current text describes **More**.
- Settings, playlist editing, album shuffle, lock-screen configuration and plugin layouts have changed since capture.
- The old lyrics illustration is not “Abril”. Its replacement **must use “Abril” by Malpaís**, with the recording, displayed artist/title and synchronized lyrics checked before capture.
- The EPUB image is a plugin example. No personal workout/text-reader screenshot was copied into this manual.
- Account-dependent pages, connection PINs and token/QR screenshots are excluded from the bundled examples.

Do not silently describe these images as the current release. After replacement, remove the legacy-image notice only when the illustrated workflows have been checked against the selected release.

## Later capture session

`screenshot-inventory.csv` inventories the existing setup, player, dialog and plugin gallery states. It is a starting checklist, not proof that every current dynamic screen has been discovered. `screen-inventory.csv` adds source-discovered literal screen titles and current pages that need a fresh screenshot. Plugin entries and feature-dependent menus must also be checked on the actual installed set.

For the later session:

1. Record the exact build/release, device model, resolution, theme, font size and plugin versions.
2. Capture setup, Home, navigation, drawer, Music views/search/edit states, player/lyrics/queue/information, settings and submenus, wireless/DAC/import/remote screens, books, plugin screens and confirmation/error/empty states.
3. Use demonstration accounts/data for account and network pages. Preserve readable controls and an unobstructed status bar where relevant.
4. Keep R1 device screenshots separate from simulator previews or unverified R3 hardware states.
5. Replace the stable `images/*.png` files, update their manifest hashes/provenance, then refresh `docs/images/readme/` and create a new dated GitHub gallery. Keep old dated capture archives as provenance rather than overwriting their history; retain current README/manual links where possible.

No new capture, ADB operation, layout switch, flash or restart was performed for this writing task. Future test files and capture logs must go directly to persistent SD or the host; leave the owner's installed test build in place.

## Editorial rules and verification

Assume readers are capable adults who may be unfamiliar with this player or its audio terminology. Explain the consequence of a setting, not the act of tapping it. Use technical names when they match the screen, and explain them once in context. Avoid developer vocabulary, vague advice, baby steps, and words such as “just”, “simply” or “obviously”. Independent options belong in choices or tables; numbered steps imply a sequence.

Write tasks around what the reader wants to do. Use exact English menu labels in bold, short numbered procedures and tables for comparable options. Keep native features distinct from optional plugins. Explain terms where first used. Avoid internal API names and performance-debug options in normal user flows.

Installation paths and model support follow the repository README; current C menu builders resolve stale screenshot labels. Companion-plugin descriptions were checked against local `compas-plugins` files. Hardware walkthroughs still needed before publication include stock first installation/recovery, R3 behavior, account-service login and all new screenshot states. No hardware validation is inferred from a compiled port. Factory Reset has not been exercised for this manual and clears internal user data, rather than only changing appearance preferences.

## Comparable guides consulted

The guide organization draws on the task-first onboarding, controls explanations, topic chapters and separate reference material in these official project sources. No text or images were copied from them, and their features are not attributed to Compás.

- [Rockbox manual: introduction and conventions](https://github.com/Rockbox/rockbox/blob/master/manual/getting_started/main.tex): first-use guidance and consistent control names.
- [Volumio user guide](https://volumio.atlassian.net/wiki/spaces/VPKB/pages/4620289/VOLUMIO+USER+GUIDE): setup guidance separated from feature and troubleshooting topics.
- [Volumio network playback chapter](https://github.com/volumio/docs/blob/master/docs/00_User_Manual/05_Stream_audio_to_volumio.md): explaining the sender/receiver relationship before connection steps.

Compás-specific facts are verified against this repository and its companion plugins, not those manuals. HiBy's [support center](https://store.hiby.com/apps/help-center) and [R1 guide](https://guide.hiby.com/en/docs/products/audio_player/hiby_r1/guide/) are manufacturer references for stock-device procedures, not validation of the Compás ports.
