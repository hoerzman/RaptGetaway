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
- Configures an automatic Brewfather upload interval independently for each
  Pill: 15 or 30 minutes, or 1, 2, or 4 hours. The default is 30 minutes.
- Provides a web interface in English and German.
- Shows the latest BLE signal strength for discovered Pills and colors BLE and
  Wi-Fi RSSI values as a rough signal-quality indication.
- Scans Wi-Fi networks and saves the selected network on the ESP32.
- Turns off the setup access point after connecting to Wi-Fi and enables it
  again if the Wi-Fi connection is lost.
- Provides a manual test-send button for each configured Pill.
- Estimates battery voltage from the Pill's battery percentage.

## Hardware and software

- ESP32-S3 development board supported by PlatformIO's
  `esp32-s3-devkitc-1` target.
- The tested gateway hardware has 8 MB flash and an external 2.4 GHz antenna.
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

On first setup, create an administrator password (12–64 characters) before
configuring Wi-Fi. Select the home Wi-Fi network, enter its password, and save.
The setup access point remains active until both an administrator password is
set and the ESP32 connects to Wi-Fi (including when updating an older
installation without a password). The web page is then available at the
device-specific mDNS address printed to the serial console,
for example `https://raptgetaway-a1b2c3.local/`. The gateway generates and
persists its own self-signed certificate and private key; identities are not
shared between devices.

Browsers do not automatically trust a self-signed certificate. Before accepting
the browser's certificate warning, compare the certificate's SHA-256 fingerprint
with the fingerprint printed in the device serial console. Do not treat the
warning as safe to ignore without this independent comparison. Use the mDNS
hostname, not the router-assigned IP address, because the certificate is issued
for that hostname. Sign in with the administrator password to manage the
gateway.

The setup web page uses HTTP only while the WPA2-protected setup access point is
active. Once connected to home Wi-Fi, plain HTTP requests are redirected to
HTTPS and administration APIs are not served over the LAN HTTP interface. If
the home Wi-Fi is lost later, the setup access point is enabled as a fallback.
BLE scanning runs while the ESP32 is connected to Wi-Fi.
<img width="604" height="585" alt="2026-10-28 17_21_15-RaptGetawayWifi setup" src="https://github.com/user-attachments/assets/90aea3bc-767a-4d3f-ba77-8b8a556dceee" />

The setup page includes common Android, Apple, and Windows captive-portal
detection URLs and redirects those checks to the setup page. Automatic portal
pop-ups are controlled by the phone/computer operating system and are not
guaranteed; if no pop-up appears, open `http://192.168.4.1` manually.

The setup page focuses on administrator-password and Wi-Fi setup; it does not
show Pill discovery. On the HTTPS administration page, Pill management is shown
first and Wi-Fi and factory-reset controls are under **Settings**. The
**Settings** and **Sign out** buttons are side by side below the language
selector. The status line includes the connected Wi-Fi signal strength (RSSI)
in dBm.

To clear Wi-Fi settings, Pill configuration, the administrator password, and
the generated TLS identity, use **Settings → Factory reset** while signed in,
or hold **GPIO4** connected to **GND** for at least five seconds. The ESP32
then clears its saved settings and restarts. The setup access point password
in `config_local.h` is build-time configuration and is not changed by this
reset.

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
A test send restarts that Pill's configured automatic upload interval. The
interval can be set independently for each Pill to 15 or 30 minutes, or 1, 2,
or 4 hours; newly configured Pills default to 30 minutes. Existing saved Pill
configurations without an interval also adopt the 30-minute default. After each
gateway boot, the first automatic send is attempted as soon as Wi-Fi, a recent
Pill measurement, and synchronized time are available; subsequent attempts use
the configured interval.
Brewfather may enforce its own rate limit.

<img width="699" height="806" alt="2026-10-29 17_19_46-RaptGetaway" src="https://github.com/user-attachments/assets/7cfb7408-9f07-4d83-94ce-f3be0cc9df04" />



After saving a Brewfather URL, the gateway does not show or return it in the
web interface or API. The page indicates that a URL is configured and shows
the latest Brewfather send result and its date/time in the browser's local time
zone since the device started; both accepted and rejected responses include the
time when available. To change the URL, enter a replacement; leave the field
blank to keep the saved URL.

## Data sent

The gateway sends temperature in degrees Celsius, gravity as specific gravity
(SG), and estimated battery voltage. The Brewfather payload identifies gravity
with `gravity_unit: "G"`. The Pill's gravity value is SG even when the RAPT app
displays degrees Plato.

The administration page shows the latest received BLE RSSI for each Pill.
For an online Pill, the BLE signal appears beside the address and online status.
When a Pill is not recently seen, the page shows when its last advertisement and
last measurement were received. Wi-Fi and BLE RSSI values are colored green at
-65 dBm or stronger, yellow from -75 to -66 dBm, and orange below -75 dBm.
These are rough signal-quality bands, not a distance estimate; radio conditions
can make readings fluctuate.

## Antenna and stainless-steel tanks

Stainless steel can strongly attenuate and reflect 2.4 GHz radio signals. Beer
also weakens radio signals, so a Pill inside a stainless-steel tank may be
difficult to receive, particularly when it is partly submerged. The project
developer uses and recommends an external antenna for the gateway. When
compatible with the board and correctly connected, placing it outside the tank
with a clearer path to the Pill can improve reception.

A physically larger antenna does not automatically have more gain; antenna
design, orientation, placement, cable loss, and nearby metal all matter. A
gateway antenna upgrade does not increase the Pill's transmit power or
guarantee reception through a closed metal tank.

Use an antenna specified for 2.4 GHz and for the board's antenna connector.
Higher-gain antennas can also affect transmit EIRP, so stay within the radio
regulations that apply to your location. See Espressif's
[ESP32-S3 RF layout guidance](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html#rf).

RAPT reports battery percentage. The gateway estimates voltage using
`3.3 + (percentage / 100 × 0.9)` and sends this estimate in Brewfather's
`battery` field. It is not a direct battery-voltage measurement.

Brewfather allows at most one update per device name every 15 minutes. The
gateway tracks the configured interval independently for each Pill and sends
only recent measurements. After boot, it waits that Pill's configured interval
before its first automatic post.

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
- Outbound Brewfather HTTPS validates the server certificate chain against the
  Mozilla CA bundle embedded in the firmware and checks the requested hostname.
  The gateway synchronizes its clock with SNTP before sending; if time is not
  synchronized or TLS verification fails, the request is not sent. Refresh the
  root bundle during firmware maintenance; its source and generation details
  are recorded in [certs/README.md](./certs/README.md).
- Initial administration is password-protected and limited to the setup access
  point. Home-network administration uses HTTPS and a password-protected
  session; only one administrator session is active at a time. Signing in from
  another browser or device invalidates the previous session, and the previous
  browser reports that it needs to sign in again. The self-signed certificate
  must be verified out-of-band using its SHA-256 fingerprint.
- Wi-Fi credentials, Pill configuration, the administrator password verifier,
  and the generated TLS private key are stored in ESP32 NVS. They are not
  protected against physical flash extraction unless flash encryption is
  provisioned and enabled for the deployed device. A factory reset clears
  stored settings but is not a substitute for flash encryption.
- Hiding saved Brewfather URLs from the page/API reduces accidental disclosure.
  The URLs still exist in device storage and are used for outbound requests.
- RAPT Pill readings arrive as unauthenticated BLE advertisements. The
  documented v1/v2 payloads contain no signature or message-authentication
  field; the v1 MAC and BLE address identify a device but do not prove who
  transmitted the reading. A nearby transmitter may spoof measurements.
  There is no gateway-only way to verify a reading's origin with this format;
  that requires protocol support from RAPT and the Pill. Treat readings as
  untrusted telemetry, not as authenticated evidence.

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
