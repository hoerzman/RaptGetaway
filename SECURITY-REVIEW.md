# Security review and TARA summary

**Assessment date:** 2026-10-29  
**Scope:** Repository source, configuration, README, third-party notices, and tracked tests. This is a source-based threat assessment, not a device penetration test, external scan, or CRA conformity assessment.

## System context

The gateway receives Pill telemetry over BLE, provides a web-based management interface over Wi-Fi, stores Wi-Fi and Pill configuration, and forwards measurements to Brewfather over HTTPS.

### Assets

- Wi-Fi credentials and Pill configuration.
- Brewfather stream URLs, which contain private logging identifiers.
- Integrity and availability of forwarded telemetry.
- Firmware and configuration integrity.

### Trust boundaries and assumptions

- BLE advertisements are received from nearby devices and are not authenticated in the inspected code.
- The HTTP setup interface is restricted to clients on the setup access point. Home-network administration is intended to use HTTPS and a password-protected session.
- Physical access, production provisioning, and device-level flash protections were not assessed.
- Findings describe risks evidenced by the repository; exploitability and product impact should be confirmed against deployed hardware and intended use.

## Findings

### 1. Management-plane authorization and transport

- **Severity:** Medium
- **Confidence:** 9/10
- **Evidence:** `src/main.cpp` request routing, authentication, and HTTPS setup; `src/web_security.cpp` session/password support; `data/index.html` login flow.
- **Threat scenario:** A reachable client may attempt unauthorized configuration changes or intercept credentials if management is exposed over plain HTTP.
- **Impact:** Unauthorized changes to gateway configuration or telemetry forwarding, and exposure of credentials entered during management.
- **Status:** The home-LAN HTTP management routes are no longer served; they redirect to HTTPS. The HTTPS API requires a password-protected, in-memory session, mutation requests require a same-origin `Origin` header, and the initial password setup is only allowed from a setup-AP client. Login attempts are temporarily throttled. The device generates and retains a unique self-signed TLS identity, and prints its SHA-256 fingerprint for manual verification. Saved Brewfather URLs remain hidden from API reads.
- **Residual limitations:** The self-signed certificate is not automatically trusted by clients and relies on users comparing its fingerprint out of band. Initial setup uses HTTP over the WPA2 setup AP. NVS contents are not encrypted by this application, so physical flash extraction may expose the Wi-Fi password, Pill URLs, password verifier, and TLS private key. Several end-to-end flows have been exercised on one test device, but this is not a complete supported-browser/device validation.
- **First actions:** Complete the browser and device matrix, including captive portal flow, certificate persistence, session expiry, and all routes. Consider flash encryption and secure boot for production provisioning. Treat network segmentation as supplementary, not a replacement for authorization.

### 2. Outbound TLS server verification

- **Severity:** Medium
- **Confidence:** 9/10
- **Evidence:** `sendToBrewfather()` in `src/main.cpp`; `WiFiClientSecure` with `setInsecure()` was previously used.
- **Threat scenario:** An attacker able to intercept or redirect the gateway's outbound connection can impersonate Brewfather because the TLS peer certificate is not verified.
- **Impact:** Exposure of the private stream URL and modification or interception of submitted measurements.
- **Status:** Outbound requests use the embedded Mozilla CA bundle and SNTP-synchronized system time for certificate-chain and hostname validation. Requests are blocked until the clock is synchronized; verification failures do not fall back to insecure TLS. Firmware and filesystem were flashed to the test device, and the user confirmed that a test send reached Brewfather. No negative-case TLS tests were performed.
- **First actions:** Maintain the embedded Mozilla bundle during firmware releases; source and generation details are in `certs/README.md`.

### 3. Sensitive settings stored in NVS without application-level at-rest protection

- **Severity:** Medium
- **Confidence:** 8/10
- **Evidence:** `src/main.cpp` stores Wi-Fi and Pill configuration through `Preferences`; `src/web_security.cpp` stores the TLS private key and salted password verifier through `Preferences`.
- **Threat scenario:** An attacker with physical access and the ability to read the device's flash may recover stored Wi-Fi credentials, Brewfather stream URLs, the private TLS key, and the password verifier.
- **Impact:** Access to the local network or Brewfather stream, and possible impersonation of the gateway certificate. The salted verifier raises the cost of offline password guessing but does not protect the other stored secrets.
- **Treatment:** Documented in the README. Enable and provision ESP32 flash encryption (and assess secure boot and the device provisioning/update process) for products whose physical-access threat model requires confidentiality of stored secrets.

### 4. BLE telemetry source is not authenticated

- **Severity:** Medium
- **Confidence:** 8/10
- **Evidence:** BLE scanning and address matching in `src/main.cpp`, and payload decoding in `include/rapt_parser.h`; the [published RAPT Pill BLE format](https://gitlab.com/rapt.io/public/-/wikis/Pill%20Hydrometer%20Bluetooth%20Transmissions) describes v1/v2 measurement fields without a signature, message-authentication code, or freshness counter.
- **Threat scenario:** A nearby transmitter that can impersonate a configured Pill's advertised address and provide a parseable payload may cause the gateway to accept a false sample.
- **Impact:** Measurement integrity can be compromised before data is forwarded to Brewfather. This scenario requires radio proximity and successful device-address impersonation.
- **Assessment:** The v1 payload includes a MAC address used for registration, and v2 derives the registration address from the BLE address. These are identifiers, not cryptographic proof of origin. The inspected gateway passively scans advertisements and does not pair with or connect to a Pill. The published payload format does not define an authentication mechanism, so no gateway-only change can establish the sender's authenticity.
- **Treatment:** Record this as an open residual risk for the current protocol. Ask RAPT whether supported firmware offers authenticated telemetry or plans to add it. Until then, treat measurements as untrusted; any range, continuity, or anomaly checks can help detect implausible readings but cannot authenticate them. Accept the remaining risk only after assessing the consequence of falsified measurements for the intended use.

## Recommended order of work

1. **Validate and harden the management plane.** Authorization, same-origin mutation checks, LAN HTTPS, unique self-signed identities, and URL masking are implemented in source. Complete browser/device validation, then address flash extraction with production provisioning and flash encryption where required.
2. **Verify the Brewfather TLS peer.** This is a direct confidentiality and integrity improvement for outbound telemetry.
3. **Track the BLE authenticity limitation with RAPT.** Confirm with the vendor/maintainer whether authenticated telemetry exists or is planned. The currently documented format has no gateway-only authenticity fix; keep the residual risk open, assess the impact of falsified measurements, and record a justified risk-acceptance decision if no authenticated protocol becomes available.
4. **Establish CRA-oriented vulnerability handling and release evidence.** Define a vulnerability-reporting contact and process, triage and remediation workflow, security-support period, and how fixes reach users. The repository describes manual firmware/filesystem uploads but does not establish a signed update process.
5. **Improve release traceability.** Pin and record resolved PlatformIO dependency versions and produce an SBOM/provenance record for releases. `platformio.ini:16-18` lists dependencies without version constraints; this is a reproducibility and supply-chain readiness concern, not evidence of compromise.

## Follow-up verification

Confirm production provisioning and physical-device protections, including whether secure boot or flash encryption are enabled where appropriate. Add focused tests for management authorization, secret handling, and BLE telemetry parsing. Reassess severity against the deployed network model and consequences of erroneous measurements.

This assessment is a prioritized engineering input to a product risk assessment. It does not by itself establish CRA compliance or conformity.

## CRA-oriented vulnerability handling

RaptGetaway is a hobby project made by homebrewers for homebrewers. There is
currently no staffed security team or formal vulnerability-handling process,
and maintainers cannot promise continuous monitoring, response times, fixes, or
a defined security-support period. Any vulnerability handling is best effort;
users should not assume that reports will receive a timely response or that
every issue can be fixed.

This project context describes current capacity, not a CRA exemption or a claim
of compliance. If the software or gateway is placed on the EU market, the
responsible manufacturer must determine and meet the applicable CRA duties.
Before making any such compliance claim, establish an accessible reporting
channel, triage and remediation process, coordinated disclosure approach, and
security-support period. Until then, CRA-oriented vulnerability handling is an
open project gap.
