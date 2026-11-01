# Collect information for a device port

CompásOS runs on music players whose stock firmware is HiBy-style embedded Linux
(the HiBy R1, R3 Pro II, R3 II 2025 and TempoTec V1). The scripts in this folder
collect what a port to a similar player needs: processor, memory, display, buttons,
touch, audio outputs and DAC controls. They only read from the player and never
change it.

## Before you start

On your computer:

- Python 3.10 or newer.
- `adb`, from Android SDK Platform Tools. The player does not need to run Android;
  stock HiBy-style firmware ships its own ADB service.

Put `collect_device_info.py` and `device_button_mapping.py` in one folder. The
TempoTec V3 Blaze probe also needs `v3_blaze_probe.py` in the same folder.

On the player, enable ADB. On HiBy-based stock firmware this should be
**System settings > USB device mode > Dock**. Connect the player by USB and check
that `adb devices` lists it as `device`. If the player has no ADB mode, these
scripts cannot collect anything; say so when you ask for a port.

## Run the general collector

From the folder with the scripts:

```sh
python3 collect_device_info.py
```

On Windows, use `py` instead of `python3`. Options:

- `--serial ID`: pick the player when `adb devices` lists more than one device.
- `--output DIR`: choose the report folder (default `device-report-<time>` in the
  current folder).
- `--adb PATH`: use a specific `adb` executable.
- `--skip-interactive`: run only the automatic part.

The automatic part records the kernel, CPU, memory, partitions, loaded and available
kernel modules, display, input devices, ALSA cards and mixer controls, jack
switches and USB interfaces. Some checks are meant for other systems and will report
as unavailable on HiBy firmware, for example the Android properties check. This is
expected.

### Name every physical control

The script then asks for button names. Type a name such as `Play/Pause`,
`Volume up`, `Previous` or `Power`, press Enter, and press that control once.
Repeat for every button, switch and each direction of a wheel. Leave the name
empty to finish. Use a short press for Power and avoid reset or shutdown
combinations; the stock player still reacts to every press.

### Name every audio output

Next it asks for each physical output, for example `3.5 mm headphones`,
`4.4 mm balanced`, `Line out`, `Optical`, `USB DAC output` or `Bluetooth`. List
only outputs the player has. For each one, plug in a matching cable or headphones
if you have them, select that output in the stock settings if needed, and press
Enter. Leave the name empty to finish. An output with nothing plugged in can still
be listed; say that no accessory was available.

## TempoTec V3 Blaze probe

For the TempoTec V3 Blaze, run this instead of the general collector:

```sh
python3 v3_blaze_probe.py
```

It accepts the same options. It checks the differences we know of from the
HiBy R3 II 2025, which uses almost the same hardware: the buttons (volume keys
instead of a volume wheel), touch orientation, the 3.5 mm optical output, the
AK4493 DAC controls and the missing inline-remote input. It then guides you to:

1. Press each button, and hold Volume up for about two seconds.
2. Tap the top-left and bottom-right corners of the screen.
3. Plug in 3.5 mm headphones, 4.4 mm headphones and a 3.5 mm optical cable, one at
   a time, if you have them.
4. With headphones plugged in, change stock audio settings one at a time (gain,
   digital filter, line out) and name each change.
5. Play a few files in different formats, including DSD if you have one, and
   describe each format.

Type `s` to skip any step. Ctrl+C ends the current step and keeps what was
collected. It takes about ten minutes.

## Send the report

Each script writes a report folder and prints its location. The V3 Blaze probe
also writes a zip of that folder. Open the report's `README.md` to see the
results, then send the folder or zip to the CompásOS developer, with:

- the exact model and hardware revision,
- the stock firmware version and the official firmware download link,
- the screen size and a list of the physical controls and outputs.

The report excludes music, accounts, Wi-Fi settings and system logs. Serial
numbers, MAC addresses and mount paths are removed where recognized. Stock
firmware can still expose unexpected identifying details, so look through the
report before sending it.

## What the report cannot tell

The report shows which interfaces exist and how they react. It does not prove that
an output plays audio or which numeric mixer value selects which connector, and it
cannot measure charging voltages or show how to recover a player that fails to
boot. Those need the stock firmware and testing on the device.
