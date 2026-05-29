/*
 * Phyxon Remote Controller Firmware
 * Hardware: Wemos D1 Mini (ESP8266)
 * Target: Phyxon Tech V1 Home Automation (192.168.0.200)
 *
 * Pin Mapping:
 *   Debug LED  : D8 / GPIO15 (NPN transistor, active HIGH)
 *   Keypad Rows: D0/GPIO16=R1, D1/GPIO5=R2, D2/GPIO4=R3, D3/GPIO0=R4
 *   Keypad Cols: D4/GPIO2=C1, D5/GPIO14=C2, D6/GPIO12=C3, D7/GPIO13=C4
 *
 * Keypad Layout:
 *   [ 1 ][ 2 ][ 3 ][ A ]   R1
 *   [ 4 ][ 5 ][ 6 ][ B ]   R2
 *   [ 7 ][ 8 ][ 9 ][ C ]   R3
 *   [ * ][ 0 ][ # ][ D ]   R4
 *    C1   C2   C3   C4
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
#include <DNSServer.h>

// ─── Pin Definitions ───────────────────────────────────────────────────────
#define LED_PIN     15   // D8 - debug LED via NPN transistor (active HIGH)

// Keypad rows (driven LOW one at a time to scan)
#define ROW1        16   // D0 - no internal pull-up, external needed
#define ROW2         5   // D1
#define ROW3         4   // D2
#define ROW4         0   // D3 - boot pin, must be HIGH at boot

// Keypad columns (read with internal pull-ups)
#define COL1         2   // D4 - boot pin, must be HIGH at boot
#define COL2        14   // D5
#define COL3        12   // D6
#define COL4        13   // D7

// ─── Network Config ────────────────────────────────────────────────────────
const char*   AP_SSID       = "Phyxon Remote Controller";
const char*   AP_PASSWORD   = "";                    // Open network
IPAddress     AP_IP(192, 168, 5, 1);
IPAddress     AP_GATEWAY(192, 168, 5, 1);
IPAddress     AP_SUBNET(255, 255, 255, 0);

IPAddress     REMOTE_STATIC_IP(192, 168, 0, 150);   // Remote's static IP on home WiFi
IPAddress     HOME_GATEWAY(192, 168, 0, 1);
IPAddress     HOME_SUBNET(255, 255, 255, 0);
IPAddress     HOME_DNS(192, 168, 0, 1);

const char*   HOME_AUTO_IP  = "192.168.0.200";       // Main automation board IP
const int     HOME_AUTO_PORT = 80;

// ─── EEPROM Layout ─────────────────────────────────────────────────────────
#define EEPROM_SIZE         160
#define EEPROM_MAGIC        0xAB
#define EEPROM_MAGIC_ADDR   0
#define EEPROM_SSID_ADDR    1    // up to 64 bytes
#define EEPROM_PASS_ADDR    65   // up to 64 bytes

// ─── State Machine ─────────────────────────────────────────────────────────
enum RemoteState {
  STATE_AP,           // Hosting config AP, waiting for credentials
  STATE_CONNECTING,   // Trying to join home WiFi
  STATE_CONNECTED     // Connected, controlling home automation
};

RemoteState currentState = STATE_AP;

// ─── LED Blink Patterns ────────────────────────────────────────────────────
// AP mode      : double-blink every 1s  (on 80ms, off 80ms, on 80ms, off ~760ms)
// Connecting   : single blink every 1s  (on 100ms, off 900ms)
// Connected    : LED OFF
// Toggle ON    : two fast blinks         (on 60ms off 60ms x2)
// Toggle OFF   : one fast blink          (on 60ms off 60ms x1)

unsigned long ledTimer       = 0;
int           ledBlinkStep   = 0;
bool          ledState       = false;

// For transient feedback blinks (toggle on/off)
bool          feedbackActive = false;
int           feedbackCount  = 0;   // total half-cycles remaining
unsigned long feedbackTimer  = 0;

void ledOn()  { digitalWrite(LED_PIN, HIGH); ledState = true;  }
void ledOff() { digitalWrite(LED_PIN, LOW);  ledState = false; }

// Call every loop tick — handles all LED patterns non-blocking
void updateLED() {
  unsigned long now = millis();

  // Transient feedback blinks take priority
  if (feedbackActive) {
    if (now - feedbackTimer >= 60) {
      feedbackTimer = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      feedbackCount--;
      if (feedbackCount <= 0) {
        feedbackActive = false;
        ledOff();
        ledTimer = now;     // reset pattern timer
        ledBlinkStep = 0;
      }
    }
    return;
  }

  switch (currentState) {

    case STATE_AP:
      // Double-blink pattern: ON-OFF-ON-OFF ... period ~1000ms
      // Steps: 0=on(80ms) 1=off(80ms) 2=on(80ms) 3=off(760ms)
      {
        unsigned long intervals[] = {80, 80, 80, 760};
        if (now - ledTimer >= intervals[ledBlinkStep]) {
          ledTimer = now;
          ledBlinkStep = (ledBlinkStep + 1) % 4;
          // Steps 0 and 2 are ON, 1 and 3 are OFF
          (ledBlinkStep == 1 || ledBlinkStep == 3) ? ledOff() : ledOn();
        }
      }
      break;

    case STATE_CONNECTING:
      // Single blink: ON 100ms, OFF 900ms
      {
        unsigned long intervals[] = {100, 900};
        if (now - ledTimer >= intervals[ledBlinkStep]) {
          ledTimer = now;
          ledBlinkStep = (ledBlinkStep + 1) % 2;
          ledBlinkStep == 0 ? ledOn() : ledOff();
        }
      }
      break;

    case STATE_CONNECTED:
      ledOff();
      break;
  }
}

// Trigger transient feedback (toggleOn=true → 2 blinks, false → 1 blink)
void triggerFeedback(bool toggleOn) {
  feedbackCount  = toggleOn ? 4 : 2;  // each blink = 2 half-cycles
  feedbackActive = true;
  feedbackTimer  = millis() - 61;     // fire immediately
  ledState       = false;
}

// ─── Keypad ────────────────────────────────────────────────────────────────
const int ROWS = 4;
const int COLS = 4;

int rowPins[ROWS] = {ROW1, ROW2, ROW3, ROW4};
int colPins[COLS] = {COL1, COL2, COL3, COL4};

// Key map matching physical layout
const char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

char lastKey        = 0;
unsigned long keyPressTime = 0;
#define KEY_DEBOUNCE_MS  50
#define KEY_HOLD_IGNORE_MS 300   // ignore held keys

// Returns pressed key char or 0 if none
char scanKeypad() {
  for (int r = 0; r < ROWS; r++) {
    // Drive this row LOW
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], LOW);

    for (int c = 0; c < COLS; c++) {
      if (digitalRead(colPins[c]) == LOW) {
        // Key at [r][c] is pressed
        // Release row before returning
        digitalWrite(rowPins[r], HIGH);
        pinMode(rowPins[r], INPUT);
        return keyMap[r][c];
      }
    }

    // Release row
    digitalWrite(rowPins[r], HIGH);
    pinMode(rowPins[r], INPUT);
  }
  return 0;
}

// Returns a key only on the rising edge (press moment), not while held
char getKey() {
  char k = scanKeypad();
  unsigned long now = millis();

  if (k != 0) {
    if (k != lastKey) {
      lastKey = k;
      keyPressTime = now;
      return k;   // fresh press
    }
    // Same key still held — ignore
    return 0;
  } else {
    lastKey = 0;
    return 0;
  }
}

// ─── Reset Sequence Detection ──────────────────────────────────────────────
// * 0 # D pressed in sequence within MAX_SEQ_GAP_MS each
const char RESET_SEQ[]       = {'*', '0', '#', 'D'};
const int  RESET_SEQ_LEN     = 4;
const unsigned long MAX_SEQ_GAP_MS = 2000;

int           resetSeqIndex  = 0;
unsigned long resetSeqTimer  = 0;

// Feed a key into the reset sequence detector; returns true when sequence complete
bool checkResetSequence(char k) {
  unsigned long now = millis();

  if (resetSeqIndex > 0 && (now - resetSeqTimer) > MAX_SEQ_GAP_MS) {
    resetSeqIndex = 0;  // timed out, restart
  }

  if (k == RESET_SEQ[resetSeqIndex]) {
    resetSeqIndex++;
    resetSeqTimer = now;
    if (resetSeqIndex >= RESET_SEQ_LEN) {
      resetSeqIndex = 0;
      return true;
    }
  } else {
    // Wrong key — restart from beginning, but check if it matches index 0
    resetSeqIndex = (k == RESET_SEQ[0]) ? 1 : 0;
    resetSeqTimer = now;
  }
  return false;
}

// ─── EEPROM Helpers ────────────────────────────────────────────────────────
void eepromWriteString(int addr, const String& s, int maxLen) {
  int len = min((int)s.length(), maxLen - 1);
  for (int i = 0; i < len; i++) EEPROM.write(addr + i, s[i]);
  EEPROM.write(addr + len, 0);
  EEPROM.commit();
}

String eepromReadString(int addr, int maxLen) {
  String result = "";
  for (int i = 0; i < maxLen; i++) {
    char c = (char)EEPROM.read(addr + i);
    if (c == 0) break;
    result += c;
  }
  return result;
}

void saveCredentials(const String& ssid, const String& pass) {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  eepromWriteString(EEPROM_SSID_ADDR, ssid, 64);
  eepromWriteString(EEPROM_PASS_ADDR, pass, 64);
  EEPROM.commit();
}

void clearCredentials() {
  EEPROM.write(EEPROM_MAGIC_ADDR, 0xFF);
  EEPROM.commit();
}

bool loadCredentials(String& ssid, String& pass) {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC) return false;
  ssid = eepromReadString(EEPROM_SSID_ADDR, 64);
  pass = eepromReadString(EEPROM_PASS_ADDR, 64);
  return ssid.length() > 0;
}

// ─── Web Server & DNS ──────────────────────────────────────────────────────
ESP8266WebServer server(80);
DNSServer        dnsServer;

// Minimal captive portal + config page HTML
const char CONFIG_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
  <title>Phyxon Remote Setup</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;
         display:flex;align-items:center;justify-content:center;
         min-height:100vh;padding:20px}
    .card{background:#16213e;border-radius:12px;padding:30px;
          width:100%;max-width:400px;box-shadow:0 4px 20px rgba(0,0,0,0.4)}
    h1{text-align:center;color:#e94560;margin-bottom:6px;font-size:22px}
    p.sub{text-align:center;color:#888;font-size:13px;margin-bottom:24px}
    label{display:block;margin-bottom:6px;font-size:14px;color:#aaa}
    input{width:100%;padding:12px;border:1px solid #333;border-radius:8px;
          background:#0f3460;color:#fff;font-size:15px;margin-bottom:16px}
    input:focus{outline:none;border-color:#e94560}
    button{width:100%;padding:13px;background:#e94560;color:#fff;
           border:none;border-radius:8px;font-size:16px;font-weight:bold;
           cursor:pointer;transition:background 0.2s}
    button:hover{background:#c73652}
    .note{margin-top:18px;font-size:12px;color:#666;text-align:center}
  </style>
</head>
<body>
  <div class="card">
    <h1>&#x1F4F6; Phyxon Remote</h1>
    <p class="sub">Connect to your home WiFi to start controlling</p>
    <form method="POST" action="/savewifi">
      <label>WiFi Network (SSID)</label>
      <input type="text" name="ssid" placeholder="Your WiFi name" required autocomplete="off">
      <label>Password</label>
      <input type="password" name="password" placeholder="WiFi password" autocomplete="off">
      <button type="submit">Connect &amp; Save</button>
    </form>
    <p class="note">Remote will connect to home WiFi at 192.168.0.150<br>
    and control the automation board at 192.168.0.200</p>
  </div>
</body>
</html>
)rawhtml";

const char SAVED_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Phyxon Remote</title>
  <style>
    body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;
         display:flex;align-items:center;justify-content:center;min-height:100vh}
    .card{background:#16213e;border-radius:12px;padding:30px;text-align:center;
          max-width:360px;width:90%}
    h2{color:#4ecca3;margin-bottom:12px}
    p{color:#aaa;font-size:14px;line-height:1.6}
  </style>
</head>
<body>
  <div class="card">
    <h2>&#x2714; Saved!</h2>
    <p>Credentials saved.<br>
    The remote will now restart and connect to your home WiFi.<br><br>
    It will be available at <strong>192.168.0.150</strong><br>
    Use the keypad to control your switches.</p>
  </div>
</body>
</html>
)rawhtml";

void handleRoot() {
  server.send_P(200, "text/html", CONFIG_PAGE);
}

void handleSaveWifi() {
  if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
    String ssid = server.arg("ssid");
    String pass = server.arg("password");
    saveCredentials(ssid, pass);
    server.send_P(200, "text/html", SAVED_PAGE);
    delay(2000);
    ESP.restart();
  } else {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  }
}

// Captive portal redirect — catch all unknown hosts
void handleCaptivePortal() {
  server.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
  server.send(302, "text/plain", "");
}

void startAPMode() {
  currentState = STATE_AP;
  ledBlinkStep = 0;
  ledTimer = millis();

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID);   // open network, no password

  // DNS: redirect everything to our IP (captive portal)
  dnsServer.start(53, "*", AP_IP);

  server.on("/", HTTP_GET,  handleRoot);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.onNotFound(handleCaptivePortal);
  server.begin();

  Serial.println("[AP] Hosting: " + String(AP_SSID));
  Serial.println("[AP] Config portal: http://" + AP_IP.toString());
}

// ─── WiFi Connection ───────────────────────────────────────────────────────
bool connectToHomeWifi(const String& ssid, const String& pass) {
  currentState = STATE_CONNECTING;
  ledBlinkStep = 0;
  ledTimer = millis();

  server.stop();
  dnsServer.stop();
  WiFi.mode(WIFI_STA);
  WiFi.config(REMOTE_STATIC_IP, HOME_GATEWAY, HOME_SUBNET, HOME_DNS);
  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.print("[WiFi] Connecting to: " + ssid);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    updateLED();   // keep LED blinking while waiting
    delay(10);
    if (millis() - start > 15000) {   // 15s timeout
      Serial.println("\n[WiFi] Connection timed out.");
      return false;
    }
  }

  Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
  currentState = STATE_CONNECTED;
  ledOff();
  return true;
}

// ─── HTTP Requests to Main Board ───────────────────────────────────────────
bool httpPost(const String& path) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = "http://" + String(HOME_AUTO_IP) + path;

  http.begin(client, url);
  http.setTimeout(2000);
  int code = http.POST("");
  http.end();

  Serial.println("[HTTP] POST " + url + " → " + String(code));
  return (code == 200);
}

void toggleRelay(int relayIndex) {
  // relayIndex 0–7
  bool ok = httpPost("/toggle?relay=" + String(relayIndex));
  if (ok) triggerFeedback(true);   
  // Note: we don't know the new state from a blind toggle,
  // so we always do double-blink (toggled). Adjust if you add /status polling.
}

void toggleFan() {
  httpPost("/fanon");
}

void setFanLevel(int level) {
  // level 1, 2, or 3
  httpPost("/fanlevel?level=" + String(level));
}

// ─── Fan State Tracking ────────────────────────────────────────────────────
// We track fan on/off locally so we know when A/B/C are valid.
// Starts as unknown (false). Gets toggled by button 9.
// If power cycles we lose track — acceptable for a remote.
bool fanIsOn = false;

// ─── Key Action Handler ────────────────────────────────────────────────────
void handleKey(char k) {
  Serial.println("[KEY] Pressed: " + String(k));

  if (currentState != STATE_CONNECTED) return;

  switch (k) {
    // ── Switches 1–8 ──────────────────────────────────────────
    case '1': toggleRelay(0); triggerFeedback(true); break;
    case '2': toggleRelay(1); triggerFeedback(true); break;
    case '3': toggleRelay(2); triggerFeedback(true); break;
    case '4': toggleRelay(3); triggerFeedback(true); break;
    case '5': toggleRelay(4); triggerFeedback(true); break;
    case '6': toggleRelay(5); triggerFeedback(true); break;
    case '7': toggleRelay(6); triggerFeedback(true); break;
    case '8': toggleRelay(7); triggerFeedback(true); break;

    // ── Fan on/off ─────────────────────────────────────────────
    case '9':
      toggleFan();
      fanIsOn = !fanIsOn;
      triggerFeedback(fanIsOn);
      if (fanIsOn) {
        Serial.println("[FAN] Turned ON (level reset to 0 on main board)");
      } else {
        Serial.println("[FAN] Turned OFF");
      }
      break;

    // ── Fan speed levels (only when fan is ON) ─────────────────
    case 'A':
      if (fanIsOn) { setFanLevel(1); triggerFeedback(true); Serial.println("[FAN] Level 1"); }
      else          { Serial.println("[FAN] Ignored A — fan is off"); }
      break;
    case 'B':
      if (fanIsOn) { setFanLevel(2); triggerFeedback(true); Serial.println("[FAN] Level 2"); }
      else          { Serial.println("[FAN] Ignored B — fan is off"); }
      break;
    case 'C':
      if (fanIsOn) { setFanLevel(3); triggerFeedback(true); Serial.println("[FAN] Level 3"); }
      else          { Serial.println("[FAN] Ignored C — fan is off"); }
      break;

    // ── Reset sequence keys (* 0 # D) handled in main loop ────
    // (they fall through to checkResetSequence below)
    default:
      break;
  }
}

// ─── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\n=== Phyxon Remote Controller ===");

  EEPROM.begin(EEPROM_SIZE);

  // ── LED pin ──
  pinMode(LED_PIN, OUTPUT);
  ledOff();

  // ── Keypad columns: INPUT_PULLUP (HIGH by default, LOW when row pulled down) ──
  pinMode(COL1, INPUT_PULLUP);
  pinMode(COL2, INPUT_PULLUP);
  pinMode(COL3, INPUT_PULLUP);
  pinMode(COL4, INPUT_PULLUP);

  // ── Keypad rows: start as INPUT (high-Z), driven LOW only during scan ──
  // ROW1 = GPIO16 has no internal pull-up; column pull-ups handle the idle state
  pinMode(ROW1, INPUT);
  pinMode(ROW2, INPUT);
  pinMode(ROW3, INPUT);
  pinMode(ROW4, INPUT);

  // ── Try to load saved WiFi credentials ──
  String savedSSID, savedPass;
  if (loadCredentials(savedSSID, savedPass)) {
    Serial.println("[EEPROM] Found credentials for: " + savedSSID);
    if (!connectToHomeWifi(savedSSID, savedPass)) {
      Serial.println("[WiFi] Failed — falling back to AP mode");
      startAPMode();
    }
  } else {
    Serial.println("[EEPROM] No credentials — starting AP mode");
    startAPMode();
  }
}

// ─── Main Loop ─────────────────────────────────────────────────────────────
void loop() {
  updateLED();

  // Handle AP web server & captive portal DNS
  if (currentState == STATE_AP) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  // ── Keypad scan ──
  char k = getKey();

  if (k != 0) {
    // Always feed reset sequence detector first
    if (checkResetSequence(k)) {
      Serial.println("[RESET] Sequence detected! Clearing credentials and restarting AP...");
      clearCredentials();
      // Visual confirmation: rapid 5 blinks
      for (int i = 0; i < 10; i++) {
        digitalWrite(LED_PIN, i % 2 == 0 ? HIGH : LOW);
        delay(80);
      }
      ledOff();
      delay(300);
      ESP.restart();
      return;
    }

    // Then handle normal key actions
    handleKey(k);
  }

  // Small yield to keep ESP8266 stack happy
  yield();
}
