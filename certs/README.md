# Outbound TLS trust bundle

`x509_crt_bundle.bin` contains 118 Mozilla-trusted root certificates. It was
generated on 2026-10-29 from the curl CA extract, whose embedded source date is
2026-10-20.

- Source: <https://curl.se/ca/cacert-2026-10-20.pem>
- Source SHA-256: `796F1389BFEA13C0ED78F663B765BAAFF54A508E5483786BD6ECE73C5E3E3CBE`
- Source license: Mozilla Public License 2.0 (MPL-2.0), as stated by the
  [curl CA extract documentation](https://curl.se/docs/caextract.html).
- Generator: Espressif ESP-IDF v4.4.7 `gen_crt_bundle.py`
- Generator source:
  <https://github.com/espressif/esp-idf/blob/v4.4.7/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py>

To update the bundle, download a fresh `cacert.pem` from the source above and
run Espressif's generator with that file as input from this directory. The
generator writes `x509_crt_bundle`; rename the result to
`x509_crt_bundle.bin`. Record the source date, SHA-256, and certificate count
here, then build and test the firmware. The bundle is embedded in the firmware,
not served from LittleFS.
