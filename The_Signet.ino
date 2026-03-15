/*
  ===================================================================================
  The Signet Morse Beacon
  Version: 1.3.1
  Release Date: March 15, 2026
  ===================================================================================

  DESCRIPTION:

  A covert Morse code beacon that can transmit messages via IR (invisible) or RGB 
  (visible) LED. Features a captive portal web interface for configuration and custom
  message input. NO LOGS, NO TELEMETRY, NO BULLSHYTE.

  FEATURES:
  - Wi-Fi AP + Captive Portal Web UI -No APP required
  - SSID: "The_Signet_XXXXXX" (XXXXXX = Last 3 bytes of MAC Address)
  - RGB & IR LED
  - Morse Code Playback with configurable intensity
  - Adjustable Morse speed (5-20 WPM)
  - 60-second idle WiFi AP timeout to conserve power
  - Help(?) screen with Morse code definitions
  - OTA Firmware update support with LED combination lock (v1.3.0+)

  HARDWARE:
  - Seeed Studios XIAO ESP32-C6 Module (other ESP32s can be used)
  - WS2812B RGB LED
  - IR LED
  - 10 Ohm resistor
  - 270 Ohm resistor
  - 47k Ohm resistor
  - 100k Ohm resistor
  - 2N3904 NPN Transistor
  - SPST switch for sleep control
  - (optional) 503035 LiPo battery with built in charge/discharge protection 

  - See schematic for complete circuit

  AUTHOR:
  Created by Mike Stewart as a tool for all those fighting against Authoritarianism
  and to maintain free-speech.

  ===================================================================================
  VERSION HISTORY:
  ===================================================================================
  v1.3.1 (March 15, 2026) - Security hardening: OTA combination lock - 4-digit code
                              displayed via RGB LED (Red/Green/Blue/Yellow blinks) required
                              before firmware upload. Rate limiting with exponential backoff,
                              hard lockout after 5 failed attempts, 3-minute code expiry.
                              OTA serial debug gated behind OTA_DEBUG preprocessor flag.
  v1.3.0 (March 3, 2026) - Security hardening: OTA combination lock - 3-digit code
                              displayed via RGB LED (Red/Green/Blue blinks) required
                              before firmware upload. MAC address locking - first client
                              to connect gets exclusive API access until power cycle.
                              AP now shuts down when locked client disconnects (prevents
                              secondary device WiFi access). Locked MAC cleared from RAM
                              after AP shutdown (anti-forensics). True stateless design:
                              removed NVS language persistence, language modal shows on
                              every boot. Zero bytes persisted to flash.                            
  v1.2.1 (February 26, 2026) - Added UI language options for UI. English (default), French,
                              Spanish, Russian, Traditional Chinese, Simplified Chinese, Arabic and Farsi.
                              *** ITU Morse code is still implemented ***
  v1.2.0 (February 20, 2026) - OTA firmware updates via web UI. Settings gear icon added
                              to footer, opens firmware upload modal with password
                              protection. Dual-partition layout enables automatic rollback
                              on failed updates. Requires one-time USB flash from v1.1.x.
  v1.1.2 (February 19, 2026) - Bugfix: TX time display now updates correctly when message
                              speed (WPM) is modified (Issue #24). Custom message no longer
                              reverts to default when selecting options before Play (Issue #25).
                              Fixed race condition where blue pulse and playback could both
                              control LED after quick power cycle (Issue #26). API now returns
                              proper error responses for invalid requests (Issue #27).
  v1.1.1 (January 27, 2026) - UI improvements: larger help icon and 1984 footer text,
                              firmware version indicator, splash screen text and
                              improved load reliability, Harlow font title, yellow
                              message hints, white card titles, iOS auto-zoom fix,
                              IR PWM frequency increased from 200 Hz to 30 kHz
  v1.1.0 (January 21, 2026) - Default message "S O S" changed to "SOS" for standard
                              Morse timing
  v1.0.3 (January 19, 2026) - Blue pulsing LED indicates WiFi AP ready, 4 rapid blinks
                              confirm Web UI connection, custom color wheel picker,
                              hardware PWM for IR LED (improved power efficiency)
  v1.0.2 (January 3, 2026) - Compact mobile-friendly UI
  v1.0.1 (January 2, 2026) - Added adjustable Morse speed (WPM)
  v1.0.0 (January 1, 2026) - Initial Stable Release

  ===================================================================================
  PIN CONFIGURATION: for Seeed Studios ESP32-C6
  ===================================================================================
  PIN_RGB      D5    - WS2812B RGB LED data
  PIN_IR       D8    - IR LED PWM drive
  PIN_SLEEP_SW D2    - Sleep switch (SPST to GND)

  ===================================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "esp_sleep.h"
#include "esp_mac.h"   // esp_read_mac()
#include "driver/ledc.h"  // Hardware PWM for IR LED
#include <Update.h>       // OTA firmware updates
#include "esp_ota_ops.h"  // OTA boot validation
// Preferences.h removed - true stateless design (no NVS usage)
#include <atomic>         // Thread-safe atomic types
#include "esp_task_wdt.h" // Task watchdog timer
#include "esp_wifi.h"     // MAC address locking

// Increase main loop task stack size from 8KB to 32KB
// Required for WebServer handling multiple captive portal requests (especially Android)
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// -------------------- Version Information --------------------
#define FIRMWARE_VERSION "1.3.1"
#define FIRMWARE_DATE    "March 15, 2026"

// -------------------- Forward Declarations --------------------
enum Mode     { DISCREET = 0, VISIBLE = 1 };
enum ColorSel { C_RED = 0, C_GREEN = 1, C_BLUE = 2, C_CUSTOM = 3 };
enum Intensity{ I_LOW = 0, I_MED = 1, I_HIGH = 2 };

// Language codes for UI localization
enum Language : uint8_t {
  LANG_EN = 0,     // English (default)
  LANG_ES = 1,     // Spanish (Español)
  LANG_FR = 2,     // French (Français)
  LANG_RU = 3,     // Russian (Русский)
  LANG_ZH_CN = 4,  // Simplified Chinese (简体中文)
  LANG_ZH_TW = 5,  // Traditional Chinese (繁體中文)
  LANG_FA = 6,     // Farsi/Persian (فارسی)
  LANG_AR = 7      // Arabic (العربية)
};

// Language code strings for API
const char* const LANG_CODES[] = {"en", "es", "fr", "ru", "zh-CN", "zh-TW", "fa", "ar"};
const uint8_t LANG_COUNT = 8;

uint8_t  rgbBrightnessFor(Intensity i);
uint8_t  irDutyFor(Intensity i);
CRGB     colorValue(ColorSel c);
String   intensityToStr(Intensity i);
Intensity strToIntensity(const String& s);
String   colorToStr(ColorSel c);
ColorSel strToColor(const String& s);

void setupIrHardwarePwm();
void morseTask(void* pv);

void sendIndex();
void sendHelpPage();
void handleState();
void handleUpdate();
void handlePlay();
void handleStop();
void handleOtaUpload();
void handleOtaComplete();
void handleOtaCode();
void handleLanguageSet();
void handleNotFound();

bool  captivePortal();
void  updateBluePulse();
void  connectionConfirmBlink();
void  goToDeepSleep();

// -------------------- Pins & Hardware --------------------
// Adjust these to match your wiring.
#define PIN_RGB      D5
#define PIN_IR       D8
#define NUM_PIXELS   1

// Sleep switch: SPST between D2 and GND
// CLOSED  -> D2 = LOW  -> run normally
// OPEN    -> D2 = HIGH (INPUT_PULLUP) -> deep sleep
#define PIN_SLEEP_SW D2

// -------------------- Hardware PWM (LEDC) for IR LED --------------------
#define IR_LEDC_CHANNEL  LEDC_CHANNEL_0
#define IR_LEDC_TIMER    LEDC_TIMER_0
#define IR_LEDC_FREQ_HZ  30000  // 30 kHz PWM frequency

// -------------------- WiFi AP -------------------------
const char* AP_SSID_BASE = "The_Signet";
const char* AP_PASS      = "";

IPAddress apIP(192,168,4,1);
IPAddress apGateway(192,168,4,1);
IPAddress apSubnet(255,255,255,0);

String apSsid;  // must persist (global) so c_str() stays valid
String portalRedirectUrl;  // Pre-computed redirect URL to reduce heap pressure in handlers

static String buildApSsidWithMacTail() {
  uint8_t mac[6] = {0};
  // Read factory-programmed MAC from eFuse
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  char tail[7]; // 6 hex chars + null
  snprintf(tail, sizeof(tail), "%02X%02X%02X", mac[3], mac[4], mac[5]);

  String base(AP_SSID_BASE);
  while (base.endsWith("_")) base.remove(base.length() - 1);

  return base + "_" + tail;
}

// -------------------- WiFi Idle timeout logic --------------------
const unsigned long AP_IDLE_TIMEOUT_MS = 60000; // 60 seconds (privacy: minimize SSID exposure)
unsigned long lastActivityMs = 0;
bool apRunning = false;

inline void noteActivity() { lastActivityMs = millis(); }

// -------------------- UI Connection State & Blue Pulse --------------------
volatile bool uiServed = false;              // True once Web UI has been served
static uint8_t pulseState = 10;              // Current brightness (10-90)
static int8_t pulseDirection = 5;            // Direction and step size
static unsigned long lastPulseMs = 0;
const unsigned long PULSE_INTERVAL_MS = 30;  // Update every 30ms for smooth breathing

// -------------------- Servers -----------------------------
WebServer server(80);
DNSServer dnsServer;

// Language selection: Stateless design - modal shows every boot, no NVS persistence

// -------------------- RGB via FastLED ---------------------
CRGB leds[NUM_PIXELS];

// -------------------- Hardware PWM (LEDC) for IR -----------------
void setupIrHardwarePwm() {
  ledc_timer_config_t timer_conf = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_8_BIT,
    .timer_num = IR_LEDC_TIMER,
    .freq_hz = IR_LEDC_FREQ_HZ,
    .clk_cfg = LEDC_AUTO_CLK
  };
  ledc_timer_config(&timer_conf);

  ledc_channel_config_t channel_conf = {
    .gpio_num = PIN_IR,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = IR_LEDC_CHANNEL,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = IR_LEDC_TIMER,
    .duty = 0,
    .hpoint = 0
  };
  ledc_channel_config(&channel_conf);
}

inline void irSetDuty(uint8_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, IR_LEDC_CHANNEL, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, IR_LEDC_CHANNEL);
}

// -------------------- App State --------------------------
static SemaphoreHandle_t textMutex = nullptr;

struct AppState {
  volatile Mode mode = DISCREET;
  volatile ColorSel color = C_RED;
  volatile Intensity intensity = I_MED;
  volatile bool dazzle = false;
  volatile uint8_t wpm = 10;  // Words per minute (5-20 range)
  volatile uint8_t customR = 255;  // Custom color RGB components
  volatile uint8_t customG = 0;
  volatile uint8_t customB = 255;  // Default: magenta
  volatile uint8_t language = LANG_EN;  // UI language
  String text = "SOS";
  std::atomic<bool> playing{false};  // Thread-safe playback state
} state;

// -------------------- Security: MAC Address Locking --------------------
// First client to make a POST request gets exclusive access until power cycle
static uint8_t lockedMac[6] = {0};
static bool macLocked = false;

// -------------------- Security: OTA Debug Gating --------------------
// Uncomment to enable OTA serial debug output (SECURITY: code printed in plaintext!)
// #define OTA_DEBUG
#ifdef OTA_DEBUG
  #define OTA_LOG(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
  #define OTA_LOGLN(msg)    Serial.println(msg)
#else
  #define OTA_LOG(fmt, ...)
  #define OTA_LOGLN(msg)
#endif

// -------------------- Security: OTA Combination Lock --------------------
// 4-digit code (1-9 each) displayed via RGB LED blinks (R, G, B, Y)
static const uint8_t OTA_CODE_LEN = 4;
static uint8_t otaCode[OTA_CODE_LEN] = {0};
static bool otaCodeValid = false;
static unsigned long otaCodeGeneratedAt = 0;
static const unsigned long OTA_CODE_TTL_MS = 180000UL;  // 3 minutes

// Rate limiting / lockout
static uint8_t otaFailCount = 0;
static unsigned long otaLastFailTime = 0;
static bool otaHardLocked = false;
static unsigned long otaRetryAfterMs = 0;
static const uint8_t OTA_MAX_ATTEMPTS = 5;
static const unsigned long OTA_BACKOFF_BASE_MS = 1000UL;

String getTextCopy() {
  String copy;
  if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    copy = state.text;
    xSemaphoreGive(textMutex);
  } else {
    copy = "SOS";  // Fallback if mutex fails
  }
  return copy;
}

void setTextSafe(const String& newText) {
  if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    state.text = newText;
    xSemaphoreGive(textMutex);
  }
}

// -------------------- Brightness / Color -----------------
uint8_t rgbBrightnessFor(Intensity i) {
  switch (i) {
    case I_LOW:  return 40;
    case I_MED:  return 120;
    case I_HIGH: return 200;
    default:     return 120;
  }
}

uint8_t irDutyFor(Intensity i) {
  switch (i) {
    case I_LOW:  return 40;
    case I_MED:  return 120;
    case I_HIGH: return 200;
    default:     return 128;
  }
}

CRGB colorValue(ColorSel c) {
  switch (c) {
    case C_RED:    return CRGB(128,0,0);
    case C_GREEN:  return CRGB(0,128,0);
    case C_BLUE:   return CRGB(0,0,128);
    case C_CUSTOM: return CRGB(state.customR, state.customG, state.customB);
    default:       return CRGB(128,0,0);
  }
}

// -------------------- Morse Timing (WPM-based) -------------------
// Standard "PARIS" timing: dit duration = 1200 / WPM (in ms)
// All other timings are multiples of the dit duration

uint32_t ditDuration()  { uint8_t wpm = state.wpm; return 1200 / (wpm > 0 ? wpm : 1); }  // Guard against div-by-zero
uint32_t dahDuration()  { return 3 * ditDuration(); }
uint32_t gapIntra()     { return ditDuration(); }         // Between dits/dahs in same letter
uint32_t gapLetter()    { return 3 * ditDuration(); }     // Between letters
uint32_t gapWord()      { return 7 * ditDuration(); }     // Between words

// Fixed loop gap timings (not WPM-dependent)
const uint32_t LOOP_GAP = 2000;
const uint32_t VISIBLE_DAZZLE_OFF1 = 750;
const uint32_t VISIBLE_DAZZLE_OFF2 = 750;
const uint32_t VISIBLE_DAZZLE_FLASH = 500;

struct MorseEntry { char ch; const char* code; };
const MorseEntry MORSE[] = {
  {'A',".-"},{'B',"-..."},{'C',"-.-."},{'D',"-.."},{'E',"."},{'F',"..-."},{'G',"--."},{'H',"...."},{'I',".."},{'J',".---"},
  {'K',"-.-"},{'L',".-.."},{'M',"--"},{'N',"-."},{'O',"---"},{'P',".--."},{'Q',"--.-"},{'R',".-."},{'S',"..."},
  {'T',"-"},{'U',"..-"},{'V',"...-"},{'W',".--"},{'X',"-..-"},{'Y',"-.--"},{'Z',"--.."},
  {'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},{'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
  {'.',".-.-.-"},{',',"--..--"},{'?',"..--.."},{'/',"-..-."},{'-',"-....-"},{'(',"-.--."},{')',"-.--.-"},{'@',".--.-."},{'=',"-...-"}
};
const size_t MORSE_LEN = sizeof(MORSE) / sizeof(MorseEntry);

const char* lookupMorse(char c) {
  if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  for (size_t i=0; i<MORSE_LEN; i++) if (MORSE[i].ch == c) return MORSE[i].code;
  return nullptr;
}

// -------------------- Prosigns ---------------------------
struct ProsignEntry { const char* tag; const char* code; };
const ProsignEntry PROSIGNS[] = {
  {"KA", "-.-.-"},   // Starting signal
  {"AR", ".-.-."}    // End of message
};
const size_t PROSIGN_LEN = sizeof(PROSIGNS) / sizeof(ProsignEntry);

const char* lookupProsign(const String& tag) {
  for (size_t i = 0; i < PROSIGN_LEN; i++) {
    if (tag.equalsIgnoreCase(PROSIGNS[i].tag)) return PROSIGNS[i].code;
  }
  return nullptr;
}

// -------------------- LED Control -------------------------
inline void irOff() { irSetDuty(0); }
inline void irOn()  { irSetDuty(irDutyFor(state.intensity)); }

void rgbOff() {
  FastLED.setBrightness(0);
  leds[0] = CRGB::Black;
  FastLED.show();
}

void rgbOnCurrentColor() {
  FastLED.setBrightness(rgbBrightnessFor(state.intensity));
  leds[0] = colorValue(state.color);
  FastLED.show();
}

void activeLedOn()  { if (state.mode == DISCREET) irOn();  else rgbOnCurrentColor(); }
void activeLedOff() { if (state.mode == DISCREET) irOff(); else rgbOff(); }
void allOff() { irOff(); rgbOff(); }

// -------------------- Security: MAC Locking ----------------
// Get connected client's MAC address (only 1 client allowed via max_conn=1)
bool getClientMac(uint8_t* macOut) {
  wifi_sta_list_t stationList;

  if (esp_wifi_ap_get_sta_list(&stationList) != ESP_OK) return false;
  if (stationList.num == 0) return false;

  // With max_conn=1, there's only ever one client
  memcpy(macOut, stationList.sta[0].mac, 6);
  return true;
}

// Check if request is from locked MAC, or lock to first client
bool checkMacLock() {
  uint8_t clientMac[6];

  // If can't get MAC, allow (fallback for reliability)
  if (!getClientMac(clientMac)) {
    return true;
  }

  // First client - lock to their MAC
  if (!macLocked) {
    memcpy(lockedMac, clientMac, 6);
    macLocked = true;
    Serial.printf("[Security] Locked to MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  lockedMac[0], lockedMac[1], lockedMac[2],
                  lockedMac[3], lockedMac[4], lockedMac[5]);
    return true;
  }

  // Check if same MAC
  return memcmp(clientMac, lockedMac, 6) == 0;
}

// -------------------- Security: OTA Combination Lock -------
// Generate random 4-digit code (1-9 each)
// Note: Must be called with otaMutex held (called from handleOtaCode which acquires it)
void generateOtaCode() {
  for (int i = 0; i < OTA_CODE_LEN; i++) {
    otaCode[i] = (esp_random() % 9) + 1;  // 1-9
  }
  otaCodeValid = true;
  otaCodeGeneratedAt = millis();
  OTA_LOG("[OTA] Generated code: %d%d%d%d\n", otaCode[0], otaCode[1], otaCode[2], otaCode[3]);
}

// Display OTA code via RGB LED blinks: Red=digit1, Green=digit2, Blue=digit3, Yellow=digit4
void displayOtaCodeOnLed(const uint8_t* code) {
  // Stop any current playback
  bool wasPlaying = state.playing;
  state.playing = false;
  delay(200);
  allOff();

  // Blink pattern: Red, Green, Blue, Yellow for digits 1-4
  CRGB colors[OTA_CODE_LEN] = {CRGB(255,0,0), CRGB(0,255,0), CRGB(0,0,255), CRGB(255,255,0)};

  for (int digit = 0; digit < OTA_CODE_LEN; digit++) {
    delay(500);  // Pause between digits
    for (int blink = 0; blink < code[digit]; blink++) {
      leds[0] = colors[digit];
      FastLED.setBrightness(200);
      FastLED.show();
      delay(300);
      leds[0] = CRGB::Black;
      FastLED.show();
      delay(200);
    }
  }

  delay(300);
  allOff();

  // Restore playback state if needed
  if (wasPlaying) state.playing = true;
}

// Verify OTA code - invalidates after successful use
// Note: Must be called with otaMutex held (called from handleOtaUpload which acquires it)
bool verifyOtaCode(uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
  if (!otaCodeValid) return false;
  // Check TTL expiry
  if ((millis() - otaCodeGeneratedAt) > OTA_CODE_TTL_MS) {
    otaCodeValid = false;
    memset(otaCode, 0, OTA_CODE_LEN);
    OTA_LOGLN("[OTA] Code expired");
    return false;
  }
  bool match = (d1 == otaCode[0] && d2 == otaCode[1] && d3 == otaCode[2] && d4 == otaCode[3]);
  if (match) {
    otaCodeValid = false;  // Invalidate after successful use
    OTA_LOGLN("[OTA] Code verified successfully");
  } else {
    OTA_LOG("[OTA] Code verification failed: got %d%d%d%d, expected %d%d%d%d\n",
                  d1, d2, d3, d4, otaCode[0], otaCode[1], otaCode[2], otaCode[3]);
  }
  return match;
}

// -------------------- Morse Task --------------------------
TaskHandle_t morseTaskHandle = nullptr;
inline void morseDelay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void playSymbol(char s) {
  uint32_t onMs = (s == '.') ? ditDuration() : dahDuration();
  activeLedOn();
  morseDelay(onMs);
  
  allOff();
}

void playLetter(const char* code) {
  if (!code) return;
  for (size_t i=0; code[i]; i++) {
    if (!state.playing) {
      allOff(); 
      return;
    }
    playSymbol(code[i]);
    if (code[i+1]) morseDelay(gapIntra());
  }
}

void doDazzleGap() {
  allOff();  
  morseDelay(VISIBLE_DAZZLE_OFF1);

  uint32_t elapsed=0, step=30;
  while (elapsed < VISIBLE_DAZZLE_FLASH && state.playing) {
    FastLED.setBrightness(rgbBrightnessFor(state.intensity));
    leds[0]=CRGB(255,0,0); FastLED.show(); morseDelay(step);
    leds[0]=CRGB(0,255,0); FastLED.show(); morseDelay(step);
    leds[0]=CRGB(0,0,255); FastLED.show(); morseDelay(step);
    elapsed += step*3;
  }

  allOff();  
  morseDelay(VISIBLE_DAZZLE_OFF2);
}

void morseTask(void*) {
  for(;;){
    while (!state.playing) vTaskDelay(pdMS_TO_TICKS(50));

    while (state.playing) {
      String msg = getTextCopy();
      
      if (msg.length() == 0) {
        morseDelay(500);  // Wait before checking again
        continue;
      }
      
      for (size_t i=0; i<msg.length() && state.playing; i++) {
        char c = msg[i];

        // Check for prosign: <XX> syntax
        if (c == '<') {
          int closePos = msg.indexOf('>', i);
          if (closePos > (int)i + 1 && closePos <= (int)i + 4) {
            String tag = msg.substring(i + 1, closePos);
            const char* code = lookupProsign(tag);
            if (code) {
              playLetter(code);  // Play as single unit
              if (!state.playing) break;
              morseDelay(gapLetter());
              i = closePos;  // Skip past closing >
              continue;
            }
          }
        }

        if (c == ' ') {
          morseDelay(gapWord());
          continue;
        }
        const char* code = lookupMorse(c);
        if (code) {
          playLetter(code);
          if (!state.playing) break;
          morseDelay(gapLetter());
        }
      }

      if (!state.playing) break;

      if (state.mode == DISCREET) {
       
        allOff();
        morseDelay(LOOP_GAP);
      } else {
        if (state.dazzle) doDazzleGap();
        else { activeLedOff(); morseDelay(LOOP_GAP); }
      }
    }
    allOff();
  }
}

// -------------------- Web UI (+ splash via /bb.jpg) ------------------
static const char INDEX_HTML[] PROGMEM = R"====(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>The Signet Morse Beacon</title>
<style>
@font-face{font-family:'Harlow';src:url('/HarlowSolid.ttf') format('truetype')}
:root{--bg:#121212;--card:#1E1E1E;--text:#ECECEC;--muted:#B0B0B0;--accent:#8AB4F8;--primary:#8AB4F8}
*{box-sizing:border-box;font-family:Inter,system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;padding:0}
body{background:var(--bg);color:var(--text)}
.wrap{max-width:420px;margin:0 auto;padding:10px}
.appbar{text-align:center;padding:12px;font-family:'Harlow',cursive;font-size:28px;font-weight:400;color:#ddd;border-bottom:1px solid #222}
.grid{display:flex;flex-direction:column;gap:10px;margin-top:10px}
.row-2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.card{background:var(--card);border:1px solid #2A2A2A;border-radius:12px;padding:12px}
.card h3{font-size:11px;font-weight:600;color:#fff;text-transform:uppercase;letter-spacing:.5px;margin-bottom:8px}
.seg{display:flex;gap:6px}
.seg button{flex:1;border:0;border-radius:8px;padding:8px 4px;background:#171717;border:1px solid #2A2A2A;color:#ddd;font-size:12px;font-weight:500;cursor:pointer}
.seg button.active{background:var(--accent);color:#000;border-color:var(--accent)}
.seg button svg{width:18px;height:18px;fill:currentColor;vertical-align:middle}
.color-row{display:flex;gap:8px;align-items:center}
.color{width:28px;height:28px;border-radius:8px;border:1px solid #2A2A2A;cursor:pointer}
.color.active{outline:2px solid var(--accent)}
.color.red{background:#f44336}.color.green{background:#4caf50}.color.blue{background:#2196f3}
.color.custom{background:linear-gradient(135deg,#f44,#ff0,#0f0,#0ff,#00f,#f0f,#f44);position:relative}
.color-picker-wrap{display:none;margin-top:8px;text-align:center}
.color-picker-wrap.visible{display:block}
#wheelCanvas{cursor:crosshair;border-radius:50%}
.dazzle-row{display:flex;align-items:center;gap:8px}
.dazzle-row span{font-size:11px;color:var(--muted)}
.switch{position:relative;width:44px;height:24px}
.switch input{display:none}
.slider{position:absolute;cursor:pointer;inset:0;background:#333;border-radius:999px;transition:.2s}
.slider:before{content:"";position:absolute;height:18px;width:18px;left:3px;bottom:3px;background:#bbb;border-radius:50%;transition:.2s}
input:checked + .slider{background:var(--accent)}
input:checked + .slider:before{transform:translateX(20px);background:#111}
.wpm-row{display:flex;align-items:center;gap:12px}
.wpm-val{font-size:22px;font-weight:700;color:var(--accent);min-width:60px}
.wpm-val small{font-size:11px;color:var(--muted);font-weight:400}
.wpm-slider{flex:1}
.wpm-slider input[type="range"]{-webkit-appearance:none;width:100%;height:6px;border-radius:3px;background:#333}
.wpm-slider input[type="range"]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;background:var(--accent);cursor:pointer;border:2px solid #222}
.wpm-slider input[type="range"]::-moz-range-thumb{width:20px;height:20px;border-radius:50%;background:var(--accent);cursor:pointer;border:2px solid #222}
.wpm-labels{display:flex;justify-content:space-between;font-size:9px;color:#666;margin-top:4px;padding:0 2px}
.wpm-icon{font-size:20px;opacity:0.7;display:flex;align-items:center}
.wpm-icon svg{width:20px;height:20px;fill:var(--muted)}
.msg-input{width:100%;min-height:72px;background:#151515;border:1px solid #2A2A2A;border-radius:10px;padding:10px;color:#eee;font-size:16px;outline:none;resize:none;font-family:inherit;line-height:1.4}
.msg-hint{font-size:13px;color:#FFD700;margin:6px 0;text-align:center}
.msg-header{display:flex;align-items:baseline}
.max-hint{font-size:10px;color:#666;font-weight:400;margin-left:6px}
.tx-time{margin-left:auto;font-size:14px;color:#8AB4F8;font-weight:400}
.btn-row{display:flex;gap:8px;margin-top:8px}
.btn-row button{flex:1;border:0;border-radius:10px;padding:10px;font-weight:700;font-size:13px;cursor:pointer}
.btn-row .play{background:#333;color:#fff;border:1px solid #333}
.btn-row .play.active{background:var(--primary);color:#000;border-color:var(--primary)}
.btn-row .stop{background:transparent;color:#fff;border:1px solid #333}
.btn-row .stop.active{background:var(--primary);color:#000;border-color:var(--primary)}
.status{text-align:center;font-size:11px;color:#666;margin-top:6px}
.footer{display:flex;align-items:center;justify-content:center;font-size:14px;color:#FFD700;font-weight:700;margin-top:10px}
.footer-text{margin:0 8px}
.settings-btn{width:28px;height:28px;border-radius:50%;background:#333;color:#ccc;border:none;font-size:16px;line-height:28px;text-align:center;cursor:pointer}
.version{text-align:center;font-size:11px;color:#666;margin-top:6px}
.help-btn{display:inline-block;width:28px;height:28px;border-radius:50%;background:#333;color:#ccc;text-decoration:none;font-size:16px;line-height:28px;text-align:center;margin-right:8px;vertical-align:middle}
#splash{position:fixed;inset:0;background:rgba(0,0,0,.92);display:flex;flex-direction:column;align-items:center;justify-content:center;z-index:9999;opacity:0;pointer-events:none;transition:opacity .3s}
#splash p{color:#888;font-size:14px;margin-top:16px;font-style:italic}
#splash.visible{opacity:1;pointer-events:auto}
#splash img{max-width:90vw;max-height:90vh;border-radius:12px;box-shadow:0 0 40px rgba(0,0,0,.7)}
.ota-modal{position:fixed;inset:0;background:rgba(0,0,0,.9);display:flex;align-items:center;justify-content:center;z-index:9998;opacity:0;pointer-events:none;transition:opacity .2s}
.ota-modal.visible{opacity:1;pointer-events:auto}
.ota-card{background:var(--card);border-radius:16px;padding:20px;width:90%;max-width:320px}
.ota-card h3{color:#fff;margin:0 0 12px;font-size:16px;text-align:center}
.ota-close{position:absolute;top:12px;right:16px;background:none;border:none;color:#888;font-size:24px;cursor:pointer}
.ota-input{width:100%;padding:14px;margin-bottom:14px;border-radius:8px;border:1px solid #333;background:#222;color:#fff;font-size:16px;box-sizing:border-box}
.ota-input[type="file"]{padding:12px;height:auto;cursor:pointer}
.ota-btn{width:100%;padding:14px;border-radius:8px;border:none;background:var(--primary);color:#000;font-weight:600;font-size:16px;cursor:pointer}
.ota-btn:disabled{background:#555;color:#888;cursor:not-allowed}
.ota-progress{width:100%;height:6px;border-radius:3px;margin:12px 0;display:none}
.ota-status{text-align:center;font-size:12px;color:#888;margin-top:8px}
.ota-version{text-align:center;font-size:11px;color:#666;margin-bottom:12px}
/* RTL Support */
[dir="rtl"]{direction:rtl;text-align:right}
[dir="rtl"] .card{text-align:right}
[dir="rtl"] .msg-input{text-align:right}
[dir="rtl"] .footer{flex-direction:row-reverse}
[dir="rtl"] .msg-header{flex-direction:row-reverse}
[dir="rtl"] .max-hint{margin-left:0;margin-right:6px}
[dir="rtl"] .tx-time{margin-left:0;margin-right:auto}
[dir="rtl"] .wpm-row{flex-direction:row-reverse}
[dir="rtl"] .dazzle-row{flex-direction:row-reverse}
[dir="rtl"] .help-btn{margin-right:0;margin-left:8px}
/* Language Selection Modal */
.lang-modal{position:fixed;inset:0;background:rgba(0,0,0,.95);display:flex;align-items:center;justify-content:center;z-index:10000;opacity:0;pointer-events:none;transition:opacity .3s}
.lang-modal.visible{opacity:1;pointer-events:auto}
.lang-card{background:var(--card);border-radius:16px;padding:24px;width:90%;max-width:320px;text-align:center}
.lang-card h2{color:#fff;font-size:18px;margin-bottom:8px}
.lang-card p{color:#888;font-size:12px;margin-bottom:20px}
.lang-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.lang-btn{padding:14px 8px;border-radius:10px;border:1px solid #333;background:#222;color:#fff;font-size:14px;cursor:pointer;transition:all .2s}
.lang-btn:hover{background:#333;border-color:var(--accent)}
.lang-btn.selected{background:var(--accent);color:#000;border-color:var(--accent)}
.lang-native{display:block;font-size:12px;color:#888;margin-top:2px}
.lang-btn.selected .lang-native{color:#333}
.lang-confirm{margin-top:16px;width:100%;padding:14px;border-radius:10px;border:none;background:var(--primary);color:#000;font-weight:600;font-size:16px;cursor:pointer}
.lang-confirm:disabled{background:#555;color:#888;cursor:not-allowed}
/* Settings Language Section */
.settings-lang{margin-top:16px;padding-top:16px;border-top:1px solid #333}
.settings-lang h4{color:#888;font-size:11px;text-transform:uppercase;margin-bottom:10px}
.settings-lang select{width:100%;padding:12px;border-radius:8px;border:1px solid #333;background:#222;color:#fff;font-size:14px;cursor:pointer}
</style>
</head>
<body>

<div id="splash"><img src="/bb.jpg" alt="Big Brother"><p id="splashText">Click anywhere to send him a message...</p></div>

<!-- Language Selection Modal (First Boot) -->
<div class="lang-modal" id="langModal">
  <div class="lang-card">
    <h2 id="langTitle">Select Language</h2>
    <p id="langSubtitle">Choose your preferred language</p>
    <div class="lang-grid" id="langGrid"></div>
    <button class="lang-confirm" id="langConfirm" disabled>Continue</button>
  </div>
</div>

<div class="appbar">The Signet</div>
<div class="wrap">
<div class="grid">

  <div class="card">
    <h3 class="msg-header"><span id="lblMessage">Message</span> <span class="max-hint" id="lblMaxChars">(MAX 200 Characters)</span><span class="tx-time" id="txTime"></span></h3>
    <textarea class="msg-input" id="msg" placeholder="SOS" maxlength="200" rows="3"></textarea>
    <div class="msg-hint" id="lblHint">A-Z 0-9 . , ? / - ( ) @ = space &lt;KA&gt; &lt;AR&gt;</div>
    <div class="btn-row">
      <button class="play" id="play" title="Play">&#9654;</button>
      <button class="stop" id="stop" title="Stop">&#9632;</button>
    </div>
    <div class="status" id="status">Idle</div>
  </div>

  <div class="row-2">
    <div class="card">
      <h3 id="lblMode">Mode</h3>
      <div class="seg" id="modeGroup">
        <button data-mode="DISCREET" id="btnIR" title="IR/Invisible"><svg viewBox="0 0 24 24" width="18" height="18"><path fill="currentColor" d="M12 7c2.76 0 5 2.24 5 5 0 .65-.13 1.26-.36 1.83l2.92 2.92c1.51-1.26 2.7-2.89 3.43-4.75-1.73-4.39-6-7.5-11-7.5-1.4 0-2.74.25-3.98.7l2.16 2.16C10.74 7.13 11.35 7 12 7zM2 4.27l2.28 2.28.46.46A11.804 11.804 0 001 12c1.73 4.39 6 7.5 11 7.5 1.55 0 3.03-.3 4.38-.84l.42.42L19.73 22 21 20.73 3.27 3 2 4.27zM7.53 9.8l1.55 1.55c-.05.21-.08.43-.08.65 0 1.66 1.34 3 3 3 .22 0 .44-.03.65-.08l1.55 1.55c-.67.33-1.41.53-2.2.53-2.76 0-5-2.24-5-5 0-.79.2-1.53.53-2.2zm4.31-.78l3.15 3.15.02-.16c0-1.66-1.34-3-3-3l-.17.01z"/></svg></button>
        <button data-mode="VISIBLE" id="btnRGB" title="RGB/Visible"><svg viewBox="0 0 24 24" width="18" height="18"><path fill="currentColor" d="M12 4.5C7 4.5 2.73 7.61 1 12c1.73 4.39 6 7.5 11 7.5s9.27-3.11 11-7.5c-1.73-4.39-6-7.5-11-7.5zM12 17c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.66 0-3 1.34-3 3s1.34 3 3 3 3-1.34 3-3-1.34-3-3-3z"/></svg></button>
      </div>
    </div>
    <div class="card">
      <h3 id="lblIntensity">Intensity</h3>
      <div class="seg" id="intensityGroup">
        <button data-int="LOW" id="btnLo" title="Low"><svg viewBox="0 0 24 24" width="18" height="18"><circle cx="12" cy="12" r="4" fill="currentColor"/><line x1="12" y1="2" x2="12" y2="5" stroke="currentColor" stroke-width="2"/><line x1="12" y1="19" x2="12" y2="22" stroke="currentColor" stroke-width="2"/><line x1="2" y1="12" x2="5" y2="12" stroke="currentColor" stroke-width="2"/><line x1="19" y1="12" x2="22" y2="12" stroke="currentColor" stroke-width="2"/></svg></button>
        <button data-int="MED" id="btnMed" title="Medium"><svg viewBox="0 0 24 24" width="18" height="18"><circle cx="12" cy="12" r="4" fill="currentColor"/><line x1="12" y1="1" x2="12" y2="5" stroke="currentColor" stroke-width="2"/><line x1="12" y1="19" x2="12" y2="23" stroke="currentColor" stroke-width="2"/><line x1="1" y1="12" x2="5" y2="12" stroke="currentColor" stroke-width="2"/><line x1="19" y1="12" x2="23" y2="12" stroke="currentColor" stroke-width="2"/><line x1="4.22" y1="4.22" x2="6.64" y2="6.64" stroke="currentColor" stroke-width="2"/><line x1="17.36" y1="17.36" x2="19.78" y2="19.78" stroke="currentColor" stroke-width="2"/><line x1="4.22" y1="19.78" x2="6.64" y2="17.36" stroke="currentColor" stroke-width="2"/><line x1="17.36" y1="6.64" x2="19.78" y2="4.22" stroke="currentColor" stroke-width="2"/></svg></button>
        <button data-int="HIGH" id="btnHi" title="High"><svg viewBox="0 0 24 24" width="18" height="18"><circle cx="12" cy="12" r="5" fill="currentColor"/><line x1="12" y1="0" x2="12" y2="5" stroke="currentColor" stroke-width="2.5"/><line x1="12" y1="19" x2="12" y2="24" stroke="currentColor" stroke-width="2.5"/><line x1="0" y1="12" x2="5" y2="12" stroke="currentColor" stroke-width="2.5"/><line x1="19" y1="12" x2="24" y2="12" stroke="currentColor" stroke-width="2.5"/><line x1="3.5" y1="3.5" x2="6.64" y2="6.64" stroke="currentColor" stroke-width="2.5"/><line x1="17.36" y1="17.36" x2="20.5" y2="20.5" stroke="currentColor" stroke-width="2.5"/><line x1="3.5" y1="20.5" x2="6.64" y2="17.36" stroke="currentColor" stroke-width="2.5"/><line x1="17.36" y1="6.64" x2="20.5" y2="3.5" stroke="currentColor" stroke-width="2.5"/></svg></button>
      </div>
    </div>
  </div>

  <div class="row-2">
    <div class="card" id="colorCard">
      <h3 id="lblColor">Color</h3>
      <div class="color-row" id="colorGroup">
        <div class="color red" data-color="RED"></div>
        <div class="color green" data-color="GREEN"></div>
        <div class="color blue" data-color="BLUE"></div>
        <div class="color custom" data-color="CUSTOM" id="customSwatch"></div>
      </div>
      <div class="color-picker-wrap" id="pickerWrap">
        <canvas id="wheelCanvas" width="80" height="80"></canvas>
      </div>
    </div>
    <div class="card" id="dazzleCard">
      <h3 id="lblDazzle">Dazzle</h3>
      <div class="dazzle-row">
        <label class="switch"><input type="checkbox" id="dazzle"><span class="slider"></span></label>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="wpm-row">
      <span class="wpm-icon">&#128034;</span>
      <div class="wpm-slider">
        <input type="range" id="wpm" min="5" max="20" step="5" value="10">
      </div>
      <span class="wpm-icon">&#128007;</span>
    </div>
    <div class="wpm-val" style="text-align:center;margin-top:8px"><span id="wpmDisplay">10</span> <small>WPM</small></div>
  </div>

</div>
<div class="footer"><a href="/help" class="help-btn">?</a><span class="footer-text" id="footerText">1984 was not an instruction manual</span><button class="settings-btn" id="settingsBtn">&#9881;</button></div>
<div class="version" id="version"></div>
</div>

<!-- OTA Modal -->
<div class="ota-modal" id="otaModal">
  <div class="ota-card">
    <h3 id="otaTitle">Firmware Update</h3>
    <div class="ota-version"><span id="otaCurrent">Current</span>: v<span id="otaVersion">-</span></div>
    <input type="file" id="otaFile" accept=".bin" style="position:absolute;opacity:0;width:1px;height:1px">
    <label for="otaFile" class="ota-btn" id="otaChooseLabel" style="display:block;text-align:center;background:#333;color:#fff;margin-bottom:14px;cursor:pointer">Choose .bin File</label>
    <div id="otaFileName" style="text-align:center;font-size:12px;color:#888;margin-bottom:14px">No file selected</div>
    <button class="ota-btn" id="otaGetCode" style="background:#1a5f2a;margin-bottom:10px">Get Code</button>
    <div id="otaCodeSection" style="display:none;margin-bottom:14px">
      <div style="color:#8AB4F8;margin-bottom:8px;text-align:center;font-size:13px">Enter LED code:</div>
      <div style="display:flex;gap:8px;justify-content:center">
        <input type="number" id="otaD1" min="1" max="9" style="width:45px;height:45px;text-align:center;font-size:22px;border-radius:6px;border:2px solid #ff4444;background:#331111;color:#ff6666">
        <input type="number" id="otaD2" min="1" max="9" style="width:45px;height:45px;text-align:center;font-size:22px;border-radius:6px;border:2px solid #44ff44;background:#113311;color:#66ff66">
        <input type="number" id="otaD3" min="1" max="9" style="width:45px;height:45px;text-align:center;font-size:22px;border-radius:6px;border:2px solid #4444ff;background:#111133;color:#6666ff">
        <input type="number" id="otaD4" min="1" max="9" style="width:45px;height:45px;text-align:center;font-size:22px;border-radius:6px;border:2px solid #ffff44;background:#333311;color:#ffff66">
      </div>
      <div style="color:#888;font-size:11px;margin-top:6px;text-align:center">Watch LED: Red, Green, Blue, Yellow blinks</div>
    </div>
    <button class="ota-btn" id="otaUpload">Upload Firmware</button>
    <progress class="ota-progress" id="otaProgress" value="0" max="100"></progress>
    <div class="ota-status" id="otaStatus"></div>
    <div class="settings-lang">
      <h4 id="lblLanguage">Language</h4>
      <select id="langSelect"></select>
    </div>
    <button class="ota-btn" id="otaClose" style="margin-top:12px;background:#333;color:#fff">Close</button>
  </div>
</div>

<!-- Android Browser Dialog -->
<div class="ota-modal" id="androidDialog">
  <div class="ota-card">
    <h3 id="androidTitle">Open in Browser</h3>
    <p id="androidDesc" style="font-size:13px;color:#ccc;text-align:center;margin-bottom:16px">Android requires a full browser for firmware updates. Copy the link and open it in Chrome.</p>
    <button class="ota-btn" id="copyLinkBtn">Copy Link</button>
    <div id="copyStatus" style="text-align:center;font-size:12px;color:#888;margin-top:8px"></div>
    <button class="ota-btn" id="androidClose" style="margin-top:12px;background:#333;color:#fff">Close</button>
  </div>
</div>

<script>
const SPLASH_KEY = 'signet_splash_seen';
const LANG_KEY = 'signet_lang';
const MORSE={A:'.-',B:'-...',C:'-.-.',D:'-..',E:'.',F:'..-.',G:'--.',H:'....',I:'..',J:'.---',K:'-.-',L:'.-..',M:'--',N:'-.',O:'---',P:'.--.',Q:'--.-',R:'.-.',S:'...',T:'-',U:'..-',V:'...-',W:'.--',X:'-..-',Y:'-.--',Z:'--..',0:'-----',1:'.----',2:'..---',3:'...--',4:'....-',5:'.....',6:'-....',7:'--...',8:'---..',9:'----.','.':'.-.-.-',',':'--..--','?':'..--..','/':'-..-.','-':'-....-','(':'-.--.',')':'-.--.-','@':'.--.-.','=':'-...-'};
const PROSIGNS={KA:'-.-.-',AR:'.-.-.'};

// Translations for all supported languages
const LANG = {
  en: { name:'English', native:'English', dir:'ltr',
    splash:'Click anywhere to send him a message...',
    message:'Message', maxChars:'(MAX 200 Characters)', hint:'A-Z 0-9 . , ? / - ( ) @ = space <KA> <AR>',
    idle:'Idle', playing:'Playing\u2026',
    mode:'Mode', intensity:'Intensity', color:'Color', dazzle:'Dazzle',
    footer:'1984 was not an instruction manual', firmware:'Firmware',
    fwUpdate:'Firmware Update', current:'Current', chooseBin:'Choose .bin File', noFile:'No file selected',
    uploadFw:'Upload Firmware', uploading:'Uploading...', success:'Success! Rebooting...', updateFailed:'Update failed',
    netError:'Network error', selectBin:'Select a .bin file', errBin:'Error: Select a .bin file',
    openBrowser:'Open in Browser', androidDesc:'Android requires a full browser for firmware updates. Copy the link and open it in Chrome.',
    copyLink:'Copy Link', copied:'Copied! Open Chrome and paste.', copyFailed:'Copy failed. Type: 192.168.4.1', close:'Close',
    language:'Language', selectLang:'Select Language', chooseLang:'Choose your preferred language', continue:'Continue'
  },
  es: { name:'Spanish', native:'Espa\u00F1ol', dir:'ltr',
    splash:'Haz clic en cualquier lugar para enviarle un mensaje...',
    message:'Mensaje', maxChars:'(M\u00C1X 200 Caracteres)', hint:'A-Z 0-9 . , ? / - ( ) @ = espacio <KA> <AR>',
    idle:'Inactivo', playing:'Reproduciendo\u2026',
    mode:'Modo', intensity:'Intensidad', color:'Color', dazzle:'Destello',
    footer:'1984 no era un manual de instrucciones', firmware:'Firmware',
    fwUpdate:'Actualizar Firmware', current:'Actual', chooseBin:'Elegir archivo .bin', noFile:'Ning\u00FAn archivo seleccionado',
    uploadFw:'Subir Firmware', uploading:'Subiendo...', success:'\u00A1\u00C9xito! Reiniciando...', updateFailed:'Actualizaci\u00F3n fallida',
    netError:'Error de red', selectBin:'Selecciona un archivo .bin', errBin:'Error: Selecciona un archivo .bin',
    openBrowser:'Abrir en Navegador', androidDesc:'Android requiere un navegador completo para actualizaciones. Copia el enlace y \u00E1brelo en Chrome.',
    copyLink:'Copiar Enlace', copied:'\u00A1Copiado! Abre Chrome y pega.', copyFailed:'Copia fallida. Escribe: 192.168.4.1', close:'Cerrar',
    language:'Idioma', selectLang:'Seleccionar Idioma', chooseLang:'Elige tu idioma preferido', continue:'Continuar'
  },
  fr: { name:'French', native:'Fran\u00E7ais', dir:'ltr',
    splash:'Cliquez n\'importe o\u00F9 pour lui envoyer un message...',
    message:'Message', maxChars:'(MAX 200 Caract\u00E8res)', hint:'A-Z 0-9 . , ? / - ( ) @ = espace <KA> <AR>',
    idle:'Inactif', playing:'Lecture\u2026',
    mode:'Mode', intensity:'Intensit\u00E9', color:'Couleur', dazzle:'\u00C9blouir',
    footer:'1984 n\'\u00E9tait pas un manuel d\'instructions', firmware:'Firmware',
    fwUpdate:'Mise \u00E0 jour Firmware', current:'Actuel', chooseBin:'Choisir fichier .bin', noFile:'Aucun fichier s\u00E9lectionn\u00E9',
    uploadFw:'T\u00E9l\u00E9verser Firmware', uploading:'T\u00E9l\u00E9versement...', success:'Succ\u00E8s! Red\u00E9marrage...', updateFailed:'\u00C9chec de mise \u00E0 jour',
    netError:'Erreur r\u00E9seau', selectBin:'S\u00E9lectionnez un fichier .bin', errBin:'Erreur: S\u00E9lectionnez un fichier .bin',
    openBrowser:'Ouvrir dans Navigateur', androidDesc:'Android n\u00E9cessite un navigateur complet pour les mises \u00E0 jour. Copiez le lien et ouvrez-le dans Chrome.',
    copyLink:'Copier Lien', copied:'Copi\u00E9! Ouvrez Chrome et collez.', copyFailed:'\u00C9chec de copie. Tapez: 192.168.4.1', close:'Fermer',
    language:'Langue', selectLang:'S\u00E9lectionner Langue', chooseLang:'Choisissez votre langue pr\u00E9f\u00E9r\u00E9e', continue:'Continuer'
  },
  ru: { name:'Russian', native:'\u0420\u0443\u0441\u0441\u043A\u0438\u0439', dir:'ltr',
    splash:'\u041D\u0430\u0436\u043C\u0438\u0442\u0435 \u0432 \u043B\u044E\u0431\u043E\u043C \u043C\u0435\u0441\u0442\u0435, \u0447\u0442\u043E\u0431\u044B \u043E\u0442\u043F\u0440\u0430\u0432\u0438\u0442\u044C \u0435\u043C\u0443 \u0441\u043E\u043E\u0431\u0449\u0435\u043D\u0438\u0435...',
    message:'\u0421\u043E\u043E\u0431\u0449\u0435\u043D\u0438\u0435', maxChars:'(\u041C\u0410\u041A\u0421 200 \u0441\u0438\u043C\u0432\u043E\u043B\u043E\u0432)', hint:'A-Z 0-9 . , ? / - ( ) @ = \u043F\u0440\u043E\u0431\u0435\u043B <KA> <AR>',
    idle:'\u041E\u0436\u0438\u0434\u0430\u043D\u0438\u0435', playing:'\u0412\u043E\u0441\u043F\u0440\u043E\u0438\u0437\u0432\u0435\u0434\u0435\u043D\u0438\u0435\u2026',
    mode:'\u0420\u0435\u0436\u0438\u043C', intensity:'\u0418\u043D\u0442\u0435\u043D\u0441.', color:'\u0426\u0432\u0435\u0442', dazzle:'\u0412\u0441\u043F\u044B\u0448\u043A\u0430',
    footer:'1984 \u043D\u0435 \u0431\u044B\u043B \u0438\u043D\u0441\u0442\u0440\u0443\u043A\u0446\u0438\u0435\u0439', firmware:'\u041F\u0440\u043E\u0448\u0438\u0432\u043A\u0430',
    fwUpdate:'\u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 \u043F\u0440\u043E\u0448\u0438\u0432\u043A\u0438', current:'\u0422\u0435\u043A\u0443\u0449\u0430\u044F', chooseBin:'\u0412\u044B\u0431\u0440\u0430\u0442\u044C .bin', noFile:'\u0424\u0430\u0439\u043B \u043D\u0435 \u0432\u044B\u0431\u0440\u0430\u043D',
    uploadFw:'\u0417\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044C', uploading:'\u0417\u0430\u0433\u0440\u0443\u0437\u043A\u0430...', success:'\u0423\u0441\u043F\u0435\u0445! \u041F\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u043A\u0430...', updateFailed:'\u041E\u0448\u0438\u0431\u043A\u0430 \u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u044F',
    netError:'\u041E\u0448\u0438\u0431\u043A\u0430 \u0441\u0435\u0442\u0438', selectBin:'\u0412\u044B\u0431\u0435\u0440\u0438\u0442\u0435 .bin', errBin:'\u041E\u0448\u0438\u0431\u043A\u0430: \u0412\u044B\u0431\u0435\u0440\u0438\u0442\u0435 .bin',
    openBrowser:'\u041E\u0442\u043A\u0440\u044B\u0442\u044C \u0432 \u0431\u0440\u0430\u0443\u0437\u0435\u0440\u0435', androidDesc:'Android \u0442\u0440\u0435\u0431\u0443\u0435\u0442 \u043F\u043E\u043B\u043D\u044B\u0439 \u0431\u0440\u0430\u0443\u0437\u0435\u0440. \u0421\u043A\u043E\u043F\u0438\u0440\u0443\u0439\u0442\u0435 \u0441\u0441\u044B\u043B\u043A\u0443 \u0438 \u043E\u0442\u043A\u0440\u043E\u0439\u0442\u0435 \u0432 Chrome.',
    copyLink:'\u041A\u043E\u043F\u0438\u0440\u043E\u0432\u0430\u0442\u044C', copied:'\u0421\u043A\u043E\u043F\u0438\u0440\u043E\u0432\u0430\u043D\u043E! \u041E\u0442\u043A\u0440\u043E\u0439\u0442\u0435 Chrome.', copyFailed:'\u041E\u0448\u0438\u0431\u043A\u0430. \u0412\u0432\u0435\u0434\u0438\u0442\u0435: 192.168.4.1', close:'\u0417\u0430\u043A\u0440\u044B\u0442\u044C',
    language:'\u042F\u0437\u044B\u043A', selectLang:'\u0412\u044B\u0431\u0440\u0430\u0442\u044C \u044F\u0437\u044B\u043A', chooseLang:'\u0412\u044B\u0431\u0435\u0440\u0438\u0442\u0435 \u043F\u0440\u0435\u0434\u043F\u043E\u0447\u0438\u0442\u0430\u0435\u043C\u044B\u0439 \u044F\u0437\u044B\u043A', continue:'\u041F\u0440\u043E\u0434\u043E\u043B\u0436\u0438\u0442\u044C'
  },
  'zh-CN': { name:'Simplified Chinese', native:'\u7B80\u4F53\u4E2D\u6587', dir:'ltr',
    splash:'\u70B9\u51FB\u4EFB\u610F\u4F4D\u7F6E\u7ED9\u4ED6\u53D1\u9001\u6D88\u606F...',
    message:'\u6D88\u606F', maxChars:'(\u6700\u591A200\u5B57\u7B26)', hint:'A-Z 0-9 . , ? / - ( ) @ = \u7A7A\u683C <KA> <AR>',
    idle:'\u7A7A\u95F2', playing:'\u64AD\u653E\u4E2D\u2026',
    mode:'\u6A21\u5F0F', intensity:'\u5F3A\u5EA6', color:'\u989C\u8272', dazzle:'\u95EA\u70C1',
    footer:'1984\u4E0D\u662F\u64CD\u4F5C\u624B\u518C', firmware:'\u56FA\u4EF6',
    fwUpdate:'\u56FA\u4EF6\u66F4\u65B0', current:'\u5F53\u524D', chooseBin:'\u9009\u62E9.bin\u6587\u4EF6', noFile:'\u672A\u9009\u62E9\u6587\u4EF6',
    uploadFw:'\u4E0A\u4F20\u56FA\u4EF6', uploading:'\u4E0A\u4F20\u4E2D...', success:'\u6210\u529F\uFF01\u91CD\u542F\u4E2D...', updateFailed:'\u66F4\u65B0\u5931\u8D25',
    netError:'\u7F51\u7EDC\u9519\u8BEF', selectBin:'\u8BF7\u9009\u62E9.bin\u6587\u4EF6', errBin:'\u9519\u8BEF\uFF1A\u8BF7\u9009\u62E9.bin\u6587\u4EF6',
    openBrowser:'\u5728\u6D4F\u89C8\u5668\u4E2D\u6253\u5F00', androidDesc:'Android\u9700\u8981\u5B8C\u6574\u6D4F\u89C8\u5668\u8FDB\u884C\u56FA\u4EF6\u66F4\u65B0\u3002\u590D\u5236\u94FE\u63A5\u5E76\u5728Chrome\u4E2D\u6253\u5F00\u3002',
    copyLink:'\u590D\u5236\u94FE\u63A5', copied:'\u5DF2\u590D\u5236\uFF01\u5728Chrome\u4E2D\u7C98\u8D34\u3002', copyFailed:'\u590D\u5236\u5931\u8D25\u3002\u8F93\u5165\uFF1A192.168.4.1', close:'\u5173\u95ED',
    language:'\u8BED\u8A00', selectLang:'\u9009\u62E9\u8BED\u8A00', chooseLang:'\u9009\u62E9\u60A8\u7684\u9996\u9009\u8BED\u8A00', continue:'\u7EE7\u7EED'
  },
  'zh-TW': { name:'Traditional Chinese', native:'\u7E41\u9AD4\u4E2D\u6587', dir:'ltr',
    splash:'\u9EDE\u64CA\u4EFB\u610F\u4F4D\u7F6E\u7D66\u4ED6\u767C\u9001\u8A0A\u606F...',
    message:'\u8A0A\u606F', maxChars:'(\u6700\u591A200\u5B57\u5143)', hint:'A-Z 0-9 . , ? / - ( ) @ = \u7A7A\u683C <KA> <AR>',
    idle:'\u9592\u7F6E', playing:'\u64AD\u653E\u4E2D\u2026',
    mode:'\u6A21\u5F0F', intensity:'\u5F37\u5EA6', color:'\u984F\u8272', dazzle:'\u9583\u720D',
    footer:'1984\u4E0D\u662F\u64CD\u4F5C\u624B\u518A', firmware:'\u97CC\u9AD4',
    fwUpdate:'\u97CC\u9AD4\u66F4\u65B0', current:'\u76EE\u524D', chooseBin:'\u9078\u64C7.bin\u6A94\u6848', noFile:'\u672A\u9078\u64C7\u6A94\u6848',
    uploadFw:'\u4E0A\u50B3\u97CC\u9AD4', uploading:'\u4E0A\u50B3\u4E2D...', success:'\u6210\u529F\uFF01\u91CD\u555F\u4E2D...', updateFailed:'\u66F4\u65B0\u5931\u6557',
    netError:'\u7DB2\u8DEF\u932F\u8AA4', selectBin:'\u8ACB\u9078\u64C7.bin\u6A94\u6848', errBin:'\u932F\u8AA4\uFF1A\u8ACB\u9078\u64C7.bin\u6A94\u6848',
    openBrowser:'\u5728\u700F\u89BD\u5668\u4E2D\u958B\u555F', androidDesc:'Android\u9700\u8981\u5B8C\u6574\u700F\u89BD\u5668\u9032\u884C\u97CC\u9AD4\u66F4\u65B0\u3002\u8907\u88FD\u9023\u7D50\u4E26\u5728Chrome\u4E2D\u958B\u555F\u3002',
    copyLink:'\u8907\u88FD\u9023\u7D50', copied:'\u5DF2\u8907\u88FD\uFF01\u5728Chrome\u4E2D\u8CBB\u4E0A\u3002', copyFailed:'\u8907\u88FD\u5931\u6557\u3002\u8F38\u5165\uFF1A192.168.4.1', close:'\u95DC\u9589',
    language:'\u8A9E\u8A00', selectLang:'\u9078\u64C7\u8A9E\u8A00', chooseLang:'\u9078\u64C7\u60A8\u7684\u9996\u9078\u8A9E\u8A00', continue:'\u7E7C\u7E8C'
  },
  fa: { name:'Farsi', native:'\u0641\u0627\u0631\u0633\u06CC', dir:'rtl',
    splash:'\u0628\u0631\u0627\u06CC \u0627\u0631\u0633\u0627\u0644 \u067E\u06CC\u0627\u0645 \u0628\u0647 \u0627\u0648\u060C \u0647\u0631 \u062C\u0627\u06CC\u06CC \u06A9\u0644\u06CC\u06A9 \u06A9\u0646\u06CC\u062F...',
    message:'\u067E\u06CC\u0627\u0645', maxChars:'(\u062D\u062F\u0627\u06A9\u062B\u0631 \u06F2\u06F0\u06F0 \u06A9\u0627\u0631\u0627\u06A9\u062A\u0631)', hint:'A-Z 0-9 . , ? / - ( ) @ = \u0641\u0627\u0635\u0644\u0647 <KA> <AR>',
    idle:'\u0622\u0645\u0627\u062F\u0647', playing:'\u062F\u0631 \u062D\u0627\u0644 \u067E\u062E\u0634\u2026',
    mode:'\u062D\u0627\u0644\u062A', intensity:'\u0634\u062F\u062A', color:'\u0631\u0646\u06AF', dazzle:'\u062F\u0631\u062E\u0634\u0634',
    footer:'\u06F1\u06F9\u06F8\u06F4 \u06CC\u06A9 \u06A9\u062A\u0627\u0628 \u0631\u0627\u0647\u0646\u0645\u0627 \u0646\u0628\u0648\u062F', firmware:'\u0641\u0631\u06CC\u0645\u0648\u0631',
    fwUpdate:'\u0628\u0631\u0648\u0632\u0631\u0633\u0627\u0646\u06CC \u0641\u0631\u06CC\u0645\u0648\u0631', current:'\u0641\u0639\u0644\u06CC', chooseBin:'\u0627\u0646\u062A\u062E\u0627\u0628 \u0641\u0627\u06CC\u0644 .bin', noFile:'\u0641\u0627\u06CC\u0644\u06CC \u0627\u0646\u062A\u062E\u0627\u0628 \u0646\u0634\u062F\u0647',
    uploadFw:'\u0628\u0627\u0631\u06AF\u0630\u0627\u0631\u06CC', uploading:'\u062F\u0631 \u062D\u0627\u0644 \u0628\u0627\u0631\u06AF\u0630\u0627\u0631\u06CC...', success:'\u0645\u0648\u0641\u0642! \u0631\u0627\u0647\u200C\u0627\u0646\u062F\u0627\u0632\u06CC \u0645\u062C\u062F\u062F...', updateFailed:'\u0628\u0631\u0648\u0632\u0631\u0633\u0627\u0646\u06CC \u0646\u0627\u0645\u0648\u0641\u0642',
    netError:'\u062E\u0637\u0627\u06CC \u0634\u0628\u06A9\u0647', selectBin:'\u06CC\u06A9 \u0641\u0627\u06CC\u0644 .bin \u0627\u0646\u062A\u062E\u0627\u0628 \u06A9\u0646\u06CC\u062F', errBin:'\u062E\u0637\u0627: \u0641\u0627\u06CC\u0644 .bin \u0627\u0646\u062A\u062E\u0627\u0628 \u06A9\u0646\u06CC\u062F',
    openBrowser:'\u0628\u0627\u0632 \u06A9\u0631\u062F\u0646 \u062F\u0631 \u0645\u0631\u0648\u0631\u06AF\u0631', androidDesc:'\u0627\u0646\u062F\u0631\u0648\u06CC\u062F \u0628\u0647 \u0645\u0631\u0648\u0631\u06AF\u0631 \u06A9\u0627\u0645\u0644 \u0646\u06CC\u0627\u0632 \u062F\u0627\u0631\u062F. \u0644\u06CC\u0646\u06A9 \u0631\u0627 \u06A9\u067E\u06CC \u06A9\u0646\u06CC\u062F \u0648 \u062F\u0631 Chrome \u0628\u0627\u0632 \u06A9\u0646\u06CC\u062F.',
    copyLink:'\u06A9\u067E\u06CC \u0644\u06CC\u0646\u06A9', copied:'\u06A9\u067E\u06CC \u0634\u062F! Chrome \u0631\u0627 \u0628\u0627\u0632 \u06A9\u0646\u06CC\u062F.', copyFailed:'\u06A9\u067E\u06CC \u0646\u0634\u062F. \u062A\u0627\u06CC\u067E \u06A9\u0646\u06CC\u062F: 192.168.4.1', close:'\u0628\u0633\u062A\u0646',
    language:'\u0632\u0628\u0627\u0646', selectLang:'\u0627\u0646\u062A\u062E\u0627\u0628 \u0632\u0628\u0627\u0646', chooseLang:'\u0632\u0628\u0627\u0646 \u0645\u0648\u0631\u062F \u0646\u0638\u0631 \u062E\u0648\u062F \u0631\u0627 \u0627\u0646\u062A\u062E\u0627\u0628 \u06A9\u0646\u06CC\u062F', continue:'\u0627\u062F\u0627\u0645\u0647'
  },
  ar: { name:'Arabic', native:'\u0627\u0644\u0639\u0631\u0628\u064A\u0629', dir:'rtl',
    splash:'\u0627\u0646\u0642\u0631 \u0641\u064A \u0623\u064A \u0645\u0643\u0627\u0646 \u0644\u0625\u0631\u0633\u0627\u0644 \u0631\u0633\u0627\u0644\u0629 \u0625\u0644\u064A\u0647...',
    message:'\u0631\u0633\u0627\u0644\u0629', maxChars:'(\u0623\u0642\u0635\u0649 \u0662\u0660\u0660 \u062D\u0631\u0641)', hint:'A-Z 0-9 . , ? / - ( ) @ = \u0645\u0633\u0627\u0641\u0629 <KA> <AR>',
    idle:'\u062E\u0627\u0645\u0644', playing:'\u062C\u0627\u0631\u064A \u0627\u0644\u062A\u0634\u063A\u064A\u0644\u2026',
    mode:'\u0627\u0644\u0648\u0636\u0639', intensity:'\u0627\u0644\u0634\u062F\u0629', color:'\u0627\u0644\u0644\u0648\u0646', dazzle:'\u0648\u0645\u064A\u0636',
    footer:'\u0661\u0669\u0668\u0664 \u0644\u0645 \u064A\u0643\u0646 \u062F\u0644\u064A\u0644 \u062A\u0639\u0644\u064A\u0645\u0627\u062A', firmware:'\u0628\u0631\u0646\u0627\u0645\u062C \u062B\u0627\u0628\u062A',
    fwUpdate:'\u062A\u062D\u062F\u064A\u062B \u0627\u0644\u0628\u0631\u0646\u0627\u0645\u062C', current:'\u0627\u0644\u062D\u0627\u0644\u064A', chooseBin:'\u0627\u062E\u062A\u0631 \u0645\u0644\u0641 .bin', noFile:'\u0644\u0645 \u064A\u062A\u0645 \u0627\u062E\u062A\u064A\u0627\u0631 \u0645\u0644\u0641',
    uploadFw:'\u0631\u0641\u0639 \u0627\u0644\u0628\u0631\u0646\u0627\u0645\u062C', uploading:'\u062C\u0627\u0631\u064A \u0627\u0644\u0631\u0641\u0639...', success:'\u0646\u062C\u0627\u062D! \u0625\u0639\u0627\u062F\u0629 \u0627\u0644\u062A\u0634\u063A\u064A\u0644...', updateFailed:'\u0641\u0634\u0644 \u0627\u0644\u062A\u062D\u062F\u064A\u062B',
    netError:'\u062E\u0637\u0623 \u0641\u064A \u0627\u0644\u0634\u0628\u0643\u0629', selectBin:'\u0627\u062E\u062A\u0631 \u0645\u0644\u0641 .bin', errBin:'\u062E\u0637\u0623: \u0627\u062E\u062A\u0631 \u0645\u0644\u0641 .bin',
    openBrowser:'\u0641\u062A\u062D \u0641\u064A \u0627\u0644\u0645\u062A\u0635\u0641\u062D', androidDesc:'\u064A\u062A\u0637\u0644\u0628 \u0623\u0646\u062F\u0631\u0648\u064A\u062F \u0645\u062A\u0635\u0641\u062D\u0627\u064B \u0643\u0627\u0645\u0644\u0627\u064B. \u0627\u0646\u0633\u062E \u0627\u0644\u0631\u0627\u0628\u0637 \u0648\u0627\u0641\u062A\u062D\u0647 \u0641\u064A Chrome.',
    copyLink:'\u0646\u0633\u062E \u0627\u0644\u0631\u0627\u0628\u0637', copied:'\u062A\u0645 \u0627\u0644\u0646\u0633\u062E! \u0627\u0641\u062A\u062D Chrome.', copyFailed:'\u0641\u0634\u0644 \u0627\u0644\u0646\u0633\u062E. \u0627\u0643\u062A\u0628: 192.168.4.1', close:'\u0625\u063A\u0644\u0627\u0642',
    language:'\u0627\u0644\u0644\u063A\u0629', selectLang:'\u0627\u062E\u062A\u064A\u0627\u0631 \u0627\u0644\u0644\u063A\u0629', chooseLang:'\u0627\u062E\u062A\u0631 \u0644\u063A\u062A\u0643 \u0627\u0644\u0645\u0641\u0636\u0644\u0629', continue:'\u0645\u062A\u0627\u0628\u0639\u0629'
  }
};

const LANG_ORDER = ['en','es','fr','ru','zh-CN','zh-TW','fa','ar'];
let currentLang = 'en';

function applyLanguage(code) {
  const L = LANG[code];
  if (!L) return;
  currentLang = code;
  document.documentElement.lang = code;
  document.documentElement.dir = L.dir;
  // Splash
  const splashText = document.getElementById('splashText');
  if (splashText) splashText.textContent = L.splash;
  // Main UI
  const setText = (id, text) => { const el = document.getElementById(id); if (el) el.textContent = text; };
  setText('lblMessage', L.message);
  setText('lblMaxChars', L.maxChars);
  setText('lblHint', L.hint);
  setText('lblMode', L.mode);
  setText('lblIntensity', L.intensity);
  setText('lblColor', L.color);
  setText('lblDazzle', L.dazzle);
  setText('footerText', L.footer);
  // OTA Modal
  setText('otaTitle', L.fwUpdate);
  setText('otaCurrent', L.current);
  setText('otaChooseLabel', L.chooseBin);
  setText('otaUpload', L.uploadFw);
  setText('otaClose', L.close);
  setText('lblLanguage', L.language);
  // Android Dialog
  setText('androidTitle', L.openBrowser);
  setText('androidDesc', L.androidDesc);
  setText('copyLinkBtn', L.copyLink);
  setText('androidClose', L.close);
  // Language Modal
  setText('langTitle', L.selectLang);
  setText('langSubtitle', L.chooseLang);
  setText('langConfirm', L.continue);
  // Update language dropdown
  const langSelect = document.getElementById('langSelect');
  if (langSelect) langSelect.value = code;
  // Store locally
  try { if (window.localStorage) localStorage.setItem(LANG_KEY, code); } catch(e){}
}

function buildLangGrid() {
  const grid = document.getElementById('langGrid');
  if (!grid) return;
  grid.innerHTML = '';
  LANG_ORDER.forEach(code => {
    const L = LANG[code];
    const btn = document.createElement('button');
    btn.className = 'lang-btn';
    btn.dataset.lang = code;
    btn.innerHTML = L.name + '<span class="lang-native">' + L.native + '</span>';
    btn.onclick = () => {
      grid.querySelectorAll('.lang-btn').forEach(b => b.classList.remove('selected'));
      btn.classList.add('selected');
      document.getElementById('langConfirm').disabled = false;
    };
    grid.appendChild(btn);
  });
}

function buildLangSelect() {
  const sel = document.getElementById('langSelect');
  if (!sel) return;
  sel.innerHTML = '';
  LANG_ORDER.forEach(code => {
    const L = LANG[code];
    const opt = document.createElement('option');
    opt.value = code;
    opt.textContent = L.name + ' (' + L.native + ')';
    sel.appendChild(opt);
  });
  sel.value = currentLang;
  sel.onchange = async () => {
    const prev = currentLang;
    const result = await post('/api/language', { lang: sel.value });
    if (result && result.ok) {
      applyLanguage(sel.value);
    } else {
      sel.value = prev;  // Revert dropdown on failure
    }
  };
}

function calcTxTime(msg,wpm){const dit=1200/wpm;let ms=0,i=0;while(i<msg.length){const c=msg[i].toUpperCase();if(c==='<'){const cl=msg.indexOf('>',i);if(cl>i+1&&cl<=i+4){const tag=msg.substring(i+1,cl).toUpperCase(),code=PROSIGNS[tag];if(code){for(const s of code)ms+=(s==='.'?dit:3*dit)+dit;ms+=2*dit;i=cl+1;continue;}}}if(c===' '){ms+=7*dit;i++;continue;}const code=MORSE[c];if(code){for(const s of code)ms+=(s==='.'?dit:3*dit)+dit;ms+=2*dit;}i++;}const secs=Math.ceil(ms/1000),mm=String(Math.floor(secs/60)).padStart(2,'0'),ss=String(secs%60).padStart(2,'0');return mm+':'+ss;}
const ui = {
  modes: document.querySelectorAll('#modeGroup button'),
  ints: document.querySelectorAll('#intensityGroup button'),
  colors: document.querySelectorAll('#colorGroup .color'),
  dazzle: document.getElementById('dazzle'),
  wpm: document.getElementById('wpm'),
  wpmDisplay: document.getElementById('wpmDisplay'),
  msg: document.getElementById('msg'),
  play: document.getElementById('play'),
  stop: document.getElementById('stop'),
  status: document.getElementById('status'),
  colorCard: document.getElementById('colorCard'),
  dazzleCard: document.getElementById('dazzleCard'),
  wheelCanvas: document.getElementById('wheelCanvas'),
  pickerWrap: document.getElementById('pickerWrap'),
  customSwatch: document.getElementById('customSwatch'),
  txTime: document.getElementById('txTime'),
  version: document.getElementById('version')
};

async function post(url, body) {
  try {
    const r = await fetch(url, { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body) });
    return r.json();
  } catch(e) { console.error(e); }
}

let wheelHue=0;
let msgDirty=false;
function hslToRgb(h,s,l){s/=100;l/=100;const k=n=>(n+h/30)%12;const a=s*Math.min(l,1-l);const f=n=>l-a*Math.max(-1,Math.min(k(n)-3,Math.min(9-k(n),1)));return{r:Math.round(f(0)*255),g:Math.round(f(8)*255),b:Math.round(f(4)*255)};}
function drawWheel(){const c=ui.wheelCanvas,ctx=c.getContext('2d'),cx=c.width/2,cy=c.height/2,r=cx-2;for(let a=0;a<360;a++){ctx.beginPath();ctx.moveTo(cx,cy);ctx.arc(cx,cy,r,a*Math.PI/180,(a+2)*Math.PI/180);ctx.closePath();ctx.fillStyle='hsl('+a+',100%,50%)';ctx.fill();}}
function pickFromWheel(e){const c=ui.wheelCanvas,rect=c.getBoundingClientRect(),x=e.clientX-rect.left-c.width/2,y=e.clientY-rect.top-c.height/2,d=Math.sqrt(x*x+y*y);if(d>c.width/2)return null;let a=Math.atan2(y,x)*180/Math.PI;if(a<0)a+=360;return Math.round(a);}

let langInitDone = false;

async function getState() {
  try {
    const r = await fetch('/api/state');
    const s = await r.json();
    ui.modes.forEach(b => b.classList.toggle('active', (s.mode===0 && b.dataset.mode==='DISCREET') || (s.mode===1 && b.dataset.mode==='VISIBLE')));
    ui.ints.forEach(b => b.classList.toggle('active', b.dataset.int === s.intensity));
    ui.colors.forEach(c => c.classList.toggle('active', c.dataset.color === s.color));
    ui.dazzle.checked = s.dazzle;

    if (s.wpm) {
      ui.wpm.value = s.wpm;
      ui.wpmDisplay.textContent = s.wpm;
    }

    // Update custom color swatch
    if (s.customR !== undefined) {
      const rgb = 'rgb('+s.customR+','+s.customG+','+s.customB+')';
      ui.customSwatch.style.background = rgb;
    }

    // Show/hide picker based on custom selection
    ui.pickerWrap.classList.toggle('visible', s.color === 'CUSTOM');

    if (s.text && !msgDirty) ui.msg.value = s.text;

    // Apply language from server
    if (!langInitDone && s.lang) {
      langInitDone = true;
      applyLanguage(s.lang);
      buildLangGrid();
      buildLangSelect();

      // Show language selection modal on first boot
      if (s.firstBoot) {
        const langModal = document.getElementById('langModal');
        if (langModal) langModal.classList.add('visible');
      }
    }

    const L = LANG[currentLang];
    ui.status.textContent = s.playing ? L.playing : L.idle;
    ui.play.classList.toggle('active', s.playing);
    ui.stop.classList.toggle('active', !s.playing);
    if (s.version) ui.version.textContent = L.firmware + ' v' + s.version;
    updateTxTime();

    const visMode = s.mode === 1;
    ui.colorCard.style.opacity = visMode ? 1 : 0.4;
    ui.dazzleCard.style.opacity = visMode ? 1 : 0.4;
  } catch(e) { console.error(e); }
}

ui.modes.forEach(b => b.addEventListener('click', async ()=>{
  ui.modes.forEach(x=>x.classList.remove('active')); b.classList.add('active');
  await post('/api/update', { mode: b.dataset.mode });
}));
ui.ints.forEach(b => b.addEventListener('click', async ()=>{
  ui.ints.forEach(x=>x.classList.remove('active')); b.classList.add('active');
  await post('/api/update', { intensity: b.dataset.int });
}));
ui.colors.forEach(c => c.addEventListener('click', async ()=>{
  ui.colors.forEach(x=>x.classList.remove('active')); c.classList.add('active');
  const isCustom = c.dataset.color === 'CUSTOM';
  ui.pickerWrap.classList.toggle('visible', isCustom);
  if(isCustom)drawWheel();
  await post('/api/update', { color: c.dataset.color });
}));
ui.wheelCanvas.addEventListener('click', async (e)=>{
  const h=pickFromWheel(e);if(h===null)return;wheelHue=h;
  const rgb=hslToRgb(wheelHue,100,50);
  ui.customSwatch.style.background='rgb('+rgb.r+','+rgb.g+','+rgb.b+')';
  await post('/api/update',{customR:rgb.r,customG:rgb.g,customB:rgb.b});
});
ui.dazzle.addEventListener('change', async ()=>{ await post('/api/update', { dazzle: ui.dazzle.checked }); });

function updateTxTime(){
  const wpm=parseInt(ui.wpm.value)||10;
  const msg=ui.msg.value||'';
  ui.txTime.textContent=msg.length>0?'\u23F1 '+calcTxTime(msg,wpm):'';
}

ui.wpm.addEventListener('input', ()=>{
  ui.wpmDisplay.textContent = ui.wpm.value;
  updateTxTime();
});
ui.wpm.addEventListener('change', async ()=>{
  await post('/api/update', { wpm: parseInt(ui.wpm.value) });
});

ui.msg.addEventListener('input', ()=>{ msgDirty=true; updateTxTime(); });

ui.play.addEventListener('click', async ()=>{ const wpm=parseInt(ui.wpm.value)||10; ui.txTime.textContent='\u23F1 '+calcTxTime(ui.msg.value||'',wpm); await post('/api/play', { text: ui.msg.value || '' }); msgDirty=false; ui.status.textContent = LANG[currentLang].playing; ui.play.classList.add('active'); ui.stop.classList.remove('active'); });
ui.stop.addEventListener('click', async ()=>{ await post('/api/stop'); ui.status.textContent = LANG[currentLang].idle; ui.stop.classList.add('active'); ui.play.classList.remove('active'); });

// Language modal confirm
document.getElementById('langConfirm').addEventListener('click', async () => {
  const selected = document.querySelector('#langGrid .lang-btn.selected');
  if (!selected) return;
  const code = selected.dataset.lang;
  applyLanguage(code);
  await post('/api/language', { lang: code });
  document.getElementById('langModal').classList.remove('visible');
});

function setupSplash(){
  const splash = document.getElementById('splash');
  if (!splash) return;

  try {
    if (window.localStorage && localStorage.getItem(SPLASH_KEY) === '1') {
      splash.remove();
      return;
    }
  } catch(e){}

  const img = splash.querySelector('img');
  let shown = false;

  const showSplash = () => {
    if (shown) return;
    shown = true;
    splash.classList.add('visible');
  };

  const hide = () => {
    splash.classList.remove('visible');
    try { if (window.localStorage) localStorage.setItem(SPLASH_KEY, '1'); } catch(e){}
    setTimeout(() => splash.remove(), 300);
  };

  // If image already cached/loaded
  if (img.complete && img.naturalHeight > 0) {
    showSplash();
  } else {
    img.onload = showSplash;
    img.onerror = () => splash.remove();
  }

  // Timeout fallback - skip if taking too long
  setTimeout(() => { if (!shown) splash.remove(); }, 3000);

  splash.addEventListener('click', hide);
  setTimeout(hide, 8000);
}

setupSplash();
getState();
setInterval(getState, 2000);

// OTA Firmware Update
const isAndroidCaptive = /android/i.test(navigator.userAgent) && /captiveportal|captive_portal|minibroswer|wv\)/i.test(navigator.userAgent);
const otaModal = document.getElementById('otaModal');
const androidDialog = document.getElementById('androidDialog');
const otaVersion = document.getElementById('otaVersion');
const otaFile = document.getElementById('otaFile');
const otaFileName = document.getElementById('otaFileName');
const otaUpload = document.getElementById('otaUpload');
const otaProgress = document.getElementById('otaProgress');
const otaStatus = document.getElementById('otaStatus');
const otaClose = document.getElementById('otaClose');
const otaGetCode = document.getElementById('otaGetCode');
const otaCodeSection = document.getElementById('otaCodeSection');
const otaD1 = document.getElementById('otaD1');
const otaD2 = document.getElementById('otaD2');
const otaD3 = document.getElementById('otaD3');
const otaD4 = document.getElementById('otaD4');

document.getElementById('settingsBtn').addEventListener('click', () => {
  const L = LANG[currentLang];
  if (isAndroidCaptive) {
    androidDialog.classList.add('visible');
    document.getElementById('copyStatus').textContent = '';
  } else {
    otaModal.classList.add('visible');
    otaVersion.textContent = ui.version.textContent.replace(L.firmware + ' v', '') || '-';
    otaStatus.textContent = '';
    otaProgress.style.display = 'none';
    otaProgress.value = 0;
    otaFileName.textContent = L.noFile;
    otaFileName.style.color = '#888';
    otaCodeSection.style.display = 'none';
    otaD1.value = ''; otaD2.value = ''; otaD3.value = ''; otaD4.value = '';
    buildLangSelect();
  }
});

otaClose.addEventListener('click', () => {
  otaModal.classList.remove('visible');
});

// OTA Code - Get code from device (displays on LED)
otaGetCode.addEventListener('click', async () => {
  const L = LANG[currentLang];
  otaStatus.textContent = 'Watch the LED...';
  otaStatus.style.color = '#8AB4F8';
  otaGetCode.disabled = true;
  try {
    const resp = await fetch('/api/ota-code', {method:'POST'});
    if (resp.ok) {
      otaCodeSection.style.display = 'block';
      otaStatus.textContent = 'Enter the code shown on LED';
      otaD1.value = ''; otaD2.value = ''; otaD3.value = ''; otaD4.value = '';
      otaD1.focus();
      otaUpload.disabled = false;
    } else {
      const data = await resp.json();
      otaStatus.textContent = data.error || 'Failed to get code';
      otaStatus.style.color = '#f44336';
    }
  } catch(e) {
    otaStatus.textContent = 'Network error';
    otaStatus.style.color = '#f44336';
  }
  otaGetCode.disabled = false;
});

// Auto-advance to next digit field
otaD1.addEventListener('input', () => { if (otaD1.value.length >= 1) otaD2.focus(); });
otaD2.addEventListener('input', () => { if (otaD2.value.length >= 1) otaD3.focus(); });
otaD3.addEventListener('input', () => { if (otaD3.value.length >= 1) otaD4.focus(); });

// Android dialog handlers
document.getElementById('androidClose').addEventListener('click', () => {
  androidDialog.classList.remove('visible');
});

document.getElementById('copyLinkBtn').addEventListener('click', () => {
  const L = LANG[currentLang];
  const url = 'http://192.168.4.1';
  const status = document.getElementById('copyStatus');

  // Use textarea + execCommand for better compatibility
  const textarea = document.createElement('textarea');
  textarea.value = url;
  textarea.style.position = 'fixed';
  textarea.style.opacity = '0';
  document.body.appendChild(textarea);
  textarea.select();

  try {
    document.execCommand('copy');
    status.textContent = L.copied;
    status.style.color = '#4CAF50';
  } catch (e) {
    status.textContent = L.copyFailed;
    status.style.color = '#ff9800';
  }

  document.body.removeChild(textarea);
});

otaModal.addEventListener('click', (e) => {
  if (e.target === otaModal) otaModal.classList.remove('visible');
});

otaFile.addEventListener('change', () => {
  const L = LANG[currentLang];
  if (otaFile.files.length > 0) {
    const name = otaFile.files[0].name;
    if (!name.toLowerCase().endsWith('.bin')) {
      otaFileName.textContent = L.errBin;
      otaFileName.style.color = '#f44336';
      otaFile.value = '';
      return;
    }
    otaFileName.textContent = name;
    otaFileName.style.color = '#8AB4F8';
  } else {
    otaFileName.textContent = L.noFile;
    otaFileName.style.color = '#888';
  }
});

otaUpload.addEventListener('click', async () => {
  const L = LANG[currentLang];
  const file = otaFile.files[0];

  if (!file) {
    otaStatus.textContent = L.selectBin;
    otaStatus.style.color = '#ff9800';
    return;
  }

  // Validate OTA code (must be 4 digits, each 1-9)
  const code = '' + (otaD1.value||'') + (otaD2.value||'') + (otaD3.value||'') + (otaD4.value||'');
  if (code.length !== 4 || !/^[1-9]{4}$/.test(code)) {
    otaStatus.textContent = 'Enter 4-digit code first (click Get Code)';
    otaStatus.style.color = '#ff9800';
    return;
  }

  otaUpload.disabled = true;
  otaStatus.textContent = L.uploading;
  otaStatus.style.color = '#8AB4F8';
  otaProgress.style.display = 'block';
  otaProgress.value = 0;

  const formData = new FormData();
  formData.append('firmware', file);

  const xhr = new XMLHttpRequest();
  xhr.upload.addEventListener('progress', (e) => {
    if (e.lengthComputable) {
      otaProgress.value = (e.loaded / e.total) * 100;
      otaStatus.textContent = Math.round(otaProgress.value) + '%';
    }
  });

  xhr.addEventListener('load', () => {
    if (xhr.status === 200) {
      otaStatus.textContent = L.success;
      otaStatus.style.color = '#4CAF50';
      setTimeout(() => location.reload(), 5000);
    } else {
      // Parse error message from response if available
      let msg = L.updateFailed;
      try {
        const resp = JSON.parse(xhr.responseText);
        if (resp.error) msg = resp.error;
        if (resp.lockedOut) {
          msg = 'Locked out - click Get Code for a new code';
          otaUpload.disabled = true;
        }
        if (resp.retryAfterMs) {
          const secs = Math.ceil(resp.retryAfterMs / 1000);
          msg += ' (retry in ' + secs + 's)';
          otaUpload.disabled = true;
          setTimeout(() => { otaUpload.disabled = false; }, resp.retryAfterMs);
        }
      } catch(e) {}
      otaStatus.textContent = msg;
      otaStatus.style.color = '#f44336';
      if (!otaUpload.disabled) otaUpload.disabled = false;
    }
  });

  xhr.addEventListener('error', () => {
    otaStatus.textContent = L.netError;
    otaStatus.style.color = '#f44336';
    otaUpload.disabled = false;
  });

  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('X-OTA-Code', code);
  xhr.send(formData);
});
</script>
</body></html>
)====";

// -------------------- Help Page HTML ----------------------
static const char HELP_HTML[] PROGMEM = R"====(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Morse Reference</title>
<style>
:root{--bg:#121212;--card:#1E1E1E;--text:#ECECEC}
*{box-sizing:border-box;font-family:system-ui,sans-serif;margin:0;padding:0}
body{background:var(--bg);color:var(--text);padding:16px;max-width:480px;margin:0 auto}
h1{font-size:18px;margin-bottom:16px;text-align:center}
h2{font-size:14px;color:#888;margin:16px 0 8px;border-bottom:1px solid #333;padding-bottom:4px}
.back{display:block;text-align:center;color:#8AB4F8;margin-bottom:16px}
table{width:100%;border-collapse:collapse;font-size:12px}
td,th{padding:6px 8px;border:1px solid #333;text-align:left}
th{background:#222;color:#888;font-weight:600}
.code{font-family:monospace;letter-spacing:2px}
.timing{background:#1a1a1a;padding:12px;border-radius:8px;font-family:monospace;font-size:11px;line-height:1.8}
.dot{color:#8AB4F8}
.dash{color:#8AB4F8}
.gap{color:#666}
[dir="rtl"]{direction:rtl}
[dir="rtl"] td,[dir="rtl"] th{text-align:right}
[dir="rtl"] .code{text-align:left;direction:ltr}
</style>
</head>
<body>
<a href="/" class="back" id="backLink">\u2190 Back to Control</a>
<h1 id="helpTitle">Morse Code Reference</h1>

<h2>Letters</h2>
<table>
<tr><th>Char</th><th>Code</th><th>Char</th><th>Code</th><th>Char</th><th>Code</th></tr>
<tr><td>A</td><td class="code">·−</td><td>J</td><td class="code">·−−−</td><td>S</td><td class="code">···</td></tr>
<tr><td>B</td><td class="code">−···</td><td>K</td><td class="code">−·−</td><td>T</td><td class="code">−</td></tr>
<tr><td>C</td><td class="code">−·−·</td><td>L</td><td class="code">·−··</td><td>U</td><td class="code">··−</td></tr>
<tr><td>D</td><td class="code">−··</td><td>M</td><td class="code">−−</td><td>V</td><td class="code">···−</td></tr>
<tr><td>E</td><td class="code">·</td><td>N</td><td class="code">−·</td><td>W</td><td class="code">·−−</td></tr>
<tr><td>F</td><td class="code">··−·</td><td>O</td><td class="code">−−−</td><td>X</td><td class="code">−··−</td></tr>
<tr><td>G</td><td class="code">−−·</td><td>P</td><td class="code">·−−·</td><td>Y</td><td class="code">−·−−</td></tr>
<tr><td>H</td><td class="code">····</td><td>Q</td><td class="code">−−·−</td><td>Z</td><td class="code">−−··</td></tr>
<tr><td>I</td><td class="code">··</td><td>R</td><td class="code">·−·</td><td></td><td></td></tr>
</table>

<h2>Numbers</h2>
<table>
<tr><th>Char</th><th>Code</th><th>Char</th><th>Code</th></tr>
<tr><td>0</td><td class="code">−−−−−</td><td>5</td><td class="code">·····</td></tr>
<tr><td>1</td><td class="code">·−−−−</td><td>6</td><td class="code">−····</td></tr>
<tr><td>2</td><td class="code">··−−−</td><td>7</td><td class="code">−−···</td></tr>
<tr><td>3</td><td class="code">···−−</td><td>8</td><td class="code">−−−··</td></tr>
<tr><td>4</td><td class="code">····−</td><td>9</td><td class="code">−−−−·</td></tr>
</table>

<h2>Punctuation</h2>
<table>
<tr><th>Char</th><th>Code</th><th>Char</th><th>Code</th></tr>
<tr><td>.</td><td class="code">·−·−·−</td><td>-</td><td class="code">−····−</td></tr>
<tr><td>,</td><td class="code">−−··−−</td><td>(</td><td class="code">−·−−·</td></tr>
<tr><td>?</td><td class="code">··−−··</td><td>)</td><td class="code">−·−−·−</td></tr>
<tr><td>/</td><td class="code">−··−·</td><td>@</td><td class="code">·−−·−·</td></tr>
<tr><td>=</td><td class="code">−···−</td><td></td><td></td></tr>
</table>

<h2>Prosigns</h2>
<p style="font-size:11px;color:#888;margin-bottom:8px">Type with angle brackets: &lt;KA&gt; &lt;AR&gt;</p>
<table>
<tr><th>Tag</th><th>Code</th><th>Meaning</th></tr>
<tr><td>&lt;KA&gt;</td><td class="code">−·−·−</td><td>Starting signal</td></tr>
<tr><td>&lt;AR&gt;</td><td class="code">·−·−·</td><td>End of message</td></tr>
</table>

<h2>Timing</h2>
<div class="timing">
<span class="dot">·</span> Dot = 1 unit<br>
<span class="dash">−</span> Dash = 3 units<br>
<span class="gap">|</span> Gap (intra-char) = 1 unit<br>
<span class="gap">| | |</span> Gap (between letters) = 3 units<br>
<span class="gap">| | | | | | |</span> Gap (between words) = 7 units<br><br>
At 10 WPM: 1 unit = 120ms
</div>

<script>
const HELP_LANG = {
  en: { back:'\u2190 Back to Control', title:'Morse Code Reference' },
  es: { back:'\u2190 Volver al Control', title:'Referencia de C\u00F3digo Morse' },
  fr: { back:'\u2190 Retour au Contr\u00F4le', title:'R\u00E9f\u00E9rence Code Morse' },
  ru: { back:'\u2190 \u041D\u0430\u0437\u0430\u0434', title:'\u0421\u043F\u0440\u0430\u0432\u043E\u0447\u043D\u0438\u043A \u041C\u043E\u0440\u0437\u0435' },
  'zh-CN': { back:'\u2190 \u8FD4\u56DE\u63A7\u5236', title:'\u6469\u5C14\u65AF\u7535\u7801\u53C2\u8003' },
  'zh-TW': { back:'\u2190 \u8FD4\u56DE\u63A7\u5236', title:'\u6469\u723E\u65AF\u96FB\u78BC\u53C3\u8003' },
  fa: { back:'\u2192 \u0628\u0627\u0632\u06AF\u0634\u062A', title:'\u0645\u0631\u062C\u0639 \u06A9\u062F \u0645\u0648\u0631\u0633' },
  ar: { back:'\u2192 \u0639\u0648\u062F\u0629', title:'\u0645\u0631\u062C\u0639 \u0634\u0641\u0631\u0629 \u0645\u0648\u0631\u0633' }
};
const RTL_LANGS = ['fa','ar'];
try {
  const lang = localStorage.getItem('signet_lang') || 'en';
  const L = HELP_LANG[lang] || HELP_LANG.en;
  document.getElementById('backLink').textContent = L.back;
  document.getElementById('helpTitle').textContent = L.title;
  if (RTL_LANGS.includes(lang)) {
    document.documentElement.dir = 'rtl';
    document.documentElement.lang = lang;
  }
} catch(e){}
</script>
</body></html>
)====";

// -------------------- Captive Portal helpers --------------
// Validates that a string is a properly formatted IPv4 address (e.g., "192.168.4.1")
// Rejects: empty strings, wrong octet counts, values > 255, leading zeros, non-numeric chars
bool isIpAddress(const String& str) {
  if (str.length() == 0 || str.length() > 15) return false;

  int dots = 0;
  size_t octetStart = 0;

  for (size_t i = 0; i <= str.length(); i++) {
    if (i == str.length() || str[i] == '.') {
      // Check for empty octet (consecutive dots or leading/trailing dot)
      if (i == octetStart) return false;

      // Extract and validate octet
      size_t octetLen = i - octetStart;
      if (octetLen > 3) return false;

      // Parse octet value
      int val = 0;
      for (size_t j = octetStart; j < i; j++) {
        val = val * 10 + (str[j] - '0');
      }
      if (val > 255) return false;

      // Reject leading zeros (e.g., "01", "001") except for "0" itself
      if (octetLen > 1 && str[octetStart] == '0') return false;

      octetStart = i + 1;
      if (i < str.length()) dots++;
    } else if (str[i] < '0' || str[i] > '9') {
      // Non-numeric character
      return false;
    }
  }

  // Must have exactly 3 dots (4 octets)
  return dots == 3;
}

bool captivePortal() {
  if (!isIpAddress(server.hostHeader())) {
    noteActivity();
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
    return true;
  }
  return false;
}

// -------------------- API helpers -------------------------
String intensityToStr(Intensity i){
  switch(i){
    case I_LOW:  return "LOW";
    case I_MED:  return "MED";
    case I_HIGH: return "HIGH";
    default:     return "MED";
  }
}

Intensity strToIntensity(const String& s){
  if(s=="LOW")  return I_LOW;
  if(s=="HIGH") return I_HIGH;
  return I_MED;
}

String colorToStr(ColorSel c){
  switch(c){
    case C_RED:    return "RED";
    case C_GREEN:  return "GREEN";
    case C_BLUE:   return "BLUE";
    case C_CUSTOM: return "CUSTOM";
    default:       return "RED";
  }
}

ColorSel strToColor(const String& s){
  if(s=="GREEN")  return C_GREEN;
  if(s=="BLUE")   return C_BLUE;
  if(s=="CUSTOM") return C_CUSTOM;
  return C_RED;
}

void sendIndex(){
  noteActivity();
  // First time UI is served: confirm connection with 4 rapid blue blinks
  if (!uiServed) {
    uiServed = true;
    connectionConfirmBlink();
  }
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

void sendHelpPage(){
  if (captivePortal()) return;
  noteActivity();
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send(200, "text/html; charset=utf-8", HELP_HTML);
}

void handleState(){
  if (captivePortal()) return;
  noteActivity();
  StaticJsonDocument<512> doc;
  doc["mode"]     = (int)state.mode;
  doc["intensity"]= intensityToStr(state.intensity);
  doc["color"]    = colorToStr(state.color);
  doc["customR"]  = state.customR;
  doc["customG"]  = state.customG;
  doc["customB"]  = state.customB;
  doc["dazzle"]   = state.dazzle;
  doc["wpm"]      = state.wpm;
  // FIX: Thread-safe access to text
  doc["text"]     = getTextCopy();
  doc["playing"]  = state.playing.load();  // Explicit load for ArduinoJson compatibility
  doc["version"]  = FIRMWARE_VERSION;
  // Language info for UI localization
  doc["lang"]     = LANG_CODES[state.language];
  doc["firstBoot"]= true;  // Always show language modal (stateless design)
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleUpdate(){
  noteActivity();
  if (!checkMacLock()) {
    server.send(403, "application/json", "{\"error\":\"access denied\"}");
    return;
  }
  // Payload size limit (defense-in-depth against malformed input)
  if (server.arg("plain").length() > 512) {
    server.send(413, "application/json", "{\"error\":\"payload too large\"}");
    return;
  }
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  if (doc.containsKey("mode")) {
    String m = doc["mode"].as<String>();
    state.mode = (m == "VISIBLE") ? VISIBLE : DISCREET;
    allOff();
  }
  if (doc.containsKey("intensity")) {
    state.intensity = strToIntensity(doc["intensity"].as<String>());
  }
  if (doc.containsKey("color"))     state.color     = strToColor(doc["color"].as<String>());
  if (doc.containsKey("customR"))   state.customR   = (uint8_t)constrain(doc["customR"].as<int>(), 0, 255);
  if (doc.containsKey("customG"))   state.customG   = (uint8_t)constrain(doc["customG"].as<int>(), 0, 255);
  if (doc.containsKey("customB"))   state.customB   = (uint8_t)constrain(doc["customB"].as<int>(), 0, 255);
  if (doc.containsKey("dazzle"))    state.dazzle    = doc["dazzle"].as<bool>();
  if (doc.containsKey("wpm")) {
    int newWpm = doc["wpm"].as<int>();
    if (newWpm < 5) newWpm = 5;
    if (newWpm > 20) newWpm = 20;
    state.wpm = (uint8_t)newWpm;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handlePlay(){
  noteActivity();
  if (!checkMacLock()) {
    server.send(403, "application/json", "{\"error\":\"access denied\"}");
    return;
  }
  // Payload size limit (defense-in-depth against malformed input)
  if (server.arg("plain").length() > 512) {
    server.send(413, "application/json", "{\"error\":\"payload too large\"}");
    return;
  }
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, server.arg("plain"));
  if (!err && doc.containsKey("text")) {
    String newText = doc["text"].as<String>();
    if (newText.length() > 200) newText = newText.substring(0, 200);
    if (newText.length() > 0) {
      setTextSafe(newText);
      state.playing = true;
      server.send(200, "application/json", "{\"playing\":true}");
      return;
    }
  }
  server.send(400, "application/json", "{\"error\":\"missing or empty text\"}");
}

void handleStop(){
  noteActivity();
  if (!checkMacLock()) {
    server.send(403, "application/json", "{\"error\":\"access denied\"}");
    return;
  }
  state.playing = false;
  allOff();
  server.send(200, "application/json", "{\"playing\":false}");
}

// -------------------- Language Selection --------------------
void handleLanguageSet(){
  noteActivity();
  if (!checkMacLock()) {
    server.send(403, "application/json", "{\"error\":\"access denied\"}");
    return;
  }
  // Payload size limit (defense-in-depth against malformed input)
  if (server.arg("plain").length() > 256) {
    server.send(413, "application/json", "{\"error\":\"payload too large\"}");
    return;
  }
  StaticJsonDocument<128> doc;
  auto err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }

  if (!doc.containsKey("lang")) {
    server.send(400, "application/json", "{\"error\":\"missing lang field\"}");
    return;
  }

  String langCode = doc["lang"].as<String>();

  // Find language code index
  int langIndex = -1;
  for (uint8_t i = 0; i < LANG_COUNT; i++) {
    if (langCode == LANG_CODES[i]) {
      langIndex = i;
      break;
    }
  }

  if (langIndex < 0) {
    server.send(400, "application/json", "{\"error\":\"invalid language code\"}");
    return;
  }

  // Update state (RAM only - stateless design, no NVS persistence)
  state.language = (uint8_t)langIndex;
  Serial.printf("Language changed to: %s\n", LANG_CODES[state.language]);

  server.send(200, "application/json", "{\"ok\":true}");
}

// -------------------- OTA Firmware Update --------------------
// OTA State Management
static SemaphoreHandle_t otaMutex = nullptr;
static bool otaInProgress = false;
static bool otaHadError = false;
static bool otaHeaderChecked = false;  // For firmware magic byte validation
static size_t otaExpectedSize = 0;     // Content-Length from header
static size_t otaReceivedSize = 0;     // Actual bytes received
static String otaErrorMessage = "";

// Max firmware size (from partition table: 0x140000 = 1,310,720 bytes)
const size_t OTA_MAX_SIZE = 0x140000;

// Handler to generate and display OTA code via LED blinks
// Acquires otaMutex to protect otaCode[] and rate limiting state
void handleOtaCode() {
  noteActivity();

  // Check MAC lock
  if (!checkMacLock()) {
    server.send(403, "application/json", "{\"error\":\"access denied\"}");
    return;
  }

  // Acquire mutex to protect code generation and rate limit reset
  if (!otaMutex || xSemaphoreTake(otaMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }

  // Generate new code and reset rate limiting (mutex held)
  generateOtaCode();  // Note: called only from here, always under otaMutex
  otaFailCount = 0;
  otaLastFailTime = 0;
  otaHardLocked = false;
  otaRetryAfterMs = 0;

  // Copy code before releasing mutex (avoids holding mutex during ~16s LED display)
  uint8_t codeCopy[OTA_CODE_LEN];
  memcpy(codeCopy, otaCode, OTA_CODE_LEN);

  xSemaphoreGive(otaMutex);

  // Display code on LED using local copy (no mutex needed)
  displayOtaCodeOnLed(codeCopy);

  // Return success (code is NOT sent in response - must be read from LED)
  server.send(200, "application/json", "{\"ready\":true}");
}

void handleOtaUpload() {
  HTTPUpload& upload = server.upload();

  // Keep AP alive during upload (prevents timeout during slow uploads)
  noteActivity();

  if (upload.status == UPLOAD_FILE_START) {
    // Check MAC lock first
    if (!checkMacLock()) {
      otaHadError = true;
      otaErrorMessage = "Access denied";
      return;
    }

    // Try to acquire mutex for exclusive OTA access (prevents concurrent uploads)
    if (!otaMutex || xSemaphoreTake(otaMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      otaHadError = true;
      otaErrorMessage = "Update already in progress";
      OTA_LOGLN("[OTA] Rejected: another update in progress");
      return;
    }

    // Hard lockout check
    if (otaHardLocked) {
      otaHadError = true;
      otaErrorMessage = "Locked out - generate a new code";
      OTA_LOGLN("[OTA] Rejected: hard lockout active");
      xSemaphoreGive(otaMutex);
      return;
    }

    // Exponential backoff check
    if (otaFailCount > 0) {
      unsigned long backoffMs = OTA_BACKOFF_BASE_MS << (otaFailCount - 1);
      unsigned long elapsed = millis() - otaLastFailTime;
      if (elapsed < backoffMs) {
        otaRetryAfterMs = backoffMs - elapsed;
        otaHadError = true;
        otaErrorMessage = "Rate limited - try again shortly";
        OTA_LOG("[OTA] Rejected: rate limited, retry after %lu ms\n", otaRetryAfterMs);
        xSemaphoreGive(otaMutex);
        return;
      }
    }

    // Verify OTA combination code (displayed on LED via /api/ota-code)
    String codeHeader = server.header("X-OTA-Code");
    if (codeHeader.length() != OTA_CODE_LEN) {
      otaHadError = true;
      otaErrorMessage = "Missing OTA code - click Get Code first";
      OTA_LOGLN("[OTA] Rejected: missing or invalid OTA code header");
      xSemaphoreGive(otaMutex);
      return;
    }
    // Check code expiry before verification (distinct error message)
    if (otaCodeValid && (millis() - otaCodeGeneratedAt) > OTA_CODE_TTL_MS) {
      otaCodeValid = false;
      memset(otaCode, 0, OTA_CODE_LEN);
      otaHadError = true;
      otaErrorMessage = "Code expired - click Get Code for a new one";
      OTA_LOGLN("[OTA] Rejected: code expired");
      xSemaphoreGive(otaMutex);
      return;
    }

    uint8_t d1 = codeHeader[0] - '0';
    uint8_t d2 = codeHeader[1] - '0';
    uint8_t d3 = codeHeader[2] - '0';
    uint8_t d4 = codeHeader[3] - '0';
    if (!verifyOtaCode(d1, d2, d3, d4)) {
      otaFailCount++;
      otaLastFailTime = millis();
      otaRetryAfterMs = 0;

      if (otaFailCount >= OTA_MAX_ATTEMPTS) {
        otaHardLocked = true;
        otaCodeValid = false;
        memset(otaCode, 0, OTA_CODE_LEN);
        otaHadError = true;
        otaErrorMessage = "Locked out - too many failed attempts";
        OTA_LOG("[OTA] Hard lockout after %d failed attempts\n", otaFailCount);
      } else {
        unsigned long nextBackoff = OTA_BACKOFF_BASE_MS << (otaFailCount - 1);
        otaRetryAfterMs = nextBackoff;
        otaHadError = true;
        otaErrorMessage = "Invalid OTA code";
        OTA_LOG("[OTA] Rejected: wrong code (attempt %d/%d)\n", otaFailCount, OTA_MAX_ATTEMPTS);
      }
      xSemaphoreGive(otaMutex);
      return;
    }
    // Successful verification - reset fail count
    otaFailCount = 0;
    otaRetryAfterMs = 0;

    // Reset error state for new upload
    otaHadError = false;
    otaHeaderChecked = false;
    otaErrorMessage = "";

    // Firmware size pre-check
    size_t contentLength = 0;
    if (server.hasHeader("Content-Length")) {
      contentLength = server.header("Content-Length").toInt();
    }
    if (contentLength > OTA_MAX_SIZE) {
      otaHadError = true;
      otaErrorMessage = "Firmware too large (max 1.25MB)";
      OTA_LOG("[OTA] Rejected: size %u exceeds max %u\n", contentLength, OTA_MAX_SIZE);
      xSemaphoreGive(otaMutex);
      return;
    }

    OTA_LOG("[OTA] Starting: %s (%u bytes)\n", upload.filename.c_str(), contentLength);

    // Begin update with known size for better error detection
    if (!Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      otaHadError = true;
      otaErrorMessage = "Update.begin() failed";
      Update.printError(Serial);
      xSemaphoreGive(otaMutex);
      return;
    }

    otaInProgress = true;
    otaExpectedSize = contentLength;
    otaReceivedSize = 0;
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    // Only process if we have an active update
    if (!otaInProgress) return;

    // Validate firmware magic byte on first chunk (ESP32 firmware starts with 0xE9)
    if (!otaHeaderChecked && upload.currentSize > 0) {
      otaHeaderChecked = true;
      if (upload.buf[0] != 0xE9) {
        otaHadError = true;
        otaErrorMessage = "Invalid firmware format";
        OTA_LOGLN("[OTA] Rejected: invalid firmware header (not 0xE9)");
        Update.abort();
        otaInProgress = false;
        if (otaMutex) xSemaphoreGive(otaMutex);
        return;
      }
    }

    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaHadError = true;
      otaErrorMessage = "Write error during upload";
      Update.printError(Serial);
      Update.abort();
      otaInProgress = false;
      if (otaMutex) xSemaphoreGive(otaMutex);
    } else {
      otaReceivedSize += upload.currentSize;  // Track bytes received
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (!otaInProgress) {
      // Upload ended but we never started properly
      if (otaMutex) xSemaphoreGive(otaMutex);
      return;
    }

    if (Update.end(true)) {
      OTA_LOG("[OTA] Success: %u bytes written\n", upload.totalSize);
      // Warn if received size doesn't match Content-Length header
      if (otaExpectedSize > 0 && otaReceivedSize != otaExpectedSize) {
        OTA_LOG("[OTA] Warning: expected %u bytes, received %u\n", otaExpectedSize, otaReceivedSize);
      }
    } else {
      otaHadError = true;
      otaErrorMessage = "Update finalization failed";
      Update.printError(Serial);
    }

    otaInProgress = false;
    if (otaMutex) xSemaphoreGive(otaMutex);
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    OTA_LOGLN("[OTA] Upload aborted by client");
    if (otaInProgress) {
      Update.abort();
      otaInProgress = false;
    }
    otaHadError = true;
    otaErrorMessage = "Upload aborted";
    if (otaMutex) xSemaphoreGive(otaMutex);
  }
}

void handleOtaComplete() {
  noteActivity();

  // Check for errors during upload
  if (otaHadError || Update.hasError()) {
    String response = "{\"error\":\"";
    if (otaErrorMessage.length() > 0) {
      response += otaErrorMessage;
    } else {
      response += "Update failed";
    }
    response += "\"";
    if (otaRetryAfterMs > 0) {
      response += ",\"retryAfterMs\":";
      response += String(otaRetryAfterMs);
    }
    if (otaHardLocked) {
      response += ",\"lockedOut\":true";
    }
    response += "}";

    // Clear error state (but NOT rate limiting state)
    otaHadError = false;
    otaErrorMessage = "";
    otaRetryAfterMs = 0;

    server.send(400, "application/json", response);
    return;
  }

  // Success!
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Rebooting...\"}");

  // Allow response to be sent before reboot
  delay(1000);
  state.playing = false;
  allOff();
  ESP.restart();
}

void handleNotFound(){
  if (captivePortal()) return;
  noteActivity();
  sendIndex();
}

// -------------------- Deep Sleep helper -------------------
void goToDeepSleep() {
  state.playing = false;
  allOff();

  if (apRunning) {
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apRunning = false;
  }

  // D2 on XIAO ESP32C6 is GPIO2 (RTC-capable)
  const uint64_t mask = 1ULL << 2;  // GPIO2
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

  delay(50);
  esp_deep_sleep_start();
}

// -------------------- Blue Pulse (WiFi AP Ready Indicator) --------------------
// Called repeatedly in loop() to create a breathing blue effect while waiting for UI connection
void updateBluePulse() {
  unsigned long now = millis();
  if (now - lastPulseMs < PULSE_INTERVAL_MS) return;
  lastPulseMs = now;

  // Update brightness with direction
  int newState = pulseState + pulseDirection;
  if (newState >= 90) {
    newState = 90;
    pulseDirection = -5;  // Reverse to dim
  } else if (newState <= 10) {
    newState = 10;
    pulseDirection = 5;   // Reverse to brighten
  }
  pulseState = (uint8_t)newState;

  // Apply blue color at current brightness
  FastLED.setBrightness(pulseState);
  leds[0] = CRGB(0, 0, 128);
  FastLED.show();
}

// -------------------- Connection Confirm Blink --------------------
// 4 rapid blue blinks to indicate successful Web UI connection
void connectionConfirmBlink() {
  for (int i = 0; i < 4; i++) {
    FastLED.setBrightness(160);
    leds[0] = CRGB(0, 0, 128);
    FastLED.show();
    delay(100);
    rgbOff();
    delay(100);
  }
  rgbOff();
}

// -------------------- Setup / Loop -----------------------
void setup(){
  delay(100);
  Serial.begin(115200);
  
  // Print startup banner
  Serial.println();
  Serial.println("===========================================");
  Serial.println("  The Signet Morse Beacon");
  Serial.printf("  Firmware Version: %s\n", FIRMWARE_VERSION);
  Serial.printf("  Build Date: %s\n", FIRMWARE_DATE);
  Serial.println("===========================================");
  Serial.println();

  // Sleep switch first
  pinMode(PIN_SLEEP_SW, INPUT_PULLUP);

  // IR LED hardware PWM setup
  setupIrHardwarePwm();

  // Pre-set RGB data pin LOW to prevent spurious flash during FastLED init
  pinMode(PIN_RGB, OUTPUT);
  digitalWrite(PIN_RGB, LOW);
  delay(1);

  // RGB (WS2812) - init & turn OFF immediately
  leds[0] = CRGB::Black;
  FastLED.addLeds<NEOPIXEL, PIN_RGB>(leds, NUM_PIXELS);
  rgbOff();

  // If switch is OPEN at boot (D2 HIGH), go straight to deep sleep
  if (digitalRead(PIN_SLEEP_SW) == HIGH) {
    goToDeepSleep();
  }

  // LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("WARNING: LittleFS mount failed - splash image unavailable");
  }

  // Stateless design: Default to English, language modal shows every boot
  state.language = LANG_EN;
  Serial.printf("Language: %s (stateless - modal shows each boot)\n", LANG_CODES[state.language]);

  // Create mutex for thread-safe text access
  textMutex = xSemaphoreCreateMutex();
  if (!textMutex) {
    Serial.println("FATAL: Failed to create text mutex - halting");
    while (true) { delay(1000); }  // Halt - don't run without mutex
  }

  // Create mutex for OTA upload synchronization
  otaMutex = xSemaphoreCreateMutex();
  if (!otaMutex) {
    Serial.println("FATAL: Failed to create OTA mutex - halting");
    while (true) { delay(1000); }  // Halt - don't run without mutex
  }

  // Wi-Fi AP + captive DNS
  // Single connection limit: first client gets exclusive access (OTA security)
  WiFi.mode(WIFI_AP);

  apSsid = buildApSsidWithMacTail();
  WiFi.softAPConfig(apIP, apGateway, apSubnet);

  // Select random non-overlapping WiFi channel (1, 6, or 11) to reduce interference
  const uint8_t channels[] = {1, 6, 11};
  uint8_t apChannel = channels[esp_random() % 3];
  WiFi.softAP(apSsid.c_str(), AP_PASS, apChannel, 0, 1);  // hidden=0, max_conn=1

  // Pre-compute redirect URL once (reduces heap pressure in request handlers)
  portalRedirectUrl = String("http://") + apIP.toString() + "/";

  // Debug
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  Serial.print("AP SSID: "); Serial.println(apSsid);
  Serial.print("AP IP: ");   Serial.println(WiFi.softAPIP());
  Serial.printf("AP Channel: %d\n", apChannel);

  dnsServer.start(53, "*", apIP);

  apRunning = true;
  noteActivity();

  // HTTP routes
  server.on("/",           HTTP_GET,  sendIndex);
  server.on("/help",       HTTP_GET,  sendHelpPage);
  server.on("/api/state",  HTTP_GET,  handleState);
  server.on("/api/update", HTTP_POST, handleUpdate);
  server.on("/api/play",     HTTP_POST, handlePlay);
  server.on("/api/stop",     HTTP_POST, handleStop);
  server.on("/api/language", HTTP_POST, handleLanguageSet);
  server.on("/api/ota-code", HTTP_POST, handleOtaCode);
  server.on("/api/ota",      HTTP_POST, handleOtaComplete, handleOtaUpload);

  // Collect custom headers for OTA code verification
  const char* otaHeaders[] = {"X-OTA-Code"};
  server.collectHeaders(otaHeaders, 1);

  // Serve splash image from LittleFS as /bb.jpg
  server.on("/bb.jpg", HTTP_GET, [](){
    File f = LittleFS.open("/bb.jpg", "r");
    if (!f) {
      server.send(404, "text/plain", "Image not found");
      return;
    }
    server.streamFile(f, "image/jpeg");
    f.close();
  });

  // Serve custom font from LittleFS
  server.on("/HarlowSolid.ttf", HTTP_GET, [](){
    File f = LittleFS.open("/HarlowSolid.ttf", "r");
    if (!f) {
      server.send(404, "text/plain", "Font not found");
      return;
    }
    server.streamFile(f, "font/ttf");
    f.close();
  });

  // ---- OS-Specific Captive Portal Detection Handlers ----
  // These endpoints are probed by various operating systems to detect captive portals.
  // IMPORTANT: HTTP 302 redirect triggers the popup. HTTP 204 tells the OS "internet works" and PREVENTS the popup.

  // Android / Chrome: Probes /generate_204 expecting HTTP 204 if internet works.
  // Returning 302 redirect instead triggers the "Sign in to network" popup.
  server.on("/generate_204", HTTP_GET, [](){
    noteActivity();
    server.sendHeader("Location", portalRedirectUrl, true);
    server.send(302, "text/plain", "");
  });
  server.on("/gen_204", HTTP_GET, [](){
    noteActivity();
    server.sendHeader("Location", portalRedirectUrl, true);
    server.send(302, "text/plain", "");
  });

  // iOS / macOS: Probes /hotspot-detect.html, redirect triggers popup
  server.on("/hotspot-detect.html", HTTP_GET, [](){
    noteActivity();
    server.sendHeader("Location", portalRedirectUrl, true);
    server.send(302, "text/plain", "");
  });

  // Windows 10/11: Probes /connecttest.txt expecting "Microsoft Connect Test" body.
  // Redirect to http://logout.net (documented workaround that triggers Windows captive portal popup).
  server.on("/connecttest.txt", HTTP_GET, [](){
    noteActivity();
    server.sendHeader("Location", "http://logout.net", true);
    server.send(302, "text/plain", "");
  });
  // Windows follows up by requesting /redirect after detecting captive portal
  server.on("/redirect", HTTP_GET, [](){
    noteActivity();
    server.sendHeader("Location", portalRedirectUrl, true);
    server.send(302, "text/plain", "");
  });
  // Microsoft fwlink endpoint (used for captive portal interactions)
  server.on("/fwlink", HTTP_GET, [](){
    noteActivity();
    server.sendHeader("Location", portalRedirectUrl, true);
    server.send(302, "text/plain", "");
  });

  // Firefox: Probes /canonical.html and /success.txt
  server.on("/canonical.html", HTTP_GET, [](){
    noteActivity();
    server.sendHeader("Location", portalRedirectUrl, true);
    server.send(302, "text/plain", "");
  });
  server.on("/success.txt", HTTP_GET, [](){
    noteActivity();
    server.sendHeader("Location", portalRedirectUrl, true);
    server.send(302, "text/plain", "");
  });

  // Prevent Windows WPAD (Web Proxy Auto-Discovery) repeated requests
  server.on("/wpad.dat", HTTP_GET, [](){
    server.send(404, "text/plain", "");
  });

  server.onNotFound(handleNotFound);
  server.begin();

  // Morse task
  xTaskCreate(morseTask, "morse", 4096, nullptr, 1, &morseTaskHandle);

  // Mark firmware as valid (for OTA rollback protection)
  esp_ota_mark_app_valid_cancel_rollback();

  // Task Watchdog Timer - Configure with 60s timeout for captive portal operations
  // Android makes multiple rapid requests which can delay loop execution
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 60000,  // 60 seconds (longer for Android captive portal)
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_deinit();  // Remove any existing WDT config
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog timer enabled (60s timeout)");
}

void loop(){
  // If switch is OPEN at any time, go to deep sleep (with debounce)
  if (digitalRead(PIN_SLEEP_SW) == HIGH) {
    delay(50);  // Debounce delay
    if (digitalRead(PIN_SLEEP_SW) == HIGH) {  // Confirm still HIGH
      goToDeepSleep();
    }
  }

  if (apRunning) {
    dnsServer.processNextRequest();
    server.handleClient();

    // Security: If MAC is locked and locked client disconnected, shut down AP
    if (macLocked) {
      wifi_sta_list_t stationList;
      if (esp_wifi_ap_get_sta_list(&stationList) == ESP_OK) {
        bool lockedClientPresent = false;
        for (int i = 0; i < stationList.num; i++) {
          if (memcmp(stationList.sta[i].mac, lockedMac, 6) == 0) {
            lockedClientPresent = true;
            break;
          }
        }
        if (!lockedClientPresent) {
          Serial.println("[Security] Locked client disconnected - shutting down AP");
          dnsServer.stop();
          WiFi.softAPdisconnect(true);
          WiFi.mode(WIFI_OFF);
          apRunning = false;
          allOff();
          // Clear locked MAC from memory (defense-in-depth)
          memset(lockedMac, 0, sizeof(lockedMac));
          macLocked = false;
        }
      }
    }

    // Pulse blue LED while waiting for Web UI connection
    if (!uiServed && !state.playing) {
      updateBluePulse();
    }

    // idle timeout -> shut down Wi-Fi
    unsigned long now = millis();
    if ((now - lastActivityMs) > AP_IDLE_TIMEOUT_MS) {
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      apRunning = false;
      allOff();
    }
  }

  // Feed the watchdog timer (prevents reset during normal operation)
  esp_task_wdt_reset();
}
