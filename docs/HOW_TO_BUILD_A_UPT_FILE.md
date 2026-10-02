# How to build a `.upt` firmware image

This guide explains how to build a complete firmware update package for the
HiBy R1, R3 Pro II, or R3II 2025 from an approved base image and matching
Compás player and bootloader binaries. Each output is for one exact board.

The repository does not contain redistributable stock HiBy firmware. Obtain
the approved base image separately and keep it outside the repository. The
GitHub workflows use the private `staging-image-base` release and verify each
base against a repository secret before packaging it. The secret names are
`STAGING_IMAGE_SHA256` (R1), `R3PROII_STAGING_IMAGE_SHA256` (R3 Pro II), and
`R3II_2025_STAGING_IMAGE_SHA256` (R3II 2025). The secret values are never
needed in the source tree or in these instructions.

## What the script produces

[`scripts/repack_upt.sh`](../scripts/repack_upt.sh) creates an ISO 9660 `.upt`
file in the format expected by the device recovery updater. It extracts the
base, checks its board identity and player/bootloader handoff, installs the
matching binaries, copies tracked project assets and the firmware overlay,
recompresses the root filesystem, builds the OTA chunks and metadata, and
checks the final package against the 45 MiB limit (47,185,920 bytes).

The script works in a temporary directory. It does not modify the base image
or source tree, and the output path must differ from the base image path.

## Requirements

Run the commands from the repository root on Linux. The repack step requires:

```text
7z, unsquashfs, mksquashfs, genisoimage, file, md5sum, sha256sum, split
```

On Debian or Ubuntu, install the non-core tools with:

```sh
sudo apt-get install genisoimage p7zip-full squashfs-tools file
```

`md5sum`, `sha256sum`, and `split` come from GNU coreutils. Building the
binaries also needs the project build dependencies and MIPS musl cross-toolchain;
see the build section in [`README.md`](../README.md).

## 1. Obtain the matching approved base image

Use a complete, approved staging image for the same model as the binaries:

| Board | Base image example | Firmware board marker | Panel |
| --- | --- | --- | --- |
| HiBy R1 | `/path/to/r1.upt` | R1 | 480×800 |
| HiBy R3 Pro II | `/path/to/r3proii.upt` | `R3PROII` | 480×720 |
| HiBy R3II 2025 | `/path/to/r3ii_2025.upt` | `R3II_2025` | 320×480 |

The R3 bases must also contain the supported BlueALSA 5 / BlueZ runtime and
Speex rate-conversion files required by the player. The packer checks these
requirements and stops if they are missing. Do not substitute an arbitrary
stock image or an older public beta: the packer checks the firmware identity,
required runtime files, font checksums, and bootloader handoff. Keep the base
unchanged as a recovery image.

The approved base images contain proprietary HiBy files. They remain external
inputs and must not be committed, uploaded as public source artifacts, or
included in this repository. GitHub Actions obtains its copies from the
private `staging-image-base` release and validates them with the corresponding
checksum secret.

## 2. Build matching player and bootloader binaries

Build both outputs for the same board. The Makefile writes board-specific
outputs so builds do not overwrite each other:

| Board | Build commands | Player output | Bootloader output |
| --- | --- | --- | --- |
| R1 | `make target BOARD=r1`<br>`make bootloader BOARD=r1` | `compas_player_target` | `compas_bootloader` |
| R3 Pro II | `make target BOARD=r3proii`<br>`make bootloader BOARD=r3proii` | `compas_player_target_r3proii` | `compas_bootloader_r3proii` |
| R3II 2025 | `make target BOARD=r3ii_2025`<br>`make bootloader BOARD=r3ii_2025` | `compas_player_target_r3ii_2025` | `compas_bootloader_r3ii_2025` |

Do not mix binaries between boards or use a host executable. The image installs
them as `/usr/bin/compas_player` and `/usr/bin/compas_bootloader`. It also
removes the old `/usr/bin/open_hiby_bootloader` and updates the launcher to
start the new bootloader.

## 3. Check the repository inputs

The repacker adds project-maintained files after unpacking the base. It copies
only files tracked by Git from `assets/`:

| Repository path | Device destination | Application order |
| --- | --- | --- |
| `assets/theme1/` | `/usr/resource/litegui/theme1/` | Shared theme assets |
| `assets/theme2/` | `/usr/resource/litegui/theme2/` | Shared theme assets |
| `assets/r1/`, `assets/r3proii/`, or `assets/r3ii_2025/` | matching device paths | Board-specific files are applied after shared files and take priority |
| `assets/fonts/` | `/usr/resource/fonts/` | Tracked project fonts are copied after the board assets |
| `firmware/overlay/` | `/` (root-relative) | All files and symlinks are copied after the assets |

The `assets/` trees mirror paths below their destination directory. The
`firmware/overlay/` tree is already root-filesystem-relative and holds files
that are not UI assets, such as the NTP helper, filesystem tools, and the
Speex runtime files.

The Korean font is intentionally not tracked because its source image does not
provide a license. The approved base supplies `/usr/resource/fonts/Korean.ttf`;
the packer checks its checksum along with the other required firmware fonts.
Likewise, ignored files copied from a local stock firmware dump are not
packaged. Stock assets not replaced by tracked project assets come from the
approved base image.

To inspect the exact tracked inputs before packaging:

```sh
git ls-files assets/theme1 assets/theme2 assets/r1 assets/r3proii assets/r3ii_2025 assets/fonts
find firmware/overlay \( -type f -o -type l \) -print | sort
```

An asset present locally but untracked will not ship. Track all intended
project assets before creating a release image. Do not put generated binaries,
extracted firmware trees, local overlays, private base images, or checksum
secret values in a commit.

## 4. Create the `.upt` file

Choose the command matching the base and binary set. The output directory is
created automatically.

```sh
# R1
scripts/repack_upt.sh --board r1 \
  /path/to/r1.upt compas_player_target compas_bootloader output/r1-custom.upt

# R3 Pro II
scripts/repack_upt.sh --board r3proii \
  /path/to/r3proii.upt compas_player_target_r3proii \
  compas_bootloader_r3proii output/r3proii-custom.upt

# R3II 2025
scripts/repack_upt.sh --board r3ii_2025 \
  /path/to/r3ii_2025.upt compas_player_target_r3ii_2025 \
  compas_bootloader_r3ii_2025 output/r3ii_2025-custom.upt
```

On success, the script prints the output size and SHA-256 checksum. A
successful run also means the base had the required filesystem and kernel
chunks, passed the board/runtime/font/handoff checks, and the final image
passed the size limit. If the size check fails, do not flash the image; review
the filesystem and overlay contents before rebuilding.

## 5. Inspect the generated package

For example, inspect the R1 output (substitute the matching path for another
board):

```sh
file output/r1-custom.upt
7z t output/r1-custom.upt
7z l output/r1-custom.upt | sed -n '1,40p'
```

`file` should identify an ISO 9660 filesystem and `7z t` should report no
errors. The archive should contain `ota_config.in`, `ota_v0/ota_update.in`,
`ota_v0/ota_v0.ok`, the `xImage` and `rootfs.squashfs` chunks, and the
corresponding `ota_md5_*` files.

For a closer inspection:

```sh
inspect_dir=$(mktemp -d)
7z x -y output/r1-custom.upt -o"$inspect_dir" >/dev/null
cat "$inspect_dir/ota_v0/ota_update.in"
cat "$inspect_dir/ota_config.in"
rm -rf "$inspect_dir"
```

`ota_update.in` identifies the kernel and root filesystem images, including
their byte sizes and initial MD5. `ota_config.in` should contain
`current_version=0`.

## Optional: creating an upgraded runtime base

The normal `.upt` workflow starts from an approved staging image and adds the
tracked project assets, firmware overlay, player, and bootloader. The separate
`scripts/build_base_*.sh` and `scripts/prepare_*overlay.sh` recipes build
optional replacement runtime components from source for preparing such a
staging image. They are not needed to compile the player or repack an already
approved base. Their stock firmware trees and build stages are local inputs;
they are not the three-board GitHub packaging workflow and must not be
committed.

## Important limitations

A successfully created image has not necessarily been tested on hardware.
Install only an image for the exact model and retain a known-good recovery
image. R3 Pro II and R3II 2025 builds still require target hardware validation.
For a player-only update, use the standalone binary for the matching board;
changes to assets, fonts, firmware overlay, or the bootloader require a full
`.upt` rebuild.
