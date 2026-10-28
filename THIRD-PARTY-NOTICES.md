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

## PlatformIO dependencies

- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) by h2zero —
  Apache-2.0.
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) by Benoit Blanchon —
  MIT.
- The ESP32 Arduino framework and its bundled libraries provide Wi-Fi, web
  server, DNS, HTTP client, Preferences, and LittleFS functionality. Their
  licenses are distributed with the PlatformIO framework packages.
