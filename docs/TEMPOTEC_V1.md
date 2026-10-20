# TempoTec Variations V1

Compás on the TempoTec Variations V1. The player is HibyOS on an Ingenic X1600, the same class of machine as the R1 and R3 Pro II. This port does not replace the kernel or the audio drivers. It adds a board profile and the UI that fits a 240×320 panel.

The button map and the jack note below are taken from the Sonix V1 fork (`mr-f0xx/sonix-player-tempotec-v1`), which has been run on a V1. They are not guesses from a datasheet. Build with `make host BOARD=tempotec_v1` or `make target BOARD=tempotec_v1`. There is no `v1.upt` packer yet.

## What is in this branch

- Board `tempotec_v1`: 240×320, a Previous key, update filename `v1.upt`, install marker `COMPAS_PLAYER_BOARD:tempotec_v1`.
- Home is a scrolling list unless a plugin asks for tiles.
- Now Playing is the poster layout: a 240×188 cover at the top, then title, artist, format, progress, and the transport row. The album line is hidden because the deck has no room for it.
- Compact font sizes and transport icons, shared with the R3 II 2025 path.
- Onboard skip keys are swapped, and the screenshot chord is Power + Volume Up.
- The R1 charger-register writes and the R1 USB-role node stay off. Their V1 equivalents are not confirmed.

## Buttons

Left flank: Volume Up, Volume Down, then the microSD slot. Right flank, top to bottom: Power, Previous, Play, Next.

The V1 kernel labels the skip keys backwards, the same way the R3 Pro II does. On `md-gpio-keys` and `jz adc keyboard`:

| Kernel code | Physical key | Default action | Hold |
| --- | --- | --- | --- |
| `KEY_NEXTSONG` | Previous | Previous track | Seek back. Same timing as Next on the other boards |
| `KEY_PREVIOUSSONG` | Next | Next track | Seek forward |
| `KEY_PLAYPAUSE` | Play | Play / pause | — |
| `KEY_VOLUMEUP` / `KEY_VOLUMEDOWN` | Volume | One step | Repeats |
| `KEY_POWER` | Power | Screen on / off | Power menu |

A cable remote (`earpods_adc`) and Bluetooth AVRCP are not swapped. Power + Volume Up takes a screenshot. Power + Volume Down does not.

## Jack and outputs

`/sys/class/switch` on the measured V1 has `headset` only, no `balance`. That node reads `1` with both sockets empty, so `1` is unplugged. Any other value is still reported as the 3.5 mm jack until a plug is measured. The V1 has two CS43131s and both a 3.5 mm and a 4.4 mm jack. It must not take the R1 single-DAC shortcut. That route selection is not wired yet.

## Still open

- A stock `v1.upt` to read the charge-voltage script, the USB role node, the touch ranges, and the real 4.4 mm switch value.
- Firmware repack. Do not flash an R1 or R3 image.
- Settings pages, the equaliser, and the keyboard still use the scaled 480px layout. They need a pass on the simulator.
- On-device proof: boot, touch, every key, both jacks, sleep and wake, a library scan, Wi-Fi, Bluetooth, update, and stock restore.
