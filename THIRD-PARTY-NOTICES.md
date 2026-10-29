# Third-party notices

RaptGetaway depends on third-party software and implements the RAPT BLE
advertisement format described by the project listed below. Dependencies are
fetched by PlatformIO and are not vendored in this repository.

## RAPT BLE advertisement format

The decoder in `include/rapt_parser.h` is a C++ implementation of the
advertisement layout and measurement conversions documented by
[`sairon/rapt-ble`](https://github.com/sairon/rapt-ble), specifically its
[`parser.py`](https://github.com/sairon/rapt-ble/blob/main/src/rapt_ble/parser.py).

Copyright (c) 2023 Jan Čermák

The upstream project is distributed under the MIT License:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

## Resolved PlatformIO dependencies

The versions below were resolved on 2026-10-29. They are a record of that
environment, not version pins: `platformio.ini` leaves the platform and
libraries unconstrained.

- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino), resolved as
  2.5.2 — Apache-2.0. Its upstream NOTICE credits Apache Mynewt NimBLE and
  Espressif modifications. The source package also includes TinyCrypt and
  micro-ecc license notices. The corresponding full texts are supplied in
  [`third_party_licenses/`](./third_party_licenses/).
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) by Benoit Blanchon,
  resolved as 7.4.3 — MIT. Its full license text is included in
  [`third_party_licenses/ArduinoJson-MIT.txt`](./third_party_licenses/ArduinoJson-MIT.txt).
- [Unity](https://github.com/ThrowTheSwitch/Unity), resolved as 2.6.1 for the
  PlatformIO parser-test environment — MIT. This is a test-only dependency and
  is not part of the production firmware. Its full license text is also
  included in [`third_party_licenses/`](./third_party_licenses/).
- The resolved PlatformIO `espressif32` platform was 6.8.1. It supplied
  Arduino-ESP32 2.0.17 (`LGPL-2.1-or-later`) and ESP-IDF 4.4.7 (`Apache-2.0`).
  Full license texts are included in
  [`third_party_licenses/`](./third_party_licenses/). ESP-IDF bundles
  components with their own licenses, including FreeRTOS (MIT); consult the
  included component notices when redistributing.

## TLS root certificates

The outbound TLS trust bundle in `certs/x509_crt_bundle.bin` is generated from
the Mozilla root certificate program's CA data, distributed by the curl
project. The CA data is licensed under MPL-2.0; its full license text is
included in [`third_party_licenses/MPL-2.0.txt`](./third_party_licenses/MPL-2.0.txt).
The bundle format is generated using Espressif's ESP-IDF `gen_crt_bundle.py`
utility, licensed under Apache-2.0; the generator itself is not included in
this repository. Source date, hash, and update instructions are documented in
[certs/README.md](./certs/README.md).

## License text files

The `third_party_licenses` directory contains the full upstream license texts
and notices for the dependencies identified above, including the applicable
NimBLE nested notices. The `rapt-ble` MIT license text is reproduced above.
These notices document the versions resolved during the review; regenerate
and review this inventory if the PlatformIO platform or dependencies change.

## Distribution notes

The root [LICENSE](./LICENSE) applies to RaptGetaway's own code; it does not
replace the licenses above. This is a dependency and license-text record, not a
legal opinion or exhaustive inventory of every object in a firmware binary.
The framework also contains precompiled Espressif components and toolchain
runtime libraries whose exact redistribution terms must be checked against the
specific firmware release. Before distributing firmware binaries, review
those terms and the LGPL relinking/source obligations, and include all
applicable notices with the release.
