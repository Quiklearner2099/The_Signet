# The Signet v1.3.1 (Dev Branch) - Comprehensive Security Audit

**Date:** March 15, 2026
**Firmware Version:** 1.3.1 (Dev branch)
**Source File:** `The_Signet.ino` (2,284 lines)
**Scope:** Full firmware + embedded web UI static analysis
**Methodology:** OWASP IoT Top 10, NIST IR 8259, CWE/SANS Top 25, ESP32-specific attack surfaces
**Auditor:** Claude Code (Opus 4.6)
**Prior Audit Reference:** `SECURITY_AUDIT_v1.3.0_FRESH.md` (March 10, 2026)

---

## Executive Summary

The Signet v1.3.1 on the Dev branch includes significant OTA security improvements (rate limiting, exponential backoff, hard lockout, 4-digit codes, code expiry, and debug gating). However, **several v1.3.0 security features documented in the session memory are NOT present in the current Dev branch code**. This appears to be a branch divergence issue where the Dev branch was forked before some v1.3.0 hardening was merged.

### Risk Rating: **HIGH**

The missing v1.3.0 mitigations (no WiFi connection limit, weak IP validation, no WDT, non-fatal mutex failures) significantly impact the overall security posture despite the strong v1.3.1 OTA improvements.

### Delta Summary (v1.3.0 Audit → v1.3.1 Dev)

| Category | v1.3.0 Audit | v1.3.1 Dev | Direction |
|----------|-------------|------------|-----------|
| CRITICAL | 2 | 1 | Improved (OTA brute-force fixed) |
| HIGH | 5 | 7 | Regressed (missing v1.3.0 mitigations) |
| MEDIUM | 8 | 10 | Regressed |
| LOW | 6 | 6 | Unchanged |
| INFORMATIONAL | 4 | 5 | New items |

---

## PART 1: v1.3.1 OTA Security Fixes - Verification

### What Was Fixed (All Confirmed Present in Code)

| Fix | Location | Status | Effectiveness |
|-----|----------|--------|---------------|
| 4-digit OTA code (9^4 = 6,561 combos) | Lines 296-297 | VERIFIED | Keyspace ~9x larger than v1.3.0 |
| Exponential backoff | Lines 1844-1855 | VERIFIED | 1s, 2s, 4s, 8s, 16s delays |
| Hard lockout after 5 failures | Lines 1886-1892 | VERIFIED | Requires new code generation to clear |
| 3-minute code TTL | Lines 300, 507-513, 1867-1875 | VERIFIED | Expired codes zeroed from RAM |
| OTA_DEBUG gate | Lines 283-292 | VERIFIED | Disabled by default, no plaintext code leak |
| Mutex protection on code state | Lines 1787-1803 | VERIFIED | Code copied before mutex release |
| Rate limit reset on new code | Lines 1794-1797 | VERIFIED | Generating new code clears lockout |

### OTA Brute-Force Analysis (Post-Fix)

**Previous (v1.3.0):** 729 combinations, no rate limit → ~6 min to exhaust
**Current (v1.3.1):** 6,561 combinations + backoff + lockout

Worst-case attack with backoff:
- Attempt 1: immediate → fail → 1s wait
- Attempt 2: → fail → 2s wait
- Attempt 3: → fail → 4s wait
- Attempt 4: → fail → 8s wait
- Attempt 5: → HARD LOCKOUT (must get new code from LED)

**Effective rate:** 5 attempts per code generation. To exhaust keyspace: 6,561 / 5 = 1,313 code generations required, each requiring physical LED observation (~16s of blinks + button press). **Practically infeasible.**

**Verdict: OTA brute-force is now MITIGATED.** Downgraded from CRITICAL to LOW residual risk.

---

## PART 2: Findings — Current Dev Branch Code

### CRITICAL Findings

#### C-01: No Firmware Signature Verification
**Location:** Lines 1944-1955
**Status:** Known, intentionally deferred (documented rationale in session memory)
**CWE:** CWE-345 (Insufficient Verification of Data Authenticity)

The only firmware validation is a single magic byte check (`0xE9`). Any binary starting with this byte passes validation. No cryptographic signing, no hash verification, no downgrade prevention.

**Risk Context:** With the v1.3.1 rate limiting, an attacker needs physical LED access AND to pass the 4-digit code, making exploitation significantly harder. This is now a theoretical risk rather than a practical one for the device's threat model.

**Recommendation:** Maintain current decision. Document in user-facing security brief. The existing defense-in-depth (LED code + MAC lock + AP isolation) is sufficient for the threat model.

---

### HIGH Findings

#### H-01: No WiFi Connection Limit (max_conn not set)
**Location:** Line 2169
**Code:** `WiFi.softAP(apSsid.c_str(), AP_PASS);`
**CWE:** CWE-284 (Improper Access Control)
**Status:** REGRESSION — Session memory documents `max_conn=1` was implemented in v1.3.0, but it is NOT present in Dev branch code.

The AP allows unlimited concurrent connections. Multiple clients can connect simultaneously. While MAC locking prevents API control by secondary clients, they can:
- See the AP and connect
- Load the web UI (GET / is not MAC-locked)
- Read device state via GET /api/state (not MAC-locked)
- Observe the message text being transmitted
- Sniff WiFi traffic from the legitimate client

**Expected code:** `WiFi.softAP(apSsid.c_str(), AP_PASS, channel, 0, 1);`

**Impact:** HIGH — Multiple clients can view the device state and intercept messages.

#### H-02: checkMacLock() Fails Open
**Location:** Lines 441-443
```cpp
if (!getClientMac(clientMac)) {
    return true;  // ALLOWS access when MAC lookup fails
}
```
**CWE:** CWE-280 (Improper Handling of Insufficient Permissions)

When `getClientMac()` fails (e.g., ESP32 API returns error, race condition), the function returns `true` (allow access). This is a **fail-open** design. Should return `false` (deny access) to fail closed.

**Impact:** HIGH — An attacker could potentially exploit MAC lookup failures to bypass the lock.

#### H-03: No Security Headers on Web Responses
**Location:** Lines 1603-1605 (sendIndex), 1611-1613 (sendHelpPage), 1636 (handleState)
**CWE:** CWE-1021 (Improper Restriction of Rendered UI Layers)

No security headers are set on any response:
- No `Content-Security-Policy` — allows inline script injection if XSS found
- No `X-Frame-Options: DENY` — clickjacking possible
- No `X-Content-Type-Options: nosniff` — MIME type confusion
- No `Access-Control-Allow-Origin` restriction — any origin can make API calls

**Impact:** HIGH — Enables DNS rebinding and clickjacking attacks.

#### H-04: DNS Rebinding Vulnerability
**Location:** Lines 2178, 1553-1561
**CWE:** CWE-350 (Reliance on Reverse DNS Resolution for Security)

The DNS server responds to ALL queries with `192.168.4.1`:
```cpp
dnsServer.start(53, "*", apIP);
```

Combined with no `Origin` header validation on POST endpoints, a malicious website on a client's browser could:
1. Resolve an attacker domain to 192.168.4.1
2. Make API calls to /api/play, /api/update, /api/stop from JS
3. Control the device without user knowledge

**Impact:** HIGH — Remote control of device via browser-based attack while client is connected.

#### H-05: SSID Contains Device Fingerprint
**Location:** Lines 196-208
**CWE:** CWE-200 (Exposure of Sensitive Information)

SSID format `The_Signet_XXYYZZ` embeds last 3 MAC bytes, enabling:
- Passive device tracking via WiFi scanning (WiGLE, etc.)
- Correlation across locations and time
- Attribution to specific hardware unit

**Mitigating factor:** AP auto-shutdown after 90 seconds of idle (but was supposed to be 60s per session memory).

#### H-06: Weak isIpAddress() Validation
**Location:** Lines 1545-1551
```cpp
bool isIpAddress(String str) {
  for (size_t i=0; i<str.length(); i++) {
    char c = str[i];
    if ((c != '.') && (c < '0' || c > '9')) return false;
  }
  return true;
}
```
**CWE:** CWE-20 (Improper Input Validation)
**Status:** REGRESSION — Session memory documents a robust IPv4 parser was implemented, but Dev branch has the weak version.

This function accepts invalid "IP addresses" such as:
- `"..."` (just dots)
- `""` (empty string)
- `"1.2.3.4.5.6"` (too many octets)
- `"999.999.999.999"` (invalid octets)

This directly affects the captive portal bypass detection. Malicious Host headers that contain only digits and dots would not trigger the redirect to the captive portal.

#### H-07: Mutex Failures Are Non-Fatal
**Location:** Lines 2152-2155 (textMutex), Lines 2158-2161 (otaMutex)
```cpp
textMutex = xSemaphoreCreateMutex();
if (!textMutex) {
    Serial.println("WARNING: Failed to create text mutex");
}
```
**CWE:** CWE-391 (Unchecked Error Condition)
**Status:** REGRESSION — Session memory documents both mutexes were made fatal-on-failure, but Dev branch uses WARNING only.

If mutex creation fails, the device continues operating without thread safety. `getTextCopy()` will always return `"SOS"` fallback, and `setTextSafe()` will silently drop writes. OTA mutex failure allows concurrent upload attempts.

**Expected behavior:** `while (true) { delay(1000); }` (halt)

---

### MEDIUM Findings

#### M-01: No Watchdog Timer Configuration
**Location:** `setup()` function (Lines 2108-2228)
**Status:** REGRESSION — Session memory documents WDT was configured with 60s timeout, but no WDT code is present in Dev branch.

Without WDT, a firmware hang (infinite loop, deadlock) will leave the device unresponsive indefinitely until power cycle. Android captive portal floods could also cause issues without WDT protection.

#### M-02: No Captive Portal Detection Handlers
**Location:** `setup()` HTTP routes (Lines 2183-2220)
**Status:** REGRESSION — Session memory documents 8 OS-specific captive portal handlers were added, but none are present in Dev branch.

Missing handlers:
- `/generate_204` and `/gen_204` (Android)
- `/hotspot-detect.html` (iOS/macOS)
- `/connecttest.txt` and `/redirect` (Windows)
- `/canonical.html` and `/success.txt` (Firefox)
- `/wpad.dat` (Windows WPAD)

Without these, captive portal popups may not appear on many devices, forcing users to manually navigate to 192.168.4.1.

#### M-03: `playing` State Not Atomic
**Location:** Line 275
```cpp
volatile bool playing = false;
```
**CWE:** CWE-362 (Race Condition)
**Status:** REGRESSION — Session memory documents `std::atomic<bool>` was added, but Dev branch uses `volatile bool`.

`volatile` prevents compiler optimization but does NOT provide atomicity guarantees. `playing` is read in `morseTask()` (FreeRTOS task on potentially different core) and written from the web server (main loop). While torn reads on a `bool` are unlikely on 32-bit ESP32, this is not guaranteed by the C++ standard.

#### M-04: Custom RGB Values Not Constrained
**Location:** Lines 1665-1667
```cpp
if (doc.containsKey("customR"))   state.customR   = (uint8_t)doc["customR"].as<int>();
if (doc.containsKey("customG"))   state.customG   = (uint8_t)doc["customG"].as<int>();
if (doc.containsKey("customB"))   state.customB   = (uint8_t)doc["customB"].as<int>();
```
**Status:** REGRESSION — Session memory documents `constrain(val, 0, 255)` was added, but Dev branch casts directly.

Casting `int` to `uint8_t` truncates values > 255. While this doesn't cause a security vulnerability (uint8_t naturally wraps), it means values like 256 → 0, 257 → 1, which could confuse users or cause unexpected behavior.

#### M-05: No WiFi Channel Randomization
**Location:** Line 2169
**Status:** REGRESSION — Session memory documents random channel selection (1, 6, 11), but Dev branch uses default channel.

Fixed WiFi channel increases susceptibility to interference and makes the device more predictable.

#### M-06: GET /api/state Not MAC-Locked
**Location:** Lines 1616-1637
**CWE:** CWE-862 (Missing Authorization)

The state endpoint serves device state (message text, firmware version, language, playback status) to any connected client without MAC lock verification. Combined with H-01 (no connection limit), any nearby device can read the transmitted message.

#### M-07: innerHTML Usage in buildLangGrid()
**Location:** Embedded JS, Line 1025
```javascript
btn.innerHTML = L.name + '<span class="lang-native">' + L.native + '</span>';
```
**CWE:** CWE-79 (Cross-site Scripting)

Values are from hardcoded `LANG` object (not user input), so not currently exploitable. However, this pattern is an XSS risk if translations are ever loaded from external sources. Should use `textContent` + DOM element creation.

#### M-08: Update.end(true) Skips Size Validation
**Location:** Line 1976
```cpp
if (Update.end(true)) {
```
**CWE:** CWE-345 (Insufficient Verification of Data Authenticity)

`Update.end(true)` forces success regardless of whether all expected bytes were received. A truncated or partial firmware upload could be written to flash. Should use `Update.end(false)` to enforce size validation when Content-Length is known.

#### M-09: Content-Length Not Mandatory for OTA
**Location:** Lines 1913-1916
```cpp
size_t contentLength = 0;
if (server.hasHeader("Content-Length")) {
    contentLength = server.header("Content-Length").toInt();
}
```

If Content-Length is absent, `contentLength` defaults to 0, which passes the size check. `Update.begin()` is called with `UPDATE_SIZE_UNKNOWN`, allowing arbitrarily-sized uploads (up to partition limit).

#### M-10: AP Idle Timeout is 90 Seconds (Was 60)
**Location:** Line 211
```cpp
const unsigned long AP_IDLE_TIMEOUT_MS = 90000; // 90 seconds
```
**Status:** REGRESSION — Session memory documents this was reduced to 60 seconds for privacy, but Dev branch has 90 seconds.

Longer window increases SSID exposure for passive tracking.

---

### LOW Findings

#### L-01: Serial Debug Leaks Non-OTA Information
**Location:** Lines 2174 (MAC), 450-452 (locked MAC), 1757 (language), 2255 (security events)

While OTA debug is now gated behind `OTA_DEBUG`, other serial output still exposes:
- Full device MAC address
- Locked client MAC address
- Security events (lock, disconnect shutdown)
- Language changes

Accessible via USB physical access.

#### L-02: Text Mutex Timeout Falls Back Silently
**Location:** Lines 310-318
```cpp
if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    copy = state.text;
    xSemaphoreGive(textMutex);
} else {
    copy = "SOS";  // Fallback if mutex fails
}
```

On mutex timeout, `getTextCopy()` returns "SOS" without logging. User gets no indication of concurrency issue.

#### L-03: localStorage Fingerprinting Artifacts
**Location:** Embedded JS, Lines 858-859, 1013, 1196, 1213

Client stores `signet_lang` and `signet_splash_seen` in localStorage. These persist in the browser and can reveal that a user connected to a Signet device through browser forensics.

#### L-04: Size Mismatch Is Warning Only
**Location:** Lines 1979-1981

When received bytes don't match Content-Length, it's logged as a warning but the firmware is still flashed via `Update.end(true)`.

#### L-05: morseTask Stack Size Margin
**Location:** Line 2224
```cpp
xTaskCreate(morseTask, "morse", 4096, nullptr, 1, &morseTaskHandle);
```

4096 bytes of stack for morseTask. The task allocates a String (up to ~200 chars), calls multiple functions. Sufficient for normal operation but limited margin under worst-case conditions.

#### L-06: Division by Zero in ditDuration()
**Location:** Line 361
```cpp
uint32_t ditDuration()  { return 1200 / state.wpm; }
```
**Status:** REGRESSION — Session memory documents a guard for wpm=0 was added, but Dev branch has no guard.

If `state.wpm` is ever 0, this causes undefined behavior (division by zero). The `handleUpdate()` function clamps wpm to 5-20 (lines 1670-1673), and the initial value is 10 (line 269), so this requires either a code bug or memory corruption to trigger.

---

### INFORMATIONAL Findings

#### I-01: OTA Code TTL Is 3 Minutes, Not 5 Minutes
**Location:** Line 300
```cpp
static const unsigned long OTA_CODE_TTL_MS = 180000UL;  // 3 minutes
```

The `CLAUDE.md` and version history comment say "5-minute code expiry" but the actual TTL is 180,000ms = 3 minutes. This is a documentation inconsistency, not a security issue. The shorter TTL is actually more secure.

#### I-02: OTA Rollback Protection Present
**Location:** Line 2227
```cpp
esp_ota_mark_app_valid_cancel_rollback();
```
Called unconditionally in setup(). This means the firmware marks itself valid immediately on boot. If the firmware has a critical bug, automatic rollback won't trigger because the app already marked itself valid. Consider delaying this call until after successful network initialization.

#### I-03: Open WiFi AP (By Design)
**Location:** Line 188
```cpp
const char* AP_PASS = "";
```

All HTTP traffic is plaintext. OTA code transmitted in cleartext via `X-OTA-Code` header. Message text in POST bodies is unencrypted. This is a deliberate usability choice documented in the project.

#### I-04: No Rate Limiting on Non-OTA Endpoints
**Location:** Lines 1639-1713

The /api/update, /api/play, /api/stop, and /api/language endpoints have no rate limiting. A connected client could flood these endpoints. Mitigated by MAC locking (only locked client can use them).

#### I-05: OTA Error Response String Concatenation
**Location:** Lines 2008-2022
```cpp
String response = "{\"error\":\"";
if (otaErrorMessage.length() > 0) {
    response += otaErrorMessage;
}
```

Error messages are embedded directly into JSON without escaping. If `otaErrorMessage` ever contains `"` characters, the JSON response would be malformed. Current error messages are all hardcoded strings without quotes, so this is not exploitable now.

---

## PART 3: Branch Divergence Analysis

### Features Documented in v1.3.0 Session Memory BUT Missing from Dev Branch

| Feature | Expected Location | Dev Branch Status | Security Impact |
|---------|-------------------|-------------------|-----------------|
| `max_conn=1` on WiFi.softAP | WiFi.softAP() call | **MISSING** | HIGH — Multiple clients can connect |
| Robust `isIpAddress()` validator | Captive portal helpers | **MISSING** (weak version present) | HIGH — Captive portal bypass |
| Fatal halt on mutex failure | setup() | **MISSING** (WARNING only) | HIGH — Runs without thread safety |
| `std::atomic<bool>` for playing | AppState struct | **MISSING** (volatile bool) | MEDIUM — Potential race |
| `constrain()` on custom RGB | handleUpdate() | **MISSING** | MEDIUM — Truncation bugs |
| Randomized WiFi channel | WiFi.softAP() call | **MISSING** | MEDIUM — Predictability |
| Captive portal OS handlers (8) | HTTP routes | **MISSING** | MEDIUM — Portal not triggering |
| WDT with 60s timeout | setup() | **MISSING** | MEDIUM — Hang vulnerability |
| AP idle timeout 60s | Constant | **90s instead of 60s** | MEDIUM — Privacy window |
| Division-by-zero guard | ditDuration() | **MISSING** | LOW — Requires memory corruption |
| Pre-computed redirect URL | Global variable | **MISSING** | LOW — Heap pressure |

**Root Cause:** The Dev branch appears to have diverged from main before the v1.3.0 captive portal, stability, and privacy hardening changes were merged. The v1.3.1 OTA fixes were then applied to Dev without first rebasing onto main.

**Recommendation:** Merge or rebase Dev onto main to incorporate all v1.3.0 mitigations before release.

---

## PART 4: Threat Model (Current Dev Branch)

| Attack Vector | Risk Level | Barriers | Notes |
|---------------|-----------|----------|-------|
| **Remote attack** | None | AP-only, no internet | Not applicable |
| **OTA firmware tampering** | LOW | 4-digit LED code + backoff + lockout + MAC lock | v1.3.1 fixes are effective |
| **Message interception** | HIGH | Open WiFi, no encryption | By design — messages are plaintext |
| **Device tracking** | HIGH | SSID contains MAC suffix | 90s window (was supposed to be 60s) |
| **DNS rebinding** | HIGH | No Origin validation, no security headers | Browser-based remote control |
| **Concurrent user access** | HIGH | No connection limit in Dev branch | Anyone nearby can view state |
| **Physical USB access** | ACCEPTED | Full serial access, OTA debug gated | Threat model accepts this |
| **Flash forensics** | None | Zero bytes persisted | Strong anti-forensics |
| **Captive portal issues** | MEDIUM | No OS-specific handlers | Users must manually navigate |
| **Device hang/deadlock** | MEDIUM | No WDT in Dev branch | Requires power cycle |

---

## PART 5: Compliance Assessment

| Standard | Status | Gaps |
|----------|--------|------|
| **OWASP IoT Top 10** | | |
| I1: Insecure Web Interface | PARTIAL | No security headers, no HTTPS, innerHTML usage |
| I2: Insecure Auth | PARTIAL | MAC lock fail-open, no connection limit in Dev |
| I3: Insecure Ecosystem | N/A | Standalone device, no cloud |
| I4: Insecure Update | IMPROVED | OTA code + backoff + lockout; no signing |
| I5: Insecure Privacy | PARTIAL | SSID tracking, open WiFi; good flash hygiene |
| **NIST IR 8259** | | |
| Logical Access | PARTIAL | MAC lock + OTA code present; fail-open weakness |
| Software Update Integrity | PARTIAL | Magic byte only; no signature verification |
| Cybersecurity State Awareness | WEAK | Serial debug only; no WDT in Dev |
| **CWE/SANS Top 25** | | |
| CWE-345 (Data Authenticity) | OPEN | No firmware signing |
| CWE-306 (Missing Auth) | MITIGATED | LED code + MAC lock (with fail-open caveat) |
| CWE-307 (Auth Brute Force) | MITIGATED | Exponential backoff + hard lockout |
| CWE-284 (Access Control) | OPEN | No connection limit, state endpoint unprotected |

---

## PART 6: Positive Security Findings

The firmware demonstrates several strong practices:

1. **True stateless design** — Zero bytes persisted to flash (no NVS, no EEPROM)
2. **OTA combination lock with rate limiting** — LED code + exponential backoff + hard lockout (v1.3.1)
3. **OTA debug gating** — `OTA_DEBUG` preprocessor flag prevents code leakage (v1.3.1)
4. **Code TTL expiry** — 3-minute window limits attack opportunity (v1.3.1)
5. **Mutex-protected text access** — `getTextCopy()`/`setTextSafe()` with fallback
6. **OTA mutex** — Prevents concurrent firmware uploads
7. **MAC address locking** — First POST client gets exclusive API control
8. **Disconnect-triggered AP shutdown** — AP shuts down when locked client leaves (Lines 2244-2264)
9. **Locked MAC cleared from RAM** — Defense against memory forensics (Line 2262)
10. **Payload size limits** — 512/256 byte caps on JSON endpoints
11. **200-char message cap** — Prevents excessive memory usage
12. **OTA max size check** — Rejects firmware exceeding partition size
13. **Firmware magic byte validation** — Rejects non-ESP32 binaries
14. **OTA rollback protection** — `esp_ota_mark_app_valid_cancel_rollback()`
15. **Safe DOM manipulation** — `textContent` used throughout (except one `innerHTML`)
16. **No eval/Function constructors** — Client-side JS avoids code injection
17. **Cache-Control no-cache** — Prevents UI caching in browser

---

## PART 7: Actionable Security Checklist

### Priority 1: Merge v1.3.0 Mitigations into Dev Branch
These are already implemented on main and just need to be merged:

- [ ] **HIGH:** Add `max_conn=1` to `WiFi.softAP()` call (line 2169)
- [ ] **HIGH:** Replace weak `isIpAddress()` with robust IPv4 parser (lines 1545-1551)
- [ ] **HIGH:** Make textMutex and otaMutex failures FATAL (halt device) (lines 2152-2161)
- [ ] **MEDIUM:** Change `playing` from `volatile bool` to `std::atomic<bool>` (line 275)
- [ ] **MEDIUM:** Add `constrain()` on customR/customG/customB (lines 1665-1667)
- [ ] **MEDIUM:** Add randomized WiFi channel selection (line 2169)
- [ ] **MEDIUM:** Add captive portal OS detection handlers (8 endpoints)
- [ ] **MEDIUM:** Configure WDT with 60s timeout in setup()
- [ ] **MEDIUM:** Reduce AP_IDLE_TIMEOUT_MS from 90000 to 60000 (line 211)
- [ ] **LOW:** Add division-by-zero guard to `ditDuration()` (line 361)

### Priority 2: New Fixes (Not Yet Implemented Anywhere)

- [ ] **HIGH:** Fix `checkMacLock()` fail-open — change `return true` to `return false` at line 443
- [ ] **HIGH:** Add basic security headers to `sendIndex()` and `sendHelpPage()`:
  ```
  X-Frame-Options: DENY
  X-Content-Type-Options: nosniff
  Content-Security-Policy: default-src 'self' 'unsafe-inline'; img-src 'self' data:
  ```
- [ ] **HIGH:** Add `Origin` header validation on all POST endpoints (DNS rebinding defense)
- [ ] **MEDIUM:** Add MAC lock check to `handleState()` (GET /api/state)
- [ ] **MEDIUM:** Use `textContent` instead of `innerHTML` in `buildLangGrid()` (JS line 1025)
- [ ] **MEDIUM:** Use `Update.end(false)` instead of `Update.end(true)` (line 1976)
- [ ] **MEDIUM:** Make Content-Length mandatory for OTA uploads
- [ ] **LOW:** Escape JSON strings in OTA error response (lines 2008-2022)
- [ ] **LOW:** Log mutex timeout failures (don't silently fall back)
- [ ] **LOW:** Consider delaying `esp_ota_mark_app_valid_cancel_rollback()` until after network init

### Priority 3: Documentation Fixes

- [x] **INFO:** Fix CLAUDE.md — OTA code TTL says "5-minute" but code is 3 minutes (FIXED)
- [x] **INFO:** Fix version history comment — same TTL discrepancy (line 48) (FIXED)
- [ ] **INFO:** Document open WiFi as intentional design tradeoff in user-facing docs
- [ ] **INFO:** Document localStorage artifacts in privacy section

---

## PART 8: Testing Recommendations

### OTA Security Tests (v1.3.1 Fixes)
- [ ] Generate code → verify 4-color LED blinks (R, G, B, Y)
- [ ] Enter correct code → successful firmware upload
- [ ] Enter wrong code → error with retry timer shown
- [ ] Enter wrong code 5x → "Locked out" message, upload disabled
- [ ] Generate new code after lockout → lockout clears, new code works
- [ ] Generate code, wait >3 min → "Code expired" error
- [ ] Build WITHOUT `#define OTA_DEBUG` → no OTA codes in serial output
- [ ] Build WITH `#define OTA_DEBUG` → OTA codes visible in serial

### Regression Tests (After Merge)
- [ ] Only 1 device can connect to AP simultaneously
- [ ] Captive portal popup appears on Android
- [ ] Captive portal popup appears on iOS
- [ ] Captive portal popup appears on Windows 10/11
- [ ] Device reboots within 60s if firmware hangs (WDT)
- [ ] Custom color values properly constrained to 0-255
- [ ] WiFi channel varies between reboots (1, 6, or 11)
- [ ] AP shuts down after 60s of inactivity (not 90s)

### Penetration Test Scenarios
- [ ] Connect 2 phones simultaneously → second should be rejected
- [ ] Send malformed Host header → should redirect to captive portal
- [ ] Attempt DNS rebinding attack → should be blocked by Origin check
- [ ] Send 1000 rapid requests to /api/update → should not crash device
- [ ] Upload firmware without Content-Length → should be rejected
- [ ] Upload truncated firmware → should fail validation

---

*Report generated: March 15, 2026*
*Branch: Dev*
*Classification: For project maintainer review*
*Next audit recommended: After merge of v1.3.0 mitigations into Dev branch*
