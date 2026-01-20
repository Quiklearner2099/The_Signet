/*
  ===================================================================================
  The Signet Morse Beacon
  Version: 1.0.3
  Release Date: January 19, 2026
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
  - 90-second idle WiFi AP timeout to conserve power

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
  - 503035 LiPo battery with built in charge/discharge protection (optional)

  - See schematic for complete circuit

  AUTHOR:
  Created by Mike Stewart as a tool for all those fighting to maintain free-speech
  and against Authoritarianism.

  ===================================================================================
  VERSION HISTORY:
  ===================================================================================

  v1.0.3 (January 19, 2026) - Blue pulsing LED indicates WiFi AP ready, 4 rapid blinks
                              confirm Web UI connection
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

// -------------------- Version Information --------------------
#define FIRMWARE_VERSION "1.0.3"
#define FIRMWARE_DATE    "JANUARY 19 2026"

// -------------------- Forward Declarations --------------------
enum Mode     { DISCREET = 0, VISIBLE = 1 };
enum ColorSel { C_RED = 0, C_GREEN = 1, C_BLUE = 2, C_CUSTOM = 3 };
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

// -------------------- UI Connection State & Blue Pulse --------------------
volatile bool uiServed = false;              // True once Web UI has been served
static uint8_t pulseState = 10;              // Current brightness (10-90)
static int8_t pulseDirection = 5;            // Direction and step size
static unsigned long lastPulseMs = 0;
const unsigned long PULSE_INTERVAL_MS = 30;  // Update every 30ms for smooth breathing

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
  volatile uint8_t wpm = 10;  // Words per minute (5-20 range)
  volatile uint8_t customR = 255;  // Custom color RGB components
  volatile uint8_t customG = 0;
  volatile uint8_t customB = 255;  // Default: magenta
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

uint32_t ditDuration()  { return 1200 / state.wpm; }
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
:root{--bg:#121212;--card:#1E1E1E;--text:#ECECEC;--muted:#B0B0B0;--accent:#8AB4F8;--primary:#8AB4F8}
*{box-sizing:border-box;font-family:Inter,system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;padding:0}
body{background:var(--bg);color:var(--text)}
.wrap{max-width:420px;margin:0 auto;padding:10px}
.appbar{text-align:center;padding:10px;font-size:15px;font-weight:600;color:#ddd;border-bottom:1px solid #222}
.grid{display:flex;flex-direction:column;gap:10px;margin-top:10px}
.row-2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.card{background:var(--card);border:1px solid #2A2A2A;border-radius:12px;padding:12px}
.card h3{font-size:11px;font-weight:600;color:#888;text-transform:uppercase;letter-spacing:.5px;margin-bottom:8px}
.seg{display:flex;gap:6px}
.seg button{flex:1;border:0;border-radius:8px;padding:8px 4px;background:#171717;border:1px solid #2A2A2A;color:#ddd;font-size:12px;font-weight:500;cursor:pointer}
.seg button.active{background:var(--accent);color:#000;border-color:var(--accent)}
.color-row{display:flex;gap:8px;align-items:center}
.color{width:28px;height:28px;border-radius:8px;border:1px solid #2A2A2A;cursor:pointer}
.color.active{outline:2px solid var(--accent)}
.color.red{background:#f44336}.color.green{background:#4caf50}.color.blue{background:#2196f3}
.color.custom{background:linear-gradient(135deg,#f44,#ff0,#0f0,#0ff,#00f,#f0f,#f44);position:relative}
.color-picker-wrap{display:none;margin-top:8px;text-align:center}
.color-picker-wrap.visible{display:block}
#wheelCanvas{cursor:crosshair;border-radius:50%}
#briSlider{width:80px;margin-top:6px;-webkit-appearance:none;height:6px;border-radius:3px;background:linear-gradient(to right,#000,#fff)}
#briSlider::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;border-radius:50%;background:var(--accent);cursor:pointer;border:2px solid #222}
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
.msg-input{width:100%;background:#151515;border:1px solid #2A2A2A;border-radius:10px;padding:10px;color:#eee;font-size:14px;outline:none}
.msg-hint{font-size:10px;color:#fff;margin:6px 0;text-align:center}
.btn-row{display:flex;gap:8px;margin-top:8px}
.btn-row button{flex:1;border:0;border-radius:10px;padding:10px;font-weight:700;font-size:13px;cursor:pointer}
.btn-row .play{background:var(--primary);color:#000}
.btn-row .stop{background:transparent;color:#fff;border:1px solid #333}
.status{text-align:center;font-size:11px;color:#666;margin-top:6px}
.footer{text-align:center;font-size:10px;color:#FFD700;font-weight:700;margin-top:10px}
#splash{position:fixed;inset:0;background:rgba(0,0,0,.92);display:flex;align-items:center;justify-content:center;z-index:9999;opacity:0;pointer-events:none;transition:opacity .3s}
#splash.visible{opacity:1;pointer-events:auto}
#splash img{max-width:90vw;max-height:90vh;border-radius:12px;box-shadow:0 0 40px rgba(0,0,0,.7)}
</style>
</head>
<body>

<div id="splash"><img src="/bb.jpg" alt="Big Brother"></div>

<div class="appbar">The Signet Morse Beacon</div>
<div class="wrap">
<div class="grid">

  <div class="card">
    <h3>Message</h3>
    <input type="text" class="msg-input" id="msg" placeholder="S O S" maxlength="64">
    <div class="msg-hint">A-Z a-z 0-9 . , ? / - ( ) @ = space</div>
    <div class="btn-row">
      <button class="play" id="play">&#9654; Play</button>
      <button class="stop" id="stop">&#9632; Stop</button>
    </div>
    <div class="status" id="status">Idle</div>
  </div>

  <div class="row-2">
    <div class="card">
      <h3>Mode</h3>
      <div class="seg" id="modeGroup">
        <button data-mode="DISCREET">IR</button>
        <button data-mode="VISIBLE">RGB</button>
      </div>
    </div>
    <div class="card">
      <h3>Intensity</h3>
      <div class="seg" id="intensityGroup">
        <button data-int="LOW">Lo</button>
        <button data-int="MED">Med</button>
        <button data-int="HIGH">Hi</button>
      </div>
    </div>
  </div>

  <div class="row-2">
    <div class="card" id="colorCard">
      <h3>Color</h3>
      <div class="color-row" id="colorGroup">
        <div class="color red" data-color="RED"></div>
        <div class="color green" data-color="GREEN"></div>
        <div class="color blue" data-color="BLUE"></div>
        <div class="color custom" data-color="CUSTOM" id="customSwatch"></div>
      </div>
      <div class="color-picker-wrap" id="pickerWrap">
        <canvas id="wheelCanvas" width="80" height="80"></canvas><br>
        <input type="range" id="briSlider" min="20" max="100" value="50">
      </div>
    </div>
    <div class="card" id="dazzleCard">
      <h3>Dazzle</h3>
      <div class="dazzle-row">
        <label class="switch"><input type="checkbox" id="dazzle"><span class="slider"></span></label>
        <span>RGB flash</span>
      </div>
    </div>
  </div>

  <div class="card">
    <h3>Speed</h3>
    <div class="wpm-row">
      <div class="wpm-val"><span id="wpmDisplay">10</span> <small>WPM</small></div>
      <div class="wpm-slider">
        <input type="range" id="wpm" min="5" max="20" step="5" value="10">
        <div class="wpm-labels"><span>5</span><span>10</span><span>15</span><span>20</span></div>
      </div>
    </div>
  </div>

</div>
<div class="footer">1984 was not an instruction manual</div>
</div>

<script>
const SPLASH_KEY = 'signet_splash_seen';
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
  briSlider: document.getElementById('briSlider'),
  pickerWrap: document.getElementById('pickerWrap'),
  customSwatch: document.getElementById('customSwatch')
};

async function post(url, body) {
  try {
    const r = await fetch(url, { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body) });
    return r.json();
  } catch(e) { console.error(e); }
}

let wheelHue=0,wheelBri=50;
function hslToRgb(h,s,l){s/=100;l/=100;const k=n=>(n+h/30)%12;const a=s*Math.min(l,1-l);const f=n=>l-a*Math.max(-1,Math.min(k(n)-3,Math.min(9-k(n),1)));return{r:Math.round(f(0)*255),g:Math.round(f(8)*255),b:Math.round(f(4)*255)};}
function drawWheel(){const c=ui.wheelCanvas,ctx=c.getContext('2d'),cx=c.width/2,cy=c.height/2,r=cx-2;for(let a=0;a<360;a++){ctx.beginPath();ctx.moveTo(cx,cy);ctx.arc(cx,cy,r,a*Math.PI/180,(a+2)*Math.PI/180);ctx.closePath();ctx.fillStyle='hsl('+a+',100%,'+wheelBri+'%)';ctx.fill();}}
function pickFromWheel(e){const c=ui.wheelCanvas,rect=c.getBoundingClientRect(),x=e.clientX-rect.left-c.width/2,y=e.clientY-rect.top-c.height/2,d=Math.sqrt(x*x+y*y);if(d>c.width/2)return null;let a=Math.atan2(y,x)*180/Math.PI;if(a<0)a+=360;return Math.round(a);}

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
  const isCustom = c.dataset.color === 'CUSTOM';
  ui.pickerWrap.classList.toggle('visible', isCustom);
  if(isCustom)drawWheel();
  await post('/api/update', { color: c.dataset.color });
}));
ui.wheelCanvas.addEventListener('click', async (e)=>{
  const h=pickFromWheel(e);if(h===null)return;wheelHue=h;
  const rgb=hslToRgb(wheelHue,100,wheelBri);
  ui.customSwatch.style.background='rgb('+rgb.r+','+rgb.g+','+rgb.b+')';
  await post('/api/update',{customR:rgb.r,customG:rgb.g,customB:rgb.b});
});
ui.briSlider.addEventListener('input',()=>{wheelBri=parseInt(ui.briSlider.value);drawWheel();});
ui.briSlider.addEventListener('change', async ()=>{
  const rgb=hslToRgb(wheelHue,100,wheelBri);
  ui.customSwatch.style.background='rgb('+rgb.r+','+rgb.g+','+rgb.b+')';
  await post('/api/update',{customR:rgb.r,customG:rgb.g,customB:rgb.b});
});
ui.dazzle.addEventListener('change', async ()=>{ await post('/api/update', { dazzle: ui.dazzle.checked }); });

ui.wpm.addEventListener('input', ()=>{
  ui.wpmDisplay.textContent = ui.wpm.value;
});
ui.wpm.addEventListener('change', async ()=>{
  await post('/api/update', { wpm: parseInt(ui.wpm.value) });
});

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
drawWheel();
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
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleState(){
  if (captivePortal()) return;
  noteActivity();
  StaticJsonDocument<384> doc;
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
    if (doc.containsKey("customR"))   state.customR   = (uint8_t)doc["customR"].as<int>();
    if (doc.containsKey("customG"))   state.customG   = (uint8_t)doc["customG"].as<int>();
    if (doc.containsKey("customB"))   state.customB   = (uint8_t)doc["customB"].as<int>();
    if (doc.containsKey("dazzle"))    state.dazzle    = doc["dazzle"].as<bool>();
    if (doc.containsKey("wpm")) {
      int newWpm = doc["wpm"].as<int>();
      // Clamp to valid range (5-20)
      if (newWpm < 5) newWpm = 5;
      if (newWpm > 20) newWpm = 20;
      state.wpm = (uint8_t)newWpm;
    }
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
  Serial.println("  (c) 2025 Cunths & Queeths LLC");
  Serial.println("===========================================");
  Serial.println();

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

    // Pulse blue LED while waiting for Web UI connection
    if (!uiServed) {
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
}
