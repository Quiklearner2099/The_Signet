/*
  ===================================================================================
  The Signet Morse Beacon
  Version: 1.0.0
  Release Date: January 1, 2026
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
  - 90-second idle WiFi AP timeout to conserve power

  HARDWARE:
  - Seeed Studios XIAO ESP32-C6 Module (other ESP32s can be used)
  - WS2812B RGB LED
  - IR LED
  - 10 Ohm resisitor
  - 270 Ohm resistor
  - 47k Ohm resistor
  - 100k Ohm resistor
  - 2N3904 NPN Transistor
  - SPST switch for sleep control
  - 503035 LiPo battery with built in charge/discharge protection (optional)

  - See schematic for complete circuit

  AUTHOR:
  Created by Mike Stewart as a tool for all those fighting to maintain free-speech
  and against Authoritarianism.

  ===================================================================================
  VERSION HISTORY:
  ===================================================================================
  
  v1.0.0 (January 1, 2026) - Initial Stable Release

  ===================================================================================
  PIN CONFIGURATION: for Seeed Studios ESP32-C6
  ===================================================================================
  PIN_RGB      D5    - WS2812B RGB LED data
  PIN_IR       D8    - IR LED driver (via NTD3528I8)
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

// -------------------- Version Information --------------------
#define FIRMWARE_VERSION "1.0.0"
#define FIRMWARE_DATE    "JANUARY 1 2026"

// -------------------- Forward Declarations --------------------
enum Mode     { DISCREET = 0, VISIBLE = 1 };
enum ColorSel { C_RED = 0, C_GREEN = 1, C_BLUE = 2 };
enum Intensity{ I_LOW = 0, I_MED = 1, I_HIGH = 2 };

uint8_t  rgbBrightnessFor(Intensity i);
uint8_t  irDutyFor(Intensity i);
CRGB     colorValue(ColorSel c);
String   intensityToStr(Intensity i);
Intensity strToIntensity(const String& s);
String   colorToStr(ColorSel c);
ColorSel strToColor(const String& s);

void irPwmTask(void* pv);
void morseTask(void* pv);

void sendIndex();
void handleState();
void handleUpdate();
void handlePlay();
void handleStop();
void handleNotFound();

bool  captivePortal();
void  readyBlink();
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

// -------------------- WiFi AP -------------------------
const char* AP_SSID_BASE = "The_Signet";
const char* AP_PASS      = "";

IPAddress apIP(192,168,4,1);
IPAddress apGateway(192,168,4,1);
IPAddress apSubnet(255,255,255,0);

String apSsid;  // must persist (global) so c_str() stays valid

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
const unsigned long AP_IDLE_TIMEOUT_MS = 90000; // 90 seconds
unsigned long lastActivityMs = 0;
bool apRunning = false;

inline void noteActivity() { lastActivityMs = millis(); }

// -------------------- Servers -----------------------------
WebServer server(80);
DNSServer dnsServer;

// -------------------- RGB via FastLED ---------------------
CRGB leds[NUM_PIXELS];

// -------------------- Software PWM for IR -----------------
// This may change to Hardware PWM in future release
static TaskHandle_t irPwmTaskHandle = nullptr;
volatile uint8_t g_irDuty = 0;
volatile bool    g_irEnable = false;

void irPwmTask(void*) {
  const uint32_t PERIOD_US = 5000; // 200 Hz
  while (true) {
    uint32_t on_us  = (g_irEnable ? (uint32_t)g_irDuty * PERIOD_US / 255 : 0);
    uint32_t off_us = (on_us >= PERIOD_US) ? 0 : (PERIOD_US - on_us);

    if (on_us > 0) {
      digitalWrite(PIN_IR, HIGH);
      delayMicroseconds(on_us);
      digitalWrite(PIN_IR, LOW);
    } else {
      digitalWrite(PIN_IR, LOW);
    }

    if (off_us >= 1000) {
      vTaskDelay(pdMS_TO_TICKS(off_us / 1000));
      uint32_t rem = off_us % 1000;
      if (rem) delayMicroseconds(rem);
    } else if (off_us > 0) {
      delayMicroseconds(off_us);
    }
  }
}

// -------------------- App State --------------------------
static SemaphoreHandle_t textMutex = nullptr;

struct AppState {
  volatile Mode mode = DISCREET;
  volatile ColorSel color = C_RED;
  volatile Intensity intensity = I_MED;
  volatile bool dazzle = false;
  String text = "S O S";
  volatile bool playing = false;
} state;

String getTextCopy() {
  String copy;
  if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    copy = state.text;
    xSemaphoreGive(textMutex);
  } else {
    copy = "S O S";  // Fallback if mutex fails
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
    case C_RED:   return CRGB(128,0,0);
    case C_GREEN: return CRGB(0,128,0);
    case C_BLUE:  return CRGB(0,0,128);
    default:      return CRGB(128,0,0);
  }
}

// -------------------- Morse Definitions -------------------
const uint32_t UNIT=160, DOT=UNIT, DASH=3*UNIT, GAP_INTRA=UNIT, GAP_LETTER=3*UNIT, GAP_WORD=7*UNIT;
const uint32_t DISCREET_LOOP_GAP=2000, VISIBLE_DAZZLE_OFF1=750, VISIBLE_DAZZLE_OFF2=750, VISIBLE_DAZZLE_FLASH=500;

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

// -------------------- LED Control -------------------------
inline void irOff() { g_irDuty = 0; g_irEnable = false; digitalWrite(PIN_IR, LOW); }
inline void irOn()  { g_irDuty = irDutyFor(state.intensity); g_irEnable = true; }

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

// -------------------- Morse Task --------------------------
TaskHandle_t morseTaskHandle = nullptr;
inline void morseDelay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void playSymbol(char s) {
  uint32_t onMs = (s == '.') ? DOT : DASH;
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
    if (code[i+1]) morseDelay(GAP_INTRA);
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
        if (c == ' ') {
          morseDelay(GAP_WORD);
          continue;
        }
        const char* code = lookupMorse(c);
        if (code) {
          playLetter(code);
          if (!state.playing) break;
          morseDelay(GAP_LETTER);
        }
      }

      if (!state.playing) break;

      if (state.mode == DISCREET) {
       
        allOff();
        morseDelay(DISCREET_LOOP_GAP);
      } else {
        if (state.dazzle) doDazzleGap();
        else { activeLedOff(); morseDelay(DISCREET_LOOP_GAP); }
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
:root{--bg:#121212;--card:#1E1E1E;--text:#ECECEC;--muted:#B0B0B0;--accent:#8AB4F8;--primary:#8AB4F8}
*{box-sizing:border-box;font-family:Inter,system-ui,Segoe UI,Roboto,Arial,sans-serif}
body{margin:0;background:var(--bg);color:var(--text)}
.wrap{max-width:820px;margin:0 auto;padding:20px}
.appbar{display:flex;justify-content:center;position:sticky;top:0;background:#0D0D0D;padding:14px 20px;border-bottom:1px solid #222}
.title{text-align:center;font-size:18px;font-weight:600;letter-spacing:.3px}
.grid{display:grid;gap:16px;margin-top:16px;grid-template-columns:1fr}
@media(min-width:720px){.grid{grid-template-columns:1fr 1fr}}
.card{background:var(--card);border:1px solid #2A2A2A;border-radius:16px;padding:16px;box-shadow:0 4px 16px rgba(0,0,0,.35)}
.card h3{text-align:center;margin:.2rem 0 1rem 0;font-size:16px;font-weight:600;color:#ddd}
.row{display:flex;gap:12px;flex-wrap:wrap;align-items:center}
.row.center{justify-content:center}
.center-text{text-align:center}
.pill{display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;background:#151515;border:1px solid #2A2A2A;cursor:pointer;user-select:none}
.pill input{accent-color:var(--accent)}
button{border:0;border-radius:12px;padding:10px 14px;background:var(--primary);color:#000;font-weight:700;cursor:pointer}
button.ghost{background:transparent;color:#fff;border:1px solid #333}
button:disabled,.disabled{opacity:.45;pointer-events:none}
.seg{display:flex;gap:8px}
.seg.center{justify-content:center}
.seg button{background:#171717;border:1px solid #2A2A2A;color:#ddd}
.seg button.active{background:var(--accent);color:#000;border-color:var(--accent)}
.color{width:34px;height:34px;border-radius:10px;border:1px solid #2A2A2A;cursor:pointer;opacity:.9}
.color.active{outline:2px solid var(--accent);opacity:1}
.color.red{background:#f44336}.color.green{background:#4caf50}.color.blue{background:#2196f3}
.muted{color:var(--muted);font-size:12px}
input[type="text"]{width:100%;background:#151515;border:1px solid #2A2A2A;border-radius:12px;padding:12px;color:#eee;outline:none}
.footer{margin-top:8px;color:#777;font-size:12px}
.switch{position:relative;display:inline-block;width:52px;height:28px}
.switch input{display:none}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#333;border:1px solid #444;transition:.2s;border-radius:999px}
.slider:before{position:absolute;content:"";height:22px;width:22px;left:3px;bottom:2px;background:#bbb;transition:.2s;border-radius:50%}
input:checked + .slider{background:var(--accent);border-color:var(--accent)}
input:checked + .slider:before{transform:translateX(23px);background:#111}
#splash{position:fixed;inset:0;background:rgba(0,0,0,.92);display:flex;align-items:center;justify-content:center;z-index:9999;opacity:0;pointer-events:none;transition:opacity .3s}
#splash.visible{opacity:1;pointer-events:auto}
#splash img{max-width:90vw;max-height:90vh;border-radius:12px;box-shadow:0 0 40px rgba(0,0,0,.7)}
</style>
</head>
<body>

<div id="splash"><img src="/bb.jpg" alt="Big Brother"></div>

<div class="appbar"><span class="title">The Signet Morse Beacon</span></div>
<div class="wrap">
<div class="grid">

  <div class="card">
    <h3>Mode</h3>
    <div class="seg center" id="modeGroup">
      <button data-mode="DISCREET">Discreet (IR)</button>
      <button data-mode="VISIBLE">Visible (RGB)</button>
    </div>
  </div>

  <div class="card">
    <h3>Intensity</h3>
    <div class="seg center" id="intensityGroup">
      <button data-int="LOW">Low</button>
      <button data-int="MED">Med</button>
      <button data-int="HIGH">High</button>
    </div>
  </div>

  <div class="card" id="colorCard">
    <h3>Color (Visible mode)</h3>
    <div class="row center" id="colorGroup">
      <div class="color red" data-color="RED"></div>
      <div class="color green" data-color="GREEN"></div>
      <div class="color blue" data-color="BLUE"></div>
    </div>
  </div>

  <div class="card" id="dazzleCard">
    <h3>Dazzle Effect</h3>
    <div class="row center">
      <label class="switch"><input type="checkbox" id="dazzle"><span class="slider"></span></label>
      <span class="muted">RGB flash between loops</span>
    </div>
  </div>

  <div class="card" style="grid-column:1/-1">
    <h3>Message</h3>
    <input type="text" id="msg" placeholder="S O S" maxlength="64">
    <p class="muted center-text" style="margin-top:6px">Letters, numbers, spaces. Use space for word gap.</p>
    <div class="row center" style="margin-top:14px">
      <button id="play">&#9654; Play</button>
      <button id="stop" class="ghost">&#9632; Stop</button>
    </div>
    <p class="muted center-text" style="margin-top:8px" id="status">Idle</p>
  </div>

</div>
<p class="footer center-text">1984 was not an instruction manual</p>
</div>

<script>
const SPLASH_KEY = 'signet_splash_seen';
const ui = {
  modes: document.querySelectorAll('#modeGroup button'),
  ints: document.querySelectorAll('#intensityGroup button'),
  colors: document.querySelectorAll('#colorGroup .color'),
  dazzle: document.getElementById('dazzle'),
  msg: document.getElementById('msg'),
  play: document.getElementById('play'),
  stop: document.getElementById('stop'),
  status: document.getElementById('status'),
  colorCard: document.getElementById('colorCard'),
  dazzleCard: document.getElementById('dazzleCard')
};

async function post(url, body) {
  try {
    const r = await fetch(url, { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body) });
    return r.json();
  } catch(e) { console.error(e); }
}

async function getState() {
  try {
    const r = await fetch('/api/state');
    const s = await r.json();
    ui.modes.forEach(b => b.classList.toggle('active', (s.mode===0 && b.dataset.mode==='DISCREET') || (s.mode===1 && b.dataset.mode==='VISIBLE')));
    ui.ints.forEach(b => b.classList.toggle('active', b.dataset.int === s.intensity));
    ui.colors.forEach(c => c.classList.toggle('active', c.dataset.color === s.color));
    ui.dazzle.checked = s.dazzle;
    // Only update text field if user is NOT currently typing in it
    if (s.text && document.activeElement !== ui.msg) ui.msg.value = s.text;
    ui.status.textContent = s.playing ? 'Playing…' : 'Idle';

    const visMode = s.mode === 1;
    ui.colorCard.style.opacity = visMode ? 1 : 0.4;
    ui.dazzleCard.style.opacity = visMode ? 1 : 0.4;
  } catch(e) { console.error(e); }
}

ui.modes.forEach(b => b.addEventListener('click', async ()=>{
  ui.modes.forEach(x=>x.classList.remove('active')); b.classList.add('active');
  await post('/api/update', { mode: b.dataset.mode });
  getState();
}));
ui.ints.forEach(b => b.addEventListener('click', async ()=>{
  ui.ints.forEach(x=>x.classList.remove('active')); b.classList.add('active');
  await post('/api/update', { intensity: b.dataset.int });
}));
ui.colors.forEach(c => c.addEventListener('click', async ()=>{
  ui.colors.forEach(x=>x.classList.remove('active')); c.classList.add('active');
  await post('/api/update', { color: c.dataset.color });
}));
ui.dazzle.addEventListener('change', async ()=>{ await post('/api/update', { dazzle: ui.dazzle.checked }); });
ui.play.addEventListener('click', async ()=>{ await post('/api/play', { text: ui.msg.value || '' }); ui.status.textContent = 'Playing…'; });
ui.stop.addEventListener('click', async ()=>{ await post('/api/stop'); ui.status.textContent = 'Idle'; });

function setupSplash(){
  const splash = document.getElementById('splash');
  if (!splash) return;

  try {
    if (window.localStorage && localStorage.getItem(SPLASH_KEY) === '1') {
      splash.remove();
      return;
    }
  } catch(e){}

  splash.classList.add('visible');

  const hide = () => {
    splash.classList.remove('visible');
    try { if (window.localStorage) localStorage.setItem(SPLASH_KEY, '1'); } catch(e){}
    setTimeout(() => splash.remove(), 300);
  };

  splash.addEventListener('click', hide);
  setTimeout(hide, 8000);
}

setupSplash();
getState();
setInterval(getState, 1800);
</script>
</body></html>
)====";

// -------------------- Captive Portal helpers --------------
bool isIpAddress(String str) {
  for (size_t i=0; i<str.length(); i++) {
    char c = str[i];
    if ((c != '.') && (c < '0' || c > '9')) return false;
  }
  return true;
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
    case C_RED:   return "RED";
    case C_GREEN: return "GREEN";
    case C_BLUE:  return "BLUE";
    default:      return "RED";
  }
}

ColorSel strToColor(const String& s){
  if(s=="GREEN") return C_GREEN;
  if(s=="BLUE")  return C_BLUE;
  return C_RED;
}

void sendIndex(){
  noteActivity();
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleState(){
  if (captivePortal()) return;
  noteActivity();
  StaticJsonDocument<256> doc;
  doc["mode"]     = (int)state.mode;
  doc["intensity"]= intensityToStr(state.intensity);
  doc["color"]    = colorToStr(state.color);
  doc["dazzle"]   = state.dazzle;
  // FIX: Thread-safe access to text
  doc["text"]     = getTextCopy();
  doc["playing"]  = state.playing;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleUpdate(){
  noteActivity();
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, server.arg("plain"));
  if (!err) {
    if (doc.containsKey("mode")) {
      String m = doc["mode"].as<String>();
      state.mode = (m == "VISIBLE") ? VISIBLE : DISCREET;
      // FIX: When switching modes, turn off BOTH LEDs to prevent bleed-through
      allOff();
    }
    if (doc.containsKey("intensity")) {
      state.intensity = strToIntensity(doc["intensity"].as<String>());
      // FIX: Update IR duty cycle live if currently playing in DISCREET mode
      if (state.playing && state.mode == DISCREET && g_irEnable) {
        g_irDuty = irDutyFor(state.intensity);
      }
    }
    if (doc.containsKey("color"))     state.color     = strToColor(doc["color"].as<String>());
    if (doc.containsKey("dazzle"))    state.dazzle    = doc["dazzle"].as<bool>();
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handlePlay(){
  noteActivity();
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, server.arg("plain"));
  // FIX: Thread-safe text update with length validation
  if (!err && doc.containsKey("text")) {
    String newText = doc["text"].as<String>();
    // Limit to 64 characters to prevent memory issues
    if (newText.length() > 64) newText = newText.substring(0, 64);
    // Don't allow empty text - keep default if empty
    if (newText.length() > 0) {
      setTextSafe(newText);
    }
  }
  state.playing = true;
  server.send(200, "application/json", "{\"playing\":true}");
}

void handleStop(){
  noteActivity();
  state.playing = false;
  allOff();
  server.send(200, "application/json", "{\"playing\":false}");
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

// -------------------- Ready Blink ------------------------
void readyBlink(){
  FastLED.setBrightness(160);
  for (int k=0; k<2; k++) {
    leds[0]=CRGB(128,0,0); FastLED.show(); delay(250);
    leds[0]=CRGB(0,128,0); FastLED.show(); delay(250);
    leds[0]=CRGB(0,0,128); FastLED.show(); delay(250);
  }
  allOff();
}

// -------------------- Setup / Loop -----------------------
void setup(){
  //delay(100);
  //Serial.begin(115200);
  
  // Print startup banner
  //Serial.println();
  //Serial.println("===========================================");
  //Serial.println("  The Signet Morse Beacon");
  //Serial.printf("  Firmware Version: %s\n", FIRMWARE_VERSION);
  //Serial.printf("  Build Date: %s\n", FIRMWARE_DATE);
  //Serial.println("  (c) 2025 Cunths & Queeths LLC");
  //Serial.println("===========================================");
  //Serial.println();

  // Sleep switch first
  pinMode(PIN_SLEEP_SW, INPUT_PULLUP);

  // IR pin
  pinMode(PIN_IR, OUTPUT);
  digitalWrite(PIN_IR, LOW);

  // RGB (WS2812) - init & turn OFF immediately
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

  // Create mutex for thread-safe text access
  textMutex = xSemaphoreCreateMutex();
  if (!textMutex) {
    Serial.println("WARNING: Failed to create text mutex");
  }

  // IR software PWM task
  xTaskCreate(irPwmTask, "irpwm", 2048, nullptr, 2, &irPwmTaskHandle);

  // Wi-Fi AP + captive DNS
  WiFi.mode(WIFI_AP);

  apSsid = buildApSsidWithMacTail();
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(apSsid.c_str(), AP_PASS);

  // Debug
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  Serial.print("AP SSID: "); Serial.println(apSsid);
  Serial.print("AP IP: ");   Serial.println(WiFi.softAPIP());

  dnsServer.start(53, "*", apIP);

  apRunning = true;
  noteActivity();

  // HTTP routes
  server.on("/",           HTTP_GET,  sendIndex);
  server.on("/api/state",  HTTP_GET,  handleState);
  server.on("/api/update", HTTP_POST, handleUpdate);
  server.on("/api/play",   HTTP_POST, handlePlay);
  server.on("/api/stop",   HTTP_POST, handleStop);

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

  server.onNotFound(handleNotFound);
  server.begin();

  // Ready indication
  readyBlink();

  // Morse task
  xTaskCreate(morseTask, "morse", 4096, nullptr, 1, &morseTaskHandle);
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
}
