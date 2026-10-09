# R1 base-image upgrade

This is a staged **userspace** upgrade, not a replacement distribution. The
player uses a static musl build; the extracted firmware uses
glibc 2.22. Build its shared libraries with the Ingenic glibc toolchain,
not `mipsel-linux-musl-gcc`.

## Component set

| Component | Candidate version / scope |
| --- | --- |
| BlueALSA | 5.0.0: daemon, control tools, ALSA plugins |
| ALSA library | 1.2.16; retain device ALSA configuration |
| SBC | 2.2 |
| AAC | fdk-aac 2.0.3 |
| aptX | rebuild libopenaptx 0.2.0; retain LGPL version |
| BlueZ | 5.87 public `libbluetooth` only; retain vendor 5.54 daemon/tools |
| D-Bus | 1.16.2 library, daemon, tools and activation helper together |
| GLib | 2.84.4, with libffi 3.4.8 and PCRE2 10.46 |
| zlib | 1.3.2 |
| Expat | 2.8.4 |

Retain the matched vendor LDAC encoder/ABR pair. The LDAC decoder is rebuilt
from source: the vendor libldacdec.so.1 segfaults (decodeSpectrumFine,
decodeScaleFactors) about a second into real LDAC reception in DAC mode.
`libldacdec-hiby.patch` bounds every read to the packet and frame, validates
stream fields and the negotiated rate/channel mode, commits IMDCT history only
for fully valid frames, and fixes upstream decoding bugs (fine-precision cap
and dequantization, descending gradient, dual-channel blocks, mode-2 scale
factor wrap, scale factor 0, S32 gain). Soname, exports and ABI match the
vendor build. During development, a temporary local host harness encoded with
the pinned AOSP encoder and decoded through `dlopen`, reporting rejects and
SNR; that harness is not a repository build input. The small decoder
compatibility header supplies the declarations BlueALSA needs.
Keep libc/loader, OpenSSL, curl, Wi-Fi drivers/firmware and the working player
mbedTLS version unchanged in this batch. Shared-library SONAME filenames do
not establish upstream source versions.

## Sources and build order

Host tools: GCC/C++, make, autoconf, automake, libtool, gettext development
tools, CMake, Meson, Ninja, pkg-config, Git, curl, archive utilities, readelf,
and qemu-user (`qemu-mipsel`). GLib development tools may also be required
for host-side generators.

Archive-based recipes download pinned archives and verify SHA-256. Git-based
recipes require the following checkouts; they reject unexpected revisions.
Clone missing checkouts and select the pinned commit, without resetting an
existing dirty checkout:

| Directory relative to repository | Upstream | Commit |
| --- | --- | --- |
| `scratch/ingenic-toolchain-v5.2` | https://github.com/tobunto/ingenic-toolchain-v5.2 | `4de21963b0f10c19118045f78c3e853a5e2c0be6` |
| `scratch/bluez-alsa-5.0.0` | https://github.com/arkq/bluez-alsa | `1935d6dcb8975f2d7a51aaafe61538d157224623` |
| `scratch/base-upgrade/fdk-aac` | https://github.com/mstorsjo/fdk-aac | `716f4394641d53f0d79c9ddac3fa93b03a49f278` |
| `scratch/base-upgrade/libopenaptx` | https://github.com/pali/libopenaptx | `2459ed4686eaef0a19dfa3f330a960813c5f60de` |
| `scratch/base-upgrade/ldacBT` | https://github.com/EHfive/ldacBT | `6579bd585a618f2e1612b3c1650d2b7fcfb1d43f` |
| `scratch/base-upgrade/libldacdec-anonymix007` | https://github.com/anonymix007/libldacdec | `c90094b15e25aef0e47c6d775fa94aceb36cabbc` |
| `dbus` (existing checkout) | https://gitlab.freedesktop.org/dbus/dbus | `958bf9db2100553bcd2fe2a854e1ebb42e886054` |

Initialize LDAC's submodule: its `libldac` revision must be
`82b6a1abee84787b8fa167efe20290073f60db2d`. The LDAC decoder is built from
anonymix007/libldacdec `c90094b15e25aef0e47c6d775fa94aceb36cabbc` plus
`libldacdec-hiby.patch` (its own `libldac` submodule is not needed).

From the repository root:

```sh
bash scripts/build_base_audio_deps.sh
bash scripts/build_base_zlib.sh
bash scripts/build_base_expat.sh
bash scripts/build_base_bluez_library.sh
bash scripts/build_base_glib.sh
bash scripts/build_base_dbus.sh
bash scripts/build_base_bt_codecs.sh
bash scripts/build_base_bluealsa.sh
make -j4 target
bash scripts/prepare_base_image_overlay.sh
```

Default stock tree: `/home/josegarita/Desktop/Test2/squashfs-root`.
Override `BASE_STOCK_ROOT` and `BASE_CROSS_PREFIX` where needed. Outputs stay
under `scratch/base-upgrade`; the overlay script does not install or flash.
Keep the downloaded sources, notices and these recipes with release source
materials. Runtime notices are included in the overlay.

## Optional R1 vendor driver refresh

The ordinary four-input `scripts/repack_upt.sh` flow keeps the base kernel and
drivers. To opt into the pinned official R1 1.8.b2 kernel and matching
`module_driver` and `lib/firmware` files, pass its UPT explicitly:

```sh
bash scripts/repack_upt.sh --board r1 \
  --vendor-drivers /path/to/approved-r1-1.8.b2.upt \
  BASE_UPT PLAYER_BINARY BOOTLOADER_BINARY OUTPUT_UPT
```

Only vendor UPTs pinned by SHA-256 in `firmware/vendor_drivers.json` are
accepted. The repacker validates the OTA chunk chain, board and version,
kernel image CRCs, then checks that overlays did not change the imported
kernel or driver payload. It preserves the existing R1 AXP2101 charging
limits (4350 mV and 70 mA) and writes source and installed hashes to
`/usr/share/compas/vendor-driver-provenance.json` in the resulting image.
The vendor initialization sequence replaces the old diagnostic timing wrapper;
it loads the same modules in the same order without writing the old boot-profile
log. Provenance records the previous loader hash and this replacement policy.
Other boards have no approved vendor pin and are rejected if this option is
used. This refresh replaces the R1 kernel and matching vendor driver payload;
it is an explicit firmware change and should be hardware-tested before use.

## Local validation and installation

These base-image recipes are local development tools; this guide does not
depend on a checked-in test runner or fixture files. After merging the overlay
into a disposable copy of the matching stock root, check that the target
binaries have the expected architecture and can report their versions or
capabilities under QEMU user emulation:

```sh
root=/absolute/path/to/candidate/root
file "$root/usr/bin/bluealsad" "$root/usr/bin/bluealsactl"
qemu-mipsel -L "$root" "$root/usr/bin/bluealsad" --version
qemu-mipsel -L "$root" "$root/usr/bin/bluealsactl" --help
```

For an R3 Bluetooth overlay, also check the staged BlueZ daemon and CLI:

```sh
qemu-mipsel -L "$root" "$root/usr/libexec/bluetooth/bluetoothd" --version
qemu-mipsel -L "$root" "$root/usr/bin/bluetoothctl" --help
```

For a Wi-Fi overlay, check the staged supplicant's version and compiled
capabilities:

```sh
qemu-mipsel -L "$root" "$root/usr/sbin/wpa_supplicant" -v
qemu-mipsel -L "$root" "$root/usr/sbin/wpa_supplicant" -h
```

Use the actual paths present in the candidate root for components not included
in a particular overlay. These checks exercise the built target binaries and
libraries, but QEMU uses the host kernel. They do not validate kernel boot,
audio quality, Bluetooth pairing, Wi-Fi association, suspend, or radio
firmware behavior; those require testing on the matching hardware.

Earlier local development checks passed eager dynamic linking of new and
retained tools, both BlueALSA plugins, compression roundtrip, XML parsing,
ALSA configuration allocation, and SBC encoding/decoding. A null-device
ALSA playback/capture check also passed under QEMU and on the R1 kernel/libc.
These are historical results from local harnesses, not commands or test files
provided by this repository.

Back up the extracted stock tree on the workstation before applying the
overlay to Test2. Do not put backups in `/usr/data`. Preserve existing D-Bus
policies/init configuration and helper permissions. The existing Test2 packer
uses `mksquashfs -all-root`; this matters because staging files are owned by
the build user. The stock activation helper is 0755; this upgrade does not
silently add setuid privileges. Service activation requiring elevated
privileges is not enabled or newly validated here.

BlueALSA 5 renames `bluealsa` to `bluealsad` and `bluealsa-cli` to
`bluealsactl`. The overlay updates device startup scripts and includes the
matching player. An older SD-card override can supersede that player at boot:
make sure the running binary is the upgraded build. Old executables are
retained for rollback/loader testing, not intended to control the new daemon.
The old and new BlueALSA policy filenames are deliberately both retained.

Before a release, test SBC/SBC-XQ/AAC/aptX/aptX-HD/LDAC playback, Bluetooth
DAC mode, volume sync on/off, reconnect, local audio, Wi-Fi, and suspend/wake
on the actual R1. Repacking/flashing is a separate step.

## Kernel boundary

The stock kernel is Linux 4.4.94+ and the vendor modules require its exact
MIPS32_R2 ABI. The available mainline/Letux tree lacks an R1 board definition
and validated audio, display/touch and other device support. Replacing the
kernel safely requires the vendor source/configuration or a full board port;
it is not a drop-in package upgrade. The optional pinned R1 refresh imports
the matching official vendor kernel and modules as one unit; otherwise keep
`xImage` and modules unchanged.

### Experimental source-built R1 kernel

`firmware/kernel/upstream.json` pins Jepl4r's
[hiby-custom-kernel](https://github.com/Jepl4r/hiby-custom-kernel), which builds
4.4.94+ from the external Ingenic SDK and preserves the R1 device tree and
vendor-module ABI. Its credits include
[MatthewBriggs's R1 kernel work](https://github.com/MatthewBriggs/hiby-r1-linux-kernel-compiling).
The kernel source/toolchain SDK is not bundled or available in this checkout.

Prepare an isolated workspace from a clean checkout at the pinned commit and
the extracted **base firmware's** `xImage`:

```sh
python3 scripts/kernel/prepare_r1_kernel.py \
  --upstream /path/to/hiby-custom-kernel \
  --stock-kernel /path/to/base/xImage \
  --workspace /path/to/fresh-compas-kernel-workspace \
  --profile optimized \
  --jobs 2
```

This extracts the stock DTB, copies the pinned source and selected profile,
and records source, kernel, DTB and profile hashes plus the container command
in `preparation.json`. `--jobs` controls upstream `build.sh` parallelism and
defaults to 2 to limit host memory use. The pinned SDK SHA-256 is checked when
the archive is supplied. The SDK is available from the vendor's X1600 SDK
directory at
`ftp://ftp.ingenic.com.cn/DevSupport/X1600/01_SW/06_kernel4.4.94_X1600-sdk_v6.0-20240606/01_ingenic-linux-kernel4.4.94-x1600-v6.0-20240606/`;
public access instructions are documented at
https://github.com/hiby-modding/hiby_os_crack.

Preparation does not flash, change the upstream checkout or run a build by
default. To build, pass `--sdk /path/to/ingenic-linux-kernel4.4.94-x1600-v6.0-20240606.tar.bz2
--build`. The runtime defaults to Docker when installed, otherwise Podman; use
`--container-runtime docker` or `--container-runtime podman` to select one
explicitly. Podman builds apply SELinux mount labels automatically. Build the
pinned kit's image locally before `--build` (choose the matching runtime):
the build container sets `TAR_OPTIONS=--no-same-owner` so rootless Podman can
extract the vendor SDK even when its archived numeric owners exceed the
container's mapped user-ID range.

```sh
cd /path/to/hiby-custom-kernel
podman build --platform linux/amd64 -t hiby-custom-kernel docker/
```

Preparation is not evidence that a kernel compiled or boots.

After a build, verify the module set that the firmware will actually retain.
For a kernel-only replacement that keeps the vendor drivers, use:

```sh
python3 scripts/kernel/verify_module_abi.py \
  --system-map /path/to/workspace/hiby-custom-kernel/out/System.map-compas-r1 \
  --built-modules /path/to/workspace/hiby-custom-kernel/out/modules-compas-r1 \
  --vendor-modules /path/to/base/root/module_driver \
  --vendor-only --output /path/to/workspace/vendor-module-abi.json
```

This checks actual exported symbols and full module vermagic, records artifact
hashes, and fails for unresolved strong imports. In vendor-only mode, rebuilt
modules cannot satisfy imports; their vermagic still verifies the build's
module identity. Static checks do not prove structure layouts, load order or
runtime behavior. Device validation and the repository's C review gate
remain required before shipping a kernel.

The preparation step also applies reviewed module-source patches from
`firmware/kernel/module-patches/` to the copied kit before a build. These are
separate from `firmware/kernel/patches/`, which the upstream build applies to
the kernel tree. The reconstructed-driver stream replaces 22 selected R1
modules and retains the seven matching vendor modules; `bcm_wlbt_power` is not
a drop-in replacement because the Wi-Fi path also depends on `brcmfmac` and
the matching boot scripts. Keep the vendor Wi-Fi modules until that complete
path is reviewed and validated.

The kernel-only packer keeps the root filesystem image bytes unchanged:

```sh
python3 scripts/kernel/repack_kernel_upt.py \
  --base-upt /path/to/compas-r1-kernel.upt \
  --kernel-workspace /path/to/workspace \
  --output /path/to/kernel-only.upt
```

When replacing reviewed drivers, use `scripts/kernel/repack_driver_upt.py`;
it rebuilds the root filesystem and checks that only the selected 22 module
files changed. `--driver-modules /path/to/rebuilt-modules` selects the reviewed
module output directory and defaults to the kernel workspace's
`out/modules-compas-r1`. Driver replacement remains subject to source review,
ABI verification, and device testing.

Both profiles apply upstream `r1-required.config` and `r1-parity.config`
before our fragment. `parity` keeps kernel debugging symbols for diagnosis;
`optimized` adds compressed-RAM-swap support and proposes removal of
unused mac80211, in-kernel NTFS and Bluetooth High Speed. It preserves
codepage 936, which Compas's FAT mount commands require, and the ABI-sensitive
netfilter/debugfs options. Unlike upstream's RAM profile, it retains crypto
self-tests and does not remove crypto algorithms. Compiling zram support does
not activate swap; activation and its CPU/memory tradeoff need their own test.
Both CFQ and deadline remain available with CFQ as the optimized profile's
default; the first device scheduler comparison was mixed.

The pinned kit includes the highatomic-reservation guard for the R1's oversized
page blocks and the DMA/USB/ALSA compatibility patches. Those are candidates
for reuse, not measured improvements in Compas. The reconstructed Wi-Fi,
display, touch and audio modules need separate device validation; copying
the reconstructed Wi-Fi modules alone would leave Compas's vendor-Wi-Fi scripts inconsistent.

Compas's local corrective patches in `firmware/kernel/patches/` apply after
the pinned upstream patches. They restrict USB DAC control OUT requests to
the supported four-byte clock-frequency value, validate rates, preserve
character-device lifetime across open handles, clean failed endpoint starts,
and bound and independently encode signed DMA gaps. Preparation records their
hashes without changing the upstream checkout. Local patch names cannot begin
with `0` or collide with upstream patch names, which prevents double application.
Run `scripts/kernel/test_uac_safety.py --source /path/to/built/f_uac_sa.c
--dma-source /path/to/built/ingenic_dma.c` to compile and exercise the actual
functions with host-side mocks. These tests do not replace device testing.

The ordinary `repack_upt.sh` path still retains its selected base kernel.
Custom-kernel firmware packaging and release are pending review and device
validation, including repeated warm/cold boots, SD access, charging, touch,
audio formats/underruns, USB DAC and Bluetooth/Wi-Fi. Require matching module
exports/vermagic, byte-identical DTB and a kernel no larger than the base image.

The existing `compas-boot-tuning` helper also accepts an opt-in
`COMPAS_SD_SCHEDULER=deadline`, `cfq` or `noop`, but writes only an advertised
scheduler. An unset or invalid value retains the kernel's current scheduler.
The tested stock R1 already exposes deadline; a new kernel is not needed to
compare it against CFQ. No scheduler override is enabled by default.

## Player-facing follow-up batches

These overlays are deliberately separate from the initial shared-library
refresh. Build and validate them individually before combining a release:

```sh
bash scripts/build_base_alsa_utils.sh
bash scripts/prepare_player_base_overlay.sh alsa

bash scripts/build_base_bluez.sh
make -j4 target
bash scripts/prepare_player_base_overlay.sh bluetooth

bash scripts/build_base_wifi.sh
bash scripts/prepare_player_base_overlay.sh wifi
```

Outputs: `scratch/base-upgrade/player-overlays/{alsa,bluetooth,wifi}/current`.
Each has a sibling `files.txt` manifest. Nothing is installed or flashed by
these scripts. The Bluetooth overlay assumes the first batch's GLib/D-Bus
libraries are already present and includes the matching player binary.

- **ALSA utilities 1.2.16:** only `aplay`, `arecord`, and `amixer` are installed.
  Keep the vendor mixer/state configuration and existing `alsamixer`.
  Earlier local null-device checks exercised the player's raw-PCM pipe at
  44.1/48/192 kHz in S16_LE/S24_LE, plus null capture; no repository test
  helper is required for this base recipe.
- **BlueZ 5.87:** upgrade `bluetoothd`, `bluetoothctl`, `hciconfig`, `hcitool`
  and `btmon`. Preserve UART/firmware loaders (`hciattach`,
  `brcm_patchram_plus`, etc.), init scripts, configuration and pairing data.
  The daemon's generated `STORAGEDIR` is redirected to `/usr/data/bluetooth`,
  matching the device's persistent stock pairing directory; `/var/lib/bluetooth`
  remains the unrelated configure-time state default and is not used for
  pairing data. The build recipe verifies both `config.h` and staged daemon
  strings before publishing its stage.
  Mesh and LE Audio/related new audio profiles are disabled for Linux 4.4;
  classic A2DP/AVRCP remain. The player detects the supported paired-device
  command from CLI help, retaining compatibility with stock BlueZ 5.54.
- **wpa_supplicant 2.12 / libnl 3.12.0:** install supplicant, CLI, client
  library, and only libnl core/generic-netlink modules. Preserve saved
  networks, DHCP and vendor drivers. The checked-in `wpa.config` keeps
  nl80211/WEXT/wired, enterprise EAP, WPS, HS20/interworking and CLI control;
  it does not inherit upstream's AP/P2P/MACsec/D-Bus defaults. The `none`
  backend supports isolated control-interface testing without touching a
  physical interface. WPA3/other capabilities still depend on device support.

The radio recipes obtain pinned development headers rather than installing
host development packages. Readline 8.0 and OpenSSL 1.1.1f **runtime libraries
are retained**, not upgraded. In particular, this does not fix vulnerabilities
in the retained old OpenSSL itself. The OpenSSL sources are used only to
generate matching public headers. The player's separate TLS implementation
is unchanged.

Validation so far: ALSA null playback/capture passed on both QEMU and the
actual R1 kernel/libc. BlueZ 5.87 version/help and read-only CLI queries ran
on the R1 from `/tmp`, without replacing the installed daemon. These do not
establish headphone pairing, audible playback, Wi-Fi association, or suspend
correctness; those remain release gates. Keep radio overlays out of Test2
until the corresponding device tests are completed.

Earlier local Wi-Fi checks passed on the R1: a temporary host-side check
resolved `nlctrl` and `nl80211` through the new libnl, and an isolated
supplicant instance using the `none` backend answered `PONG` and reported the
TLS/PEAP/TTLS/FAST methods. Those temporary helpers and configuration are not
repository artifacts; the checks did not use saved networks or take over
`wlan0`.

RAM caution: `/tmp` is RAM-backed and the R1 exposes about 56 MiB total RAM.
Keeping several batches of test binaries/libraries there exhausted enough
headroom to prevent ADB shell creation during testing. Removing unused test
copies restored access without a reboot. Stage one batch at a time, keep at
least 8 MiB available before the isolated supplicant test, and remove test
copies afterward. This is not a substitute for memory testing the final
flash-backed image during real playback and radio activity.
