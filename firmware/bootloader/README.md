# Compás bootloaders

`make bootloader BOARD=r1` and `make bootloader BOARD=r3proii` build the full
combined Compás/Rockbox Linux-hosted bootloader and place it at the existing
firmware packaging path: `compas_loader` or `compas_loader_r3proii`. The
selector is derived from Rockbox’s `hibyos_linux.c` and retains its
GPL-2.0-or-later notice. The build is pinned to Rockbox commit
`2b12d568ff32089a9efcbc2db95e9a18c7d9c68f`; its source and intermediate files
are kept under `build/rockbox-bootloader/`. `make rockbox-bootloader` remains
available as an alias that builds the same combined image in that build
directory. `ROCKBOX_SOURCE` may point to a Git checkout containing the pinned
commit; the script clones it into the build directory and does not modify the
provided checkout.

R3II 2025 has no verified Rockbox target, so `make bootloader
BOARD=r3ii_2025` continues to build the standalone Compás loader directly to
`compas_loader_r3ii_2025`.

The combined selector draws the Compás splash and prepares the SD card before
checking for Rockbox. If a valid matching installation is missing and no
selector request is pending, it skips the menu and enters the shared Compás
loader directly. Otherwise the native Compás menu opens before Rockbox
initialization. Compás is always the initial selection. With a valid Rockbox
installation and no forced selector request, Compás starts after a one-second
countdown; volume input, touch input, or opening Tools cancels that countdown
for the rest of the selection session. A forced selector request opens the
menu without a countdown. Selecting Compás re-enters loader mode in the same
binary, before Rockbox initialization. The native Tools menu provides ADB
toggle, SD script, SD remount, filesystem repair, restart, shutdown and
recovery actions. Loader mode installs the canonical SD update, selects the
installed or packaged player, and supervises it with the normal crash, reboot,
shutdown and logging behavior. It does not invoke a separate
`compas_loader` executable. The player also retains its Settings > Updates >
Player Update action.

Rockbox payloads use the standard SD `/.rockbox/rockbox.r1` or
`/.rockbox/rockbox.r3proii` location and matching `rockbox-info.txt`. R1
payloads can use the firmware’s system `libasound.so.2`; no SD-local copy is
required. Executables are
validated and staged atomically under persistent `/usr/data/.compas/rockbox/`,
rather than RAM-backed storage. A missing, invalid or unstaged payload falls
back to Compás. The source on the SD card is retained. A successful exec
replaces the selector; it does not remain as a supervisor.

Compás reboot requests ask BusyBox init to run its shutdown hooks, with a
bounded kernel reboot fallback. The shared supervisor remains active for both
the standalone loader and the combined selector.

Builds require the MIPS musl cross toolchain, a host C compiler, Perl, Git,
Python 3, GNU make and `flock`. CI installs the toolchain and provides the
host dependencies in its Ubuntu runner image.

Before deploying on a board, verify both boot choices, SD player updates,
crash recovery, restart, shutdown and card-removal handling on that board.
