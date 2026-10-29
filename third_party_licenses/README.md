# Third-party license texts

These are copies of upstream license texts and notices for the dependency
versions recorded in [the project notices](../THIRD-PARTY-NOTICES.md). They are
provided for source and firmware redistribution; they do not replace review of
the exact files and terms for a particular release.

| File | Applies to |
|---|---|
| `Apache-2.0.txt` | ESP-IDF 4.4.7; standard Apache-2.0 license text |
| `Arduino-ESP32-LGPL-2.1.md` | Arduino-ESP32 2.0.17; upstream license text |
| `LGPL-2.1.txt` | LGPL-2.1 license text referenced by Arduino-ESP32 |
| `ESP-IDF-FreeRTOS-MIT.txt` | FreeRTOS component in ESP-IDF 4.4.7 |
| `NimBLE-Arduino-LICENSE.txt` | NimBLE-Arduino 2.5.2 |
| `NimBLE-Arduino-NOTICE.txt` | Upstream NimBLE-Arduino attribution notice |
| `NimBLE-nested-BSD-licenses.txt` | TinyCrypt (BSD-3-Clause) and micro-ecc (BSD-2-Clause) notices distributed with NimBLE-Arduino |
| `ArduinoJson-MIT.txt` | ArduinoJson 7.4.3 |
| `Unity-MIT.txt` | Unity 2.6.1, used only by the parser tests |
| `MPL-2.0.txt` | Mozilla CA root data embedded in the firmware |

The `rapt-ble` MIT license text is included in
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).

This list is not an exhaustive component-level firmware SBOM. The ESP-IDF
package includes precompiled Espressif components and the toolchain supplies
runtime libraries with separate redistribution terms. Those must be checked
for the exact binary release before distributing it.
