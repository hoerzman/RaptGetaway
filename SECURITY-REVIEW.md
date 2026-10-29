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
- The HTTP management interface is available on the home Wi-Fi interface as well as during setup.
- Physical access, production provisioning, and device-level flash protections were not assessed.
- Findings describe risks evidenced by the repository; exploitability and product impact should be confirmed against deployed hardware and intended use.

## Findings

### 1. Unauthenticated management API and exposed stream URLs

- **Severity:** Medium
- **Confidence:** 9/10
- **Evidence:** `src/main.cpp:358-395, 398-431, 647-665`
- **Threat scenario:** A client on the gateway's LAN can access management endpoints without application-level authorization. It can read configured Brewfather URLs and make configuration changes, delete settings, or trigger test sends. Because the interface uses plain HTTP, an on-path LAN observer may also read management traffic and secret-bearing URLs.
- **Impact:** Disclosure of private stream identifiers and unauthorized changes to gateway configuration or telemetry forwarding.
- **First actions:** Require per-device authorization for management operations; do not return full secret-bearing URLs; add CSRF defenses; protect management traffic against LAN interception. Treat network segmentation as a supplementary control, not a replacement for authorization.

### 2. Outbound TLS certificate verification disabled

- **Severity:** Medium
- **Confidence:** 9/10
- **Evidence:** `src/main.cpp:227-232`
- **Threat scenario:** An attacker able to intercept or redirect the gateway's outbound connection can impersonate Brewfather because the TLS peer certificate is not verified.
- **Impact:** Exposure of the private stream URL and modification or interception of submitted measurements.
- **First actions:** Enable certificate verification with a maintained trust anchor or an appropriately managed pinning strategy. Account for device time and trust-anchor maintenance. The README also calls out this limitation.

### 3. BLE telemetry source is not authenticated

- **Severity:** Medium
- **Confidence:** 8/10
- **Evidence:** `src/main.cpp:684-715, 728-760`
- **Threat scenario:** A nearby transmitter that can impersonate a configured Pill's advertised address and provide a parseable payload may cause the gateway to accept a false sample.
- **Impact:** Measurement integrity can be compromised before data is forwarded to Brewfather. This scenario requires radio proximity and successful device-address impersonation.
- **First actions:** Use authenticated telemetry if supported by the Pill protocol. If it is unavailable, document spoofability as a residual risk and do not treat received readings as authenticated evidence.

## Recommended order of work

1. **Secure the management plane and secrets.** Add authorization and CSRF protection, minimize secret exposure in API responses, and protect administrative traffic in transit.
2. **Verify the Brewfather TLS peer.** This is a direct confidentiality and integrity improvement for outbound telemetry.
3. **Decide on BLE authenticity requirements.** Assess whether spoofed measurements are acceptable for the intended purpose; implement protocol-supported authentication or formally record the residual risk.
4. **Establish CRA-oriented vulnerability handling and release evidence.** Define a vulnerability-reporting contact and process, triage and remediation workflow, security-support period, and how fixes reach users. The repository describes manual firmware/filesystem uploads but does not establish a signed update process.
5. **Improve release traceability.** Pin and record resolved PlatformIO dependency versions and produce an SBOM/provenance record for releases. `platformio.ini:16-18` lists dependencies without version constraints; this is a reproducibility and supply-chain readiness concern, not evidence of compromise.

## Follow-up verification

Confirm production provisioning and physical-device protections, including whether secure boot or flash encryption are enabled where appropriate. Add focused tests for management authorization, secret handling, TLS verification configuration, and acceptance/rejection behavior for BLE telemetry. Reassess severity against the deployed network model and consequences of erroneous measurements.

This assessment is a prioritized engineering input to a product risk assessment. It does not by itself establish CRA compliance or conformity.
