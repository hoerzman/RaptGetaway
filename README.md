# RaptGetaway

RaptGetaway is an ESP32-S3 gateway for RAPT Pill hydrometers. It reads Pill
advertisements over BLE, provides a local web page for setup, and forwards
measurements to Brewfather Custom Stream.

## Acknowledgements

The project idea and direction came from the project maintainer. The software
was developed with substantial coding assistance from GitHub Copilot.

## Features

- Supports RAPT Pill advertisement payload versions 1 and 2.
- Supports up to 12 configured Pills, each with its own Brewfather stream URL.
  Testing so far has been limited to two Pills; support for more has not yet
  been verified in practice.
- Provides a web interface in English and German.
- Scans Wi-Fi networks and saves the selected network on the ESP32.
- Turns off the setup access point after connecting to Wi-Fi and enables it
  again if the Wi-Fi connection is lost.
- Provides a manual test-send button for each configured Pill.
- Estimates battery voltage from the Pill's battery percentage.

## Hardware and software

- ESP32-S3 development board supported by PlatformIO's
  `esp32-s3-devkitc-1` target.
- A RAPT Pill with Bluetooth telemetry enabled and within BLE range.
- PlatformIO Core or the PlatformIO IDE extension for Visual Studio Code.
- A 2.4 GHz Wi-Fi network and a Brewfather Custom Stream URL for each Pill.

No separate wiring is required for the Pill; the ESP32 receives its BLE
advertisements wirelessly.

## Build and configure

1. Install PlatformIO and open this project.
2. Copy `include/config_local.example.h` to `include/config_local.h`.
3. In `config_local.h`, set a unique setup access point password of at least
   eight characters. This local file is ignored by Git and must not be
   committed.
4. Build and upload the firmware:

   ```text
   pio run -e esp32-s3-devkitc-1 -t upload
   ```

5. Upload the web interface to LittleFS:

   ```text
   pio run -e esp32-s3-devkitc-1 -t uploadfs
   ```

The setup access point is named **RaptGateway-Setup**. Connect using the
password configured in `include/config_local.h`, then open
`http://192.168.4.1`.

Select the home Wi-Fi network, enter its password, and save. After the ESP32
connects, the setup access point turns off. Find the ESP32's assigned address
in the router's client list and open that address to return to the web page.
If the home Wi-Fi is lost later, the setup access point is enabled as a
fallback. BLE scanning runs while the ESP32 is connected to Wi-Fi.
<img width="604" height="585" alt="2026-10-28 17_21_15-RaptGetawayWifi setup" src="https://github.com/user-attachments/assets/90aea3bc-767a-4d3f-ba77-8b8a556dceee" />

## Configure Pills and Brewfather

Keep a Pill within BLE range. Once discovered, assign it a unique Brewfather
device name and its complete Brewfather Custom Stream URL in the web page.
Repeat for each Pill.

The same Custom Stream URL can be used for multiple Pills if they should send
to the same Brewfather account. If you use multiple Brewfather accounts, enter
the URL for the intended account for each Pill. The URL must start with
`https://`. Brewfather's app may currently copy a link starting with
`http://`; change that prefix to `https://` before saving it.

The stream URL contains a private Brewfather logging ID. Keep it secret; do
not add it to source files, screenshots, issue reports, or commits. The ESP32
stores its Wi-Fi and Pill configuration in non-volatile storage.

Each configured Pill has a **Test send now** button. It sends that Pill's
latest measurement immediately when the sample is no more than 20 minutes old.
A test send restarts that Pill's 15-minute automatic upload interval.
Brewfather may enforce its own rate limit.
<img width="603" height="717" alt="2026-10-28 17_22_20-PillConfig" src="https://github.com/user-attachments/assets/a998cb97-e067-44ad-8711-2c73846b600b" />


## Data sent

The gateway sends temperature in degrees Celsius, gravity as specific gravity
(SG), and estimated battery voltage. The Brewfather payload identifies gravity
with `gravity_unit: "G"`. The Pill's gravity value is SG even when the RAPT app
displays degrees Plato.

RAPT reports battery percentage. The gateway estimates voltage using
`3.3 + (percentage / 100 × 0.9)` and sends this estimate in Brewfather's
`battery` field. It is not a direct battery-voltage measurement.

Brewfather allows at most one update per device name every 15 minutes. The
gateway tracks this interval independently for each Pill and sends only recent
measurements. After boot, it waits 15 minutes before its first automatic post.

## Tests and diagnostics

Run parser tests with the ESP32 connected:

```text
pio test -e esp32-s3-devkitc-1
```

Open the PlatformIO serial monitor at 115200 baud to see decoded readings and
upload results:

```text
pio device monitor -b 115200
```

If a Pill does not appear, verify that its Bluetooth telemetry is enabled and
bring it within range. If no web page appears, confirm that the LittleFS image
was uploaded. If Wi-Fi setup fails, reconnect to the setup access point and
check the password in the local configuration header.

## Security notes

- Use a unique setup access point password and protect Brewfather stream URLs.
- The gateway currently encrypts HTTPS but does not validate the Brewfather
  server certificate. Use only on a trusted network until certificate
  validation is added.
- The setup web server is intended for use on a trusted local network.

## Parser and third-party components

The project was inspired by
[`RaptHooker`](https://github.com/roguecode/RaptHooker), whose work encouraged
me to build this gateway.

The C++ RAPT advertisement decoder in `include/rapt_parser.h` implements the
RAPT payload layout and conversions described by the
[`sairon/rapt-ble` parser](https://github.com/sairon/rapt-ble/blob/main/src/rapt_ble/parser.py).
The upstream parser is MIT-licensed; see [THIRD-PARTY-NOTICES.md](./THIRD-PARTY-NOTICES.md)
for attribution and dependency notices.

The RAPT Pill Bluetooth transmission format is also documented in the
[RAPT Pill Hydrometer Bluetooth Transmissions reference](https://gitlab.com/rapt.io/public/-/wikis/Pill%20Hydrometer%20Bluetooth%20Transmissions).

PlatformIO fetches NimBLE-Arduino and ArduinoJson as declared in
`platformio.ini`. Wi-Fi, web server, Preferences, DNS, HTTP client, and LittleFS
are provided by the ESP32 Arduino framework. These dependencies are not
vendored into this repository.

## License

RaptGetaway is distributed under the MIT License. See [LICENSE](./LICENSE) and
[THIRD-PARTY-NOTICES.md](./THIRD-PARTY-NOTICES.md).
