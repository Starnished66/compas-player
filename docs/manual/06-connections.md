# Connections

[User manual](README.md)

Choose the section for what you want to connect. **Bluetooth headphones** play music from Compás. **USB DAC** and **Bluetooth DAC** let you listen to audio sent by another device through the player's output.

## Connect to Wi-Fi

1. Open **Wireless > Wi-Fi** and turn **Wi-Fi** on.
2. Select your network and enter its password. If the network is hidden, use **Manual SSID Entry**; SSID means the network's name.
3. Use **Wi-Fi Info** to check the connection details.

![Wi-Fi screen](images/wifi.png)

Internet access is needed for online updates, plugin downloads and online services. Remote control and music servers on your own network can work without internet access.

## Listen with Bluetooth headphones or a speaker

1. Put the headphones or speaker into pairing mode.
2. Open **Wireless > Bluetooth** and turn Bluetooth on.
3. Select the device from the nearby devices list to pair and connect.
4. Play a track in Compás. Previously paired devices appear in their own list when you want to reconnect.

![Bluetooth screen](images/bluetooth.png)

To send music in the other direction, from a phone to the player, open **More > Bluetooth DAC**. This is a separate receiving mode; leave it when you want to return to playing the player's library.

## Copy files over USB

Select **Settings > System > USB Mode > Storage**, then connect the player to your computer with a data-capable cable. Open the SD card on the computer, copy your files, and eject it safely before disconnecting.

If the card does not appear, check the USB mode and reconnect the cable. If **ADB** is enabled under **Settings > System > About > Developer Options**, turn it off: this developer connection takes over the USB port while enabled.

## Use the player as a USB DAC

A USB DAC lets you listen to computer audio through the player's headphone output.

1. Connect the player to the computer over USB.
2. Open **Settings > System > USB Mode > USB DAC**, or **More > USB DAC**.
3. Select the player as the computer's audio output if it is not selected automatically.
4. Leave USB DAC mode on the player when you have finished.

ADB must be off before USB DAC can use the port.

## Send audio over your home network

![Wireless menu](images/wireless.png)

Connect the player and the sending device to the same Wi-Fi network, then enable the receiver you want:

| To send from | On Compás | On the sending device |
|---|---|---|
| An iPhone, iPad or Mac using AirPlay | Open **Wireless > AirPlay** and turn it on. | Select the player as the AirPlay destination. |
| An app that supports DLNA/UPnP casting | Open **Wireless > DLNA** and enable **DLNA Renderer**. | Choose the player as the destination and select music to send. |

The player receives the audio and plays it through its own output. The sending app determines which playback controls are available.

If the player is not listed, check that both devices are on the same network. Guest networks sometimes prevent devices from finding one another.

## Control Compás from a phone or browser

1. Open **Wireless > Remote** and enable **Remote Control**.
2. Use the displayed address or QR code in a compatible app or browser, and enter the **Connection PIN** when asked. Bluetooth remote control also requires pairing.
3. To copy music over Wi-Fi, start file import from a compatible remote-control app. Keep the player connected while files transfer.

Changing the Connection PIN means connected apps will need the new PIN. File import runs for that transfer session.

## Play from a Subsonic music server

Subsonic is a way to browse and play music stored on a server rather than on the SD card. You need the server's address and an account with access to its music.

Open **Subsonic** from **Stream Media** and enter the connection details, or choose a saved server. A server on your home network needs a working local connection; an online server also needs internet access. Tidal and Qobuz are available through optional [plugins](07-plugins.md) with their own account requirements.

<!-- Sources: gui_network.c Wireless/Wi-Fi/Bluetooth/Remote/DAC builders; gui_settings.c System/More/Developer Options; gui_subsonic.c connection screen. -->
