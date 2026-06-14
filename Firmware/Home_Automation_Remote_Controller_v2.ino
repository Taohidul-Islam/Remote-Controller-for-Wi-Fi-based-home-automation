/*
 * ============================================================
 *   Phyxon Remote Controller Firmware v2.0
 *   by Taohid | Phyxon Tech
 *   Hardware: Wemos D1 Mini (ESP8266)
 *   Target: Phyxon Home Automation v2.x (192.168.0.200)
 * ============================================================
 *
 * Changes in v2.0:
 *   - Fan control removed completely
 *   - 1-8: toggle switches 1-8
 *   - A: cycle OLED screen, fix it (no auto-switch after)
 *   - B: cycle OLED screen, keep auto-switch running
 *   - C: toggle LED indicator on main board
 *   - D: all ON if any off, else all OFF
 *   - 9: increase people count
 *   - 0: decrease people count
 *   - #: unused (reserved)
 *   - * + 0 + 1 simultaneously/within 500ms: factory reset main board
 *   - * + 0 + 1 (held 2s): clear remote wifi and restart AP
 *   - # + 0 + 1 simultaneously/within 500ms: OTA update main board
 *   - # + 0 + 1 ... wait thats above
 *     Correction per spec:
 *     * 0 1 → factory reset main board (clears main board settings)
 *     * 0 1 → OTA update main board (from 103.92.206.122:8080)
 *     # 0 1 → OTA update this remote (from 103.92.206.122:8282)
 *
 * Combo Detection:
 *   Three keys pressed one after another within 500ms each triggers combo.
 *   * 0 1 = factory reset main board
 *   # 0 1 = OTA update this remote
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
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <EEPROM.h>
#include <DNSServer.h>

// ─── Pin Definitions ────────────────────────────────────────
#define LED_PIN   15   // D8 - debug LED via NPN (active HIGH)
#define ROW1      16   // D0 - no internal pull-up
#define ROW2       5   // D1
#define ROW3       4   // D2
#define ROW4       0   // D3 - boot pin, HIGH at boot
#define COL1       2   // D4 - boot pin, HIGH at boot
#define COL2      14   // D5
#define COL3      12   // D6
#define COL4      13   // D7

// ─── Network Config ─────────────────────────────────────────
const char*  AP_SSID            = "Phyxon Remote Controller";
IPAddress    AP_IP(192, 168, 5, 1);
IPAddress    AP_GATEWAY(192, 168, 5, 1);
IPAddress    AP_SUBNET(255, 255, 255, 0);
IPAddress    REMOTE_STATIC_IP(192, 168, 0, 150);
IPAddress    HOME_GATEWAY(192, 168, 0, 1);
IPAddress    HOME_SUBNET(255, 255, 255, 0);
IPAddress    HOME_DNS(192, 168, 0, 1);

const char*  HOME_AUTO_IP       = "192.168.0.200";
const int    HOME_AUTO_PORT     = 80;

// OTA servers
const char*  OTA_HOST_MAIN      = "103.92.206.122";
const int    OTA_PORT_MAIN      = 8080;   // main board OTA
const int    OTA_PORT_REMOTE    = 8282;   // remote OTA
const char*  REMOTE_FW_VERSION  = "2.0";

// ─── EEPROM ─────────────────────────────────────────────────
#define EEPROM_SIZE       160
#define EEPROM_MAGIC      0xAB
#define EEPROM_MAGIC_ADDR 0
#define EEPROM_SSID_ADDR  1
#define EEPROM_PASS_ADDR  65

// ─── State Machine ──────────────────────────────────────────
enum RemoteState { STATE_AP, STATE_CONNECTING, STATE_CONNECTED };
RemoteState currentState = STATE_AP;

ESP8266WebServer server(80);
DNSServer        dnsServer;

// ─── LED ────────────────────────────────────────────────────
unsigned long ledTimer     = 0;
int           ledBlinkStep = 0;
bool          ledState_    = false;
bool          feedbackActive = false;
int           feedbackCount  = 0;
unsigned long feedbackTimer  = 0;

void ledOn()  { digitalWrite(LED_PIN, HIGH); ledState_=true;  }
void ledOff() { digitalWrite(LED_PIN, LOW);  ledState_=false; }

void updateLED() {
  unsigned long now = millis();
  if(feedbackActive) {
    if(now-feedbackTimer >= 60) {
      feedbackTimer=now; ledState_=!ledState_;
      digitalWrite(LED_PIN, ledState_?HIGH:LOW);
      if(--feedbackCount<=0) {
        feedbackActive=false; ledOff();
        ledTimer=now; ledBlinkStep=0;
      }
    }
    return;
  }
  switch(currentState) {
    case STATE_AP: {
      unsigned long iv[]={80,80,80,760};
      if(now-ledTimer>=iv[ledBlinkStep]) {
        ledTimer=now; ledBlinkStep=(ledBlinkStep+1)%4;
        (ledBlinkStep==1||ledBlinkStep==3)?ledOff():ledOn();
      }
    } break;
    case STATE_CONNECTING: {
      unsigned long iv[]={100,900};
      if(now-ledTimer>=iv[ledBlinkStep]) {
        ledTimer=now; ledBlinkStep=(ledBlinkStep+1)%2;
        ledBlinkStep==0?ledOn():ledOff();
      }
    } break;
    case STATE_CONNECTED: ledOff(); break;
  }
}

void triggerFeedback(bool success) {
  feedbackCount  = success ? 4 : 2;
  feedbackActive = true;
  feedbackTimer  = millis()-61;
  ledState_      = false;
}

void rapidBlink(int times) {
  for(int i=0;i<times*2;i++) {
    digitalWrite(LED_PIN, i%2==0?HIGH:LOW);
    delay(80);
  }
  ledOff();
}

// ─── Keypad ─────────────────────────────────────────────────
const int ROWS=4, COLS=4;
int rowPins[ROWS]={ROW1,ROW2,ROW3,ROW4};
int colPins[COLS]={COL1,COL2,COL3,COL4};
const char keyMap[ROWS][COLS]={
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

char          lastKey      = 0;
unsigned long keyPressTime = 0;

char scanKeypad() {
  for(int r=0;r<ROWS;r++) {
    pinMode(rowPins[r],OUTPUT);
    digitalWrite(rowPins[r],LOW);
    for(int c=0;c<COLS;c++) {
      if(digitalRead(colPins[c])==LOW) {
        digitalWrite(rowPins[r],HIGH);
        pinMode(rowPins[r],INPUT);
        return keyMap[r][c];
      }
    }
    digitalWrite(rowPins[r],HIGH);
    pinMode(rowPins[r],INPUT);
  }
  return 0;
}

char getKey() {
  char k=scanKeypad();
  if(k!=0) {
    if(k!=lastKey) { lastKey=k; keyPressTime=millis(); return k; }
    return 0;
  }
  lastKey=0; return 0;
}

// ─── Combo Sequence Detection ────────────────────────────────
// Detects 3-key sequences pressed within 500ms each
// Returns: 0=none, 1=*01 (reset main), 2=#01 (OTA remote)
#define COMBO_GAP_MS 500

char          comboBuffer[3]  = {0,0,0};
int           comboIdx        = 0;
unsigned long comboTimer      = 0;

// Combos:
// * 0 1 → factory reset main board
// # 0 1 → OTA update this remote
const char COMBO_RESET[3]  = {'*','0','1'};
const char COMBO_REMOTE[3] = {'#','0','1'};

int checkCombo(char k) {
  unsigned long now=millis();
  // Reset buffer if too slow
  if(comboIdx>0 && (now-comboTimer)>COMBO_GAP_MS) comboIdx=0;

  comboBuffer[comboIdx]=k;
  comboTimer=now;
  comboIdx++;

  if(comboIdx==3) {
    comboIdx=0;
    // Check * 0 1
    if(comboBuffer[0]==COMBO_RESET[0] &&
       comboBuffer[1]==COMBO_RESET[1] &&
       comboBuffer[2]==COMBO_RESET[2]) return 1;
    // Check # 0 1
    if(comboBuffer[0]==COMBO_REMOTE[0] &&
       comboBuffer[1]==COMBO_REMOTE[1] &&
       comboBuffer[2]==COMBO_REMOTE[2]) return 2;
  }
  return 0;
}

// ─── EEPROM ─────────────────────────────────────────────────
void eepromWriteString(int addr, const String& s, int maxLen) {
  int len=min((int)s.length(),maxLen-1);
  for(int i=0;i<len;i++) EEPROM.write(addr+i,s[i]);
  EEPROM.write(addr+len,0); EEPROM.commit();
}
String eepromReadString(int addr, int maxLen) {
  String r="";
  for(int i=0;i<maxLen;i++) {
    char c=(char)EEPROM.read(addr+i);
    if(c==0) break; r+=c;
  }
  return r;
}
bool loadCredentials(String& ssid, String& pass) {
  if(EEPROM.read(EEPROM_MAGIC_ADDR)!=EEPROM_MAGIC) return false;
  ssid=eepromReadString(EEPROM_SSID_ADDR,64);
  pass=eepromReadString(EEPROM_PASS_ADDR,64);
  return ssid.length()>0;
}
void saveCredentials(const String& ssid, const String& pass) {
  EEPROM.write(EEPROM_MAGIC_ADDR,EEPROM_MAGIC);
  eepromWriteString(EEPROM_SSID_ADDR,ssid,64);
  eepromWriteString(EEPROM_PASS_ADDR,pass,64);
}
void clearCredentials() {
  EEPROM.write(EEPROM_MAGIC_ADDR,0); EEPROM.commit();
}

// ─── HTTP Helpers ────────────────────────────────────────────
bool httpPost(const String& path) {
  if(WiFi.status()!=WL_CONNECTED) return false;
  WiFiClient client; HTTPClient http;
  http.begin(client,"http://"+String(HOME_AUTO_IP)+path);
  http.setTimeout(2000);
  int code=http.POST(""); http.end();
  Serial.println("[HTTP] POST "+path+" → "+String(code));
  return (code==200);
}

bool httpGet(const String& path, String& response) {
  if(WiFi.status()!=WL_CONNECTED) return false;
  WiFiClient client; HTTPClient http;
  http.begin(client,"http://"+String(HOME_AUTO_IP)+path);
  http.setTimeout(2000);
  int code=http.GET();
  if(code==200) response=http.getString();
  http.end();
  return (code==200);
}

// ─── Relay State Cache ───────────────────────────────────────
bool relayStates[8]  = {false};
bool statesFetched   = false;

void fetchStatus() {
  String resp;
  if(!httpGet("/status",resp)) return;
  int start=resp.indexOf("\"relays\":[");
  if(start<0) return;
  start+=10;
  for(int i=0;i<8;i++) {
    int t=resp.indexOf("true",start);
    int f=resp.indexOf("false",start);
    if(t<0&&f<0) break;
    bool isFalse=(f>=0&&(t<0||f<t));
    relayStates[i]=!isFalse;
    start=isFalse?f+5:t+4;
  }
  statesFetched=true;
  Serial.println("[STATUS] Relay states fetched");
}

// ─── OTA Update Main Board ───────────────────────────────────
void otaUpdateMainBoard() {
  Serial.println("[OTA] Triggering main board update...");
  rapidBlink(3);
  bool ok=httpPost("/doupdate");
  triggerFeedback(ok);
  Serial.println("[OTA] Main board update request: "+(String)(ok?"OK":"FAIL"));
}

// ─── OTA Update This Remote ──────────────────────────────────
void otaUpdateRemote() {
  Serial.println("[OTA] Updating remote firmware...");
  rapidBlink(5);
  String url="http://"+String(OTA_HOST_MAIN)+":"+String(OTA_PORT_REMOTE)+"/firmware.bin";
  t_httpUpdate_return ret=ESPhttpUpdate.update(url);
  switch(ret) {
    case HTTP_UPDATE_FAILED:
      Serial.println("[OTA] Failed: "+ESPhttpUpdate.getLastErrorString());
      rapidBlink(2); break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] No updates"); break;
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] Done, rebooting..."); break;
  }
}

// ─── Key Actions ────────────────────────────────────────────
void toggleRelay(int idx) {
  bool ok=httpPost("/toggle?relay="+String(idx));
  if(ok) relayStates[idx]=!relayStates[idx];
  triggerFeedback(ok);
}

int getPeopleCount() {
  String resp;
  if(!httpGet("/status",resp)) return -1;
  int pi=resp.indexOf("\"people\":");
  if(pi<0) return -1;
  return resp.substring(pi+9,pi+12).toInt();
}

void handleKey(char k) {
  Serial.println("[KEY] "+String(k));
  if(currentState!=STATE_CONNECTED) return;

  switch(k) {
    // Switches 1-8
    case '1': toggleRelay(0); break;
    case '2': toggleRelay(1); break;
    case '3': toggleRelay(2); break;
    case '4': toggleRelay(3); break;
    case '5': toggleRelay(4); break;
    case '6': toggleRelay(5); break;
    case '7': toggleRelay(6); break;
    case '8': toggleRelay(7); break;

    // 9 = increase people count
    case '9': {
      int c=getPeopleCount();
      if(c>=0) {
        bool ok=httpPost("/setpeople?count="+String(c+1));
        triggerFeedback(ok);
      }
      break;
    }

    // 0 = decrease people count
    case '0': {
      int c=getPeopleCount();
      if(c>0) {
        bool ok=httpPost("/setpeople?count="+String(c-1));
        triggerFeedback(ok);
      }
      break;
    }

    // A = cycle OLED screen and fix it (no auto-switch)
    case 'A': {
      bool ok=httpPost("/olednext?fix=1");
      triggerFeedback(ok);
      break;
    }

    // B = cycle OLED screen, keep auto-switch
    case 'B': {
      bool ok=httpPost("/olednext?fix=0");
      triggerFeedback(ok);
      break;
    }

    // C = toggle LED indicator
    case 'C': {
      bool ok=httpPost("/toggleled");
      triggerFeedback(ok);
      break;
    }

    // D = all ON if any off, else all OFF
    case 'D': {
      if(!statesFetched) fetchStatus();
      bool anyOff=false;
      for(int i=0;i<8;i++) if(!relayStates[i]){anyOff=true;break;}
      bool ok=httpPost(anyOff?"/allrelays?state=1":"/allrelays?state=0");
      if(ok) for(int i=0;i<8;i++) relayStates[i]=anyOff;
      triggerFeedback(ok);
      break;
    }

    // * # handled in combo detection in main loop
    case '*': break;
    case '#': break;

    default: break;
  }
}

// ─── Web Server Pages ────────────────────────────────────────
static const char CONFIG_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<meta charset='UTF-8'><title>Phyxon Remote Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#e0e0e0;
     display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{background:#16213e;border-radius:12px;padding:30px;text-align:center;max-width:360px;width:90%}
h2{color:#4ade80;margin-bottom:8px;font-size:1.3rem}
p{color:#aaa;font-size:.85rem;margin-bottom:20px}
input{width:100%;padding:10px;border-radius:8px;border:1px solid #2a2a4a;
      background:#0f3460;color:#e0e0e0;font-size:.9rem;margin-bottom:12px}
button{width:100%;padding:12px;border:none;border-radius:8px;background:#4ade80;
       color:#111;font-weight:700;font-size:.95rem;cursor:pointer}
</style></head><body>
<div class='card'>
  <h2>Phyxon Remote v2.0</h2>
  <p>Enter your home WiFi credentials to connect</p>
  <form action='/savewifi' method='post'>
    <input name='ssid' placeholder='WiFi SSID' required>
    <input name='password' type='password' placeholder='Password'>
    <button type='submit'>Connect</button>
  </form>
</div></body></html>
)rawhtml";

static const char SAVED_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Saved</title>
<style>
body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#e0e0e0;
     display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{background:#16213e;border-radius:12px;padding:30px;text-align:center;max-width:360px;width:90%}
h2{color:#4ade80;margin-bottom:12px}p{color:#aaa;font-size:.85rem;line-height:1.6}
</style></head><body>
<div class='card'>
  <h2>&#x2714; Saved!</h2>
  <p>Credentials saved.<br>Restarting and connecting...<br><br>
  Remote will be at <strong>192.168.0.150</strong></p>
</div></body></html>
)rawhtml";

void handleRoot()    { server.send_P(200,"text/html",CONFIG_PAGE); }
void handleCaptive() { server.sendHeader("Location",String("http://")+AP_IP.toString()+"/",true); server.send(302,"text/plain",""); }
void handleSaveWifi(){
  if(server.hasArg("ssid")&&server.arg("ssid").length()>0){
    saveCredentials(server.arg("ssid"),server.arg("password"));
    server.send_P(200,"text/html",SAVED_PAGE);
    delay(2000); ESP.restart();
  } else { server.sendHeader("Location","/",true); server.send(302,"text/plain",""); }
}

// ─── AP Mode ────────────────────────────────────────────────
void startAPMode() {
  currentState=STATE_AP; ledBlinkStep=0; ledTimer=millis();
  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP,AP_GATEWAY,AP_SUBNET);
  WiFi.softAP(AP_SSID);
  dnsServer.start(53,"*",AP_IP);
  server.on("/",HTTP_GET,handleRoot);
  server.on("/savewifi",HTTP_POST,handleSaveWifi);
  server.onNotFound(handleCaptive);
  server.begin();
  Serial.println("[AP] "+String(AP_SSID));
}

// ─── WiFi Connect ────────────────────────────────────────────
bool connectToHomeWifi(const String& ssid, const String& pass) {
  currentState=STATE_CONNECTING; ledBlinkStep=0; ledTimer=millis();
  server.stop(); dnsServer.stop();
  WiFi.mode(WIFI_STA);
  WiFi.config(REMOTE_STATIC_IP,HOME_GATEWAY,HOME_SUBNET,HOME_DNS);
  WiFi.begin(ssid.c_str(),pass.c_str());
  Serial.print("[WiFi] Connecting to "+ssid);
  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED) {
    updateLED(); delay(10);
    if(millis()-start>15000){ Serial.println("\n[WiFi] Timeout"); return false; }
  }
  Serial.println("\n[WiFi] Connected: "+WiFi.localIP().toString());
  currentState=STATE_CONNECTED; ledOff();
  delay(500); fetchStatus();
  return true;
}

// ─── Setup ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("\n=== Phyxon Remote v2.0 ===");
  EEPROM.begin(EEPROM_SIZE);
  pinMode(LED_PIN,OUTPUT); ledOff();
  pinMode(COL1,INPUT_PULLUP); pinMode(COL2,INPUT_PULLUP);
  pinMode(COL3,INPUT_PULLUP); pinMode(COL4,INPUT_PULLUP);
  pinMode(ROW1,INPUT); pinMode(ROW2,INPUT);
  pinMode(ROW3,INPUT); pinMode(ROW4,INPUT);
  String savedSSID,savedPass;
  if(loadCredentials(savedSSID,savedPass)){
    Serial.println("[EEPROM] Found: "+savedSSID);
    if(!connectToHomeWifi(savedSSID,savedPass)){
      Serial.println("[WiFi] Failed, AP mode");
      startAPMode();
    }
  } else {
    Serial.println("[EEPROM] No creds, AP mode");
    startAPMode();
  }
}

// ─── Loop ────────────────────────────────────────────────────
void loop() {
  updateLED();
  if(currentState==STATE_AP){
    dnsServer.processNextRequest();
    server.handleClient();
  }

  char k=getKey();
  if(k!=0) {
    int combo=checkCombo(k);
    if(combo==1) {
      // * 0 1 = factory reset main board
      Serial.println("[COMBO] Factory reset main board");
      rapidBlink(5);
      httpPost("/factoryreset");
      delay(500);
    } else if(combo==2) {
      // # 0 1 = OTA update this remote
      Serial.println("[COMBO] OTA update remote");
      otaUpdateRemote();
    } else {
      handleKey(k);
    }
  }

  yield();
}
