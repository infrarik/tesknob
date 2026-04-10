#include <M5GFX.h>

/*
 * ============================================================
 *  Tesla CAN Controller – M5Stack DIAL  v1.0
 *  ESP32-S3 + SN65HVD230 (PORT.B)
 *
 *  PINOUT M5DIAL
 *  Ecran GC9A01 : MOSI=5 SCLK=6 CS=7 DC=4 RST=8 BL=9
 *  Touch FT3267 : SDA=11 SCL=12 addr=0x38
 *  Encoder      : A=40 B=41
 *  Bouton front : GPIO42
 *  HOLD power   : GPIO46
 *  Buzzer       : GPIO3
 *  CAN TX       : GPIO2  (PORT.B)
 *  CAN RX       : GPIO1  (PORT.B)
 *
 *  BOARD    : ESP32S3 Dev Module
 *  LIBS     : M5GFX, ESP32 (TWAI inclus)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <driver/twai.h>
#include <M5GFX.h>

// ─── HOLD POWER ───────────────────────────────────────────────
#define PIN_HOLD   46

// ─── ECRAN GC9A01 240x240 ────────────────────────────────────
M5GFX   display;
M5Canvas canvas(&display);

// ─── ENCODER + BOUTON ────────────────────────────────────────
#define ENC_A      40
#define ENC_B      41
#define ENC_BTN    42
volatile int  encCount  = 0;
volatile int  encLast   = 0;
bool     btnPrev        = HIGH;
uint32_t btnPressedAt   = 0;
uint32_t lastClickAt    = 0;
bool     waitDblClick   = false;
#define LONG_PRESS_MS  800
#define DBLCLICK_MS    350

// ─── BUZZER ───────────────────────────────────────────────────
#define PIN_BUZZ   3

// ─── CAN TWAI ─────────────────────────────────────────────────
#define CAN_TX_PIN  GPIO_NUM_2
#define CAN_RX_PIN  GPIO_NUM_1

// ─── BATTERY HEAT ─────────────────────────────────────────────
bool     battHeatActive = false;
uint32_t lastHeatMs     = 0;

#define FW_VERSION "v1.0"

// ═══════════════════════════════════════════════════════════════
//  ACTIONS DISPONIBLES
// ═══════════════════════════════════════════════════════════════

#define C_RED    0xE800
#define C_DARK   0x18C3
#define C_GREEN  0x07E0
#define C_ORANGE 0xFD20
#define C_BLUE   0x001F
#define C_TEAL   0x07FF
#define C_PURPLE 0x781F

struct Action {
  const char *id;
  const char *label;
  const char *icon;
  const char *labelSec;
  uint16_t    colorBg;
};

const Action ALL_ACTIONS[] = {
  {"trunk",        "COFFRE",      "[=]", "FERMER",   C_RED    },
  {"frunk",        "FRUNK",       "^",   "",         C_ORANGE },
  {"horn",         "KLAXON",      "!",   "",         C_RED    },
  {"lock",         "VERROU",      "*",   "",         C_DARK   },
  {"unlock",       "OUVRIR",      "o",   "",         C_GREEN  },
  {"mirror_fold",  "RETROS",      "><",  "DEPLOYER", C_TEAL   },
  {"battery_heat", "BAT HEAT",    "~",   "STOP",     C_ORANGE },
  {"flash",        "FLASH",       "---", "",         C_BLUE   },
  {"precond_on",   "HVAC",        "c",   "OFF",      C_PURPLE },
  {"charge_port",  "PORT CHG",    "=|",  "FERMER",   C_GREEN  },
};
#define N_ACTIONS 10

// ═══════════════════════════════════════════════════════════════
//  SLOTS CONFIG (persistee NVS)
// ═══════════════════════════════════════════════════════════════

#define MAX_SLOTS 10

struct Slot {
  char actionId[20];
  bool hasSec;
};

Slot     slots[MAX_SLOTS];
int      nSlots   = 0;
int      curSlot  = 0;

Preferences prefs;
WebServer   server(80);
String      savedSSID, savedPASS;
bool        wifiConnected = false;
uint32_t    actionClearAt = 0;
bool        menuMode      = false;

// ── Prototypes ────────────────────────────────────────────────
void loadSlots();
void saveSlots();
void countSlots();
int  activeSlotIndex(int n);
const Action* findAction(const char *id);
void execAction(const char *id, bool secondary);
void canSend(uint32_t id, const uint8_t *d, uint8_t dlc);
void canPulse(const uint8_t *active);
void drawIdle();
void drawAction(const char *label, const char *state, uint16_t color);
void drawMenu();
void buzz(int ms);
void handleEncoder();
void handleButton();
void startAP();
void startSTA();
String pageSetup();
String pageDashboard();
String pageOTA();

// ═══════════════════════════════════════════════════════════════
//  CAN TESLA
// ═══════════════════════════════════════════════════════════════

const uint8_t BASE_FRAME[8] = {0xC1,0xA0,0x00,0x00,0xC8,0x02,0x30,0x02};
#define TESLA_ID 0x273
#define PULSE_MS 100

void frameBase(uint8_t *dst) { memcpy(dst, BASE_FRAME, 8); }

void writeField(uint8_t *d, int s, int l, int v) {
  for(int i=0;i<l;i++){int b=s+i;if(b<64)d[b/8]&=~(1<<(b%8));}
  for(int i=0;i<l;i++){if((v>>i)&1){int b=s+i;if(b<64)d[b/8]|=(1<<(b%8));}}
}
void setBit(uint8_t *d, int b) { if(b<64) d[b/8]|=(1<<(b%8)); }

void canSend(uint32_t id, const uint8_t *d, uint8_t dlc) {
  twai_message_t msg={};
  msg.identifier=id; msg.extd=0; msg.rtr=0; msg.data_length_code=dlc;
  memcpy(msg.data,d,dlc);
  twai_transmit(&msg, pdMS_TO_TICKS(100));
}
void canPulse(const uint8_t *active) {
  canSend(TESLA_ID,active,8); delay(PULSE_MS); canSend(TESLA_ID,BASE_FRAME,8);
}

void execAction(const char *id, bool sec) {
  Serial.printf("[ACT] %s sec=%d\n",id,sec);
  if(strcmp(id,"trunk")==0) {
    uint8_t f[8]; frameBase(f); writeField(f,54,2,1);
    canPulse(f); drawAction("COFFRE",sec?"FERMER":"OUVRIR",C_RED); return;
  }
  if(strcmp(id,"frunk")==0) {
    uint8_t f[8]; frameBase(f); setBit(f,5);
    canPulse(f); drawAction("FRUNK","OUVERT",C_ORANGE); return;
  }
  if(strcmp(id,"horn")==0) {
    uint8_t f[8]; frameBase(f); setBit(f,61);
    canPulse(f); drawAction("KLAXON","BEEP!",C_RED); return;
  }
  if(strcmp(id,"lock")==0) {
    uint8_t f[8]; frameBase(f); writeField(f,17,3,1);
    canPulse(f); drawAction("PORTES","LOCK",C_DARK); return;
  }
  if(strcmp(id,"unlock")==0) {
    uint8_t f[8]; frameBase(f); writeField(f,17,3,2);
    canPulse(f); drawAction("PORTES","UNLOCK",C_GREEN); return;
  }
  if(strcmp(id,"mirror_fold")==0) {
    uint8_t f[8]; frameBase(f); writeField(f,24,2,sec?2:1);
    canPulse(f); drawAction("RETROS",sec?"DEPLOYE":"REPLIE",C_TEAL); return;
  }
  if(strcmp(id,"battery_heat")==0) {
    if(sec) {
      battHeatActive=false; uint8_t d[8]={0}; canSend(0x082,d,8);
      drawAction("BAT HEAT","STOP",C_DARK);
    } else {
      battHeatActive=true; lastHeatMs=0;
      drawAction("BAT HEAT","ON",C_ORANGE);
    }
    return;
  }
  if(strcmp(id,"flash")==0) {
    uint8_t d1[]={0x00,0x00,0xC8,0x38,0x84,0x0C,0x00,0x00};
    uint8_t d0[]={0x00,0x00,0xC8,0x38,0x80,0x0C,0x00,0x00};
    canSend(0x3F5,d1,8); delay(200); canSend(0x3F5,d0,8);
    drawAction("PHARES","FLASH",C_BLUE); return;
  }
  if(strcmp(id,"precond_on")==0) {
    if(sec) { uint8_t d[]={0x0E,0x0E,0x00,0x00,0x00}; canSend(0x2F3,d,5); drawAction("HVAC","OFF",C_DARK); }
    else    { uint8_t d[]={0x0E,0x0E,0x0B,0x08,0x00}; canSend(0x2F3,d,5); drawAction("HVAC","ON",C_PURPLE); }
    return;
  }
  if(strcmp(id,"charge_port")==0) {
    uint8_t d[]={sec?(uint8_t)0x02:(uint8_t)0x01,0x00,0x00,0x00};
    canSend(0x333,d,4); drawAction("PORT",sec?"FERME":"OUVERT",C_GREEN); return;
  }
}

// ═══════════════════════════════════════════════════════════════
//  AFFICHAGE
// ═══════════════════════════════════════════════════════════════

void drawIdle() {
  canvas.fillScreen(TFT_BLACK);
  if(nSlots==0) {
    canvas.setTextColor(0x4208,TFT_BLACK);
    canvas.setTextSize(2);
    canvas.drawCentreString("Aucun slot",120,100,1);
    canvas.drawCentreString("Config web",120,130,1);
    canvas.pushSprite(0,0); return;
  }
  int idx=activeSlotIndex(curSlot);
  if(idx<0) return;
  const Slot &s=slots[idx];
  const Action *a=findAction(s.actionId);
  if(!a) return;

  canvas.fillCircle(120,108,82,a->colorBg);
  canvas.drawCircle(120,108,84,TFT_WHITE);
  canvas.drawCircle(120,108,85,0x4208);

  canvas.setTextColor(TFT_WHITE,a->colorBg);
  canvas.setTextSize(3);
  canvas.drawCentreString(a->icon,120,82,1);
  canvas.setTextSize(2);
  canvas.drawCentreString(a->label,120,128,1);

  int dotY=200, startX=120-(nSlots*10)/2;
  for(int i=0;i<nSlots;i++)
    canvas.fillCircle(startX+i*10+4,dotY,3,(i==curSlot)?TFT_WHITE:0x4208);

  if(s.hasSec && a->labelSec[0]) {
    canvas.setTextColor(0x8410,TFT_BLACK);
    canvas.setTextSize(1);
    String sec="2x: "; sec+=a->labelSec;
    canvas.drawCentreString(sec.c_str(),120,218,1);
  }
  char buf[8]; snprintf(buf,sizeof(buf),"%d/%d",curSlot+1,nSlots);
  canvas.setTextColor(0x4208,TFT_BLACK);
  canvas.setTextSize(1);
  canvas.drawCentreString(buf,120,16,1);
  canvas.pushSprite(0,0);
}

void drawAction(const char *label, const char *state, uint16_t color) {
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,110,82,color);
  canvas.drawCircle(120,110,84,TFT_WHITE);
  canvas.setTextColor(TFT_WHITE,color);
  canvas.setTextSize(2);
  canvas.drawCentreString(label,120,88,1);
  canvas.setTextSize(3);
  canvas.drawCentreString(state,120,123,1);
  canvas.pushSprite(0,0);
  actionClearAt=millis()+3000;
}

void drawMenu() {
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,120,110,0x1082);
  canvas.setTextColor(TFT_WHITE,0x1082);
  canvas.setTextSize(2);
  canvas.drawCentreString("CONFIG",120,60,1);
  canvas.setTextSize(1);
  canvas.drawCentreString("Interface web:",120,100,1);
  String ip=wifiConnected?WiFi.localIP().toString():"192.168.4.1";
  canvas.setTextColor(C_RED,0x1082);
  canvas.setTextSize(2);
  canvas.drawCentreString(ip.c_str(),120,150,1);
  canvas.pushSprite(0,0);
  actionClearAt=millis()+6000;
}

// Clic molette : fréquence haute, très court
void buzzClick() { tone(PIN_BUZZ, 3200, 8); }
// Beep validation : fréquence plus douce, un peu plus long
void buzzBeep()  { tone(PIN_BUZZ, 1400, 55); }
// Buzz générique (clic long, double-clic)
void buzz(int ms){ tone(PIN_BUZZ, 1000, ms); }

// ═══════════════════════════════════════════════════════════════
//  SLOTS
// ═══════════════════════════════════════════════════════════════

const Action* findAction(const char *id) {
  for(int i=0;i<N_ACTIONS;i++)
    if(strcmp(ALL_ACTIONS[i].id,id)==0) return &ALL_ACTIONS[i];
  return nullptr;
}
void countSlots() {
  nSlots=0;
  for(int i=0;i<MAX_SLOTS;i++) if(slots[i].actionId[0]) nSlots++;
}
int activeSlotIndex(int n) {
  int c=0;
  for(int i=0;i<MAX_SLOTS;i++) {
    if(slots[i].actionId[0]) { if(c==n) return i; c++; }
  }
  return -1;
}
void loadSlots() {
  prefs.begin("slots",true);
  for(int i=0;i<MAX_SLOTS;i++) {
    char key[8]; snprintf(key,sizeof(key),"s%d",i);
    String val=prefs.getString(key,"");
    if(val.length()>0) {
      int sep=val.indexOf(',');
      String aid=sep>0?val.substring(0,sep):val;
      bool hs=sep>0?val.substring(sep+1)=="1":false;
      strncpy(slots[i].actionId,aid.c_str(),19);
      slots[i].hasSec=hs;
    } else { slots[i].actionId[0]='\0'; slots[i].hasSec=false; }
  }
  prefs.end();
  countSlots();
}
void saveSlots() {
  prefs.begin("slots",false);
  for(int i=0;i<MAX_SLOTS;i++) {
    char key[8]; snprintf(key,sizeof(key),"s%d",i);
    if(slots[i].actionId[0]) {
      String val=String(slots[i].actionId)+","+(slots[i].hasSec?"1":"0");
      prefs.putString(key,val);
    } else prefs.remove(key);
  }
  prefs.end();
  countSlots();
}

// ═══════════════════════════════════════════════════════════════
//  ENCODER + BOUTON
// ═══════════════════════════════════════════════════════════════

void IRAM_ATTR encISR() {
  int a=digitalRead(ENC_A), b=digitalRead(ENC_B);
  if(a!=encLast) { encCount+=(a!=b)?1:-1; encLast=a; }
}

void handleEncoder() {
  static int lastCnt=0;
  if(encCount!=lastCnt && nSlots>0) {
    int d=encCount-lastCnt; lastCnt=encCount;
    curSlot=(curSlot+d+nSlots)%nSlots;
    buzzClick(); drawIdle();
  }
}

void handleButton() {
  bool now=digitalRead(ENC_BTN);
  uint32_t ms=millis();
  if(now==LOW && btnPrev==HIGH) btnPressedAt=ms;
  if(now==HIGH && btnPrev==LOW) {
    uint32_t held=ms-btnPressedAt;
    if(held>=LONG_PRESS_MS) {
      buzz(80); menuMode=true; drawMenu(); waitDblClick=false;
    } else {
      if(waitDblClick && (ms-lastClickAt)<DBLCLICK_MS) {
        waitDblClick=false; buzzBeep(); delay(80); buzzBeep();
        int idx=activeSlotIndex(curSlot);
        if(idx>=0 && slots[idx].actionId[0]) {
          const Action *a=findAction(slots[idx].actionId);
          if(a && slots[idx].hasSec && a->labelSec[0])
            execAction(slots[idx].actionId,true);
        }
      } else { waitDblClick=true; lastClickAt=ms; }
    }
  }
  if(waitDblClick && (ms-lastClickAt)>=DBLCLICK_MS) {
    waitDblClick=false;
    int idx=activeSlotIndex(curSlot);
    if(idx>=0 && slots[idx].actionId[0]) {
      buzzBeep(); execAction(slots[idx].actionId,false);
    }
  }
  btnPrev=now;
}

// ═══════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════

void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig({192,168,4,1},{192,168,4,1},{255,255,255,0});
  WiFi.softAP("Tesla-DIAL-Setup","tesla1234");
}
void startSTA() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
  uint32_t t=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t<12000) delay(300);
  if(WiFi.status()==WL_CONNECTED) { wifiConnected=true; Serial.printf("WiFi: %s\n",WiFi.localIP().toString().c_str()); }
}

// ═══════════════════════════════════════════════════════════════
//  PAGES WEB
// ═══════════════════════════════════════════════════════════════

String pageSetup() {
  return F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Tesla DIAL Setup</title>"
    "<style>*{box-sizing:border-box;margin:0;padding:0;}"
    "body{background:#07080a;color:#c8cdd8;font-family:monospace;"
    "display:flex;flex-direction:column;align-items:center;justify-content:center;"
    "min-height:100vh;padding:1.5rem;}"
    "h1{color:#E31937;font-size:1.4rem;margin-bottom:.5rem;letter-spacing:.2em;}"
    ".sub{font-size:.6rem;color:#2e3040;margin-bottom:2rem;letter-spacing:.3em;}"
    ".card{background:#0e0f12;border:1px solid #1e2028;border-radius:12px;padding:2rem;width:100%;max-width:360px;}"
    "h2{color:#E31937;font-size:.7rem;letter-spacing:.15em;margin-bottom:1rem;}"
    "label{font-size:.65rem;color:#3a3e4a;display:block;margin-bottom:.3rem;}"
    "input{width:100%;background:#070809;border:1px solid #1e2028;color:#c8cdd8;"
    "padding:.7rem .9rem;border-radius:6px;font-family:monospace;font-size:.85rem;"
    "outline:none;margin-bottom:1rem;}"
    "input:focus{border-color:#E31937;}"
    ".btn{width:100%;background:#E31937;border:none;color:#fff;"
    "font-size:.7rem;letter-spacing:.13em;padding:.85rem;border-radius:6px;"
    "cursor:pointer;text-transform:uppercase;}"
    ".nets{border:1px solid #1c1e25;border-radius:6px;max-height:150px;overflow-y:auto;margin-bottom:1rem;}"
    ".net{display:flex;justify-content:space-between;padding:.5rem .8rem;"
    "border-bottom:1px solid #12131a;cursor:pointer;font-size:.78rem;}"
    ".net:hover{background:#13151a;}"
    ".scan{background:#1c1e25;border:1px solid #2a2e38;color:#bcc1cc;"
    "padding:.6rem .9rem;border-radius:5px;cursor:pointer;margin-bottom:.5rem;width:100%;}"
    "</style></head><body>"
    "<h1>TESLA DIAL</h1>"
    "<div class='sub'>SETUP WIFI</div>"
    "<div class='card'>"
    "<h2>CONNEXION RESEAU</h2>"
    "<button class='scan' onclick='scan()'>Scanner les reseaux</button>"
    "<div class='nets' id='nets'></div>"
    "<label>SSID</label>"
    "<input id='ssid' type='text' placeholder='Nom du reseau'>"
    "<label>Mot de passe</label>"
    "<input id='pass' type='password' placeholder='Laisser vide si ouvert'>"
    "<button class='btn' onclick='conn()'>Connecter</button>"
    "</div>"
    "<script>"
    "function scan(){"
    "const n=document.getElementById('nets');"
    "n.innerHTML='<div style=\"padding:.6rem;color:#555\">Scan...</div>';"
    "fetch('/scan').then(r=>r.json()).then(list=>{"
    "list.sort((a,b)=>b.rssi-a.rssi);"
    "n.innerHTML=list.map(x=>'<div class=\"net\" onclick=\"pick(\\'' + x.ssid.replace(/\\x27/g,\"\\\\\\'\")+\\'\\')\">' + x.ssid + '<span style=\"color:#444\">' + x.rssi + 'dBm</span></div>').join('');"
    "});}"
    "function pick(s){document.getElementById('ssid').value=s;}"
    "function conn(){"
    "const s=document.getElementById('ssid').value.trim();"
    "const p=document.getElementById('pass').value;"
    "if(!s)return;"
    "const f=new FormData();f.append('ssid',s);f.append('pass',p);"
    "fetch('/connect',{method:'POST',body:f}).then(()=>{"
    "document.body.innerHTML='<div style=\"color:#22c55e;font-family:monospace;text-align:center;margin-top:40vh\">OK - Reboot dans 3s...</div>';});}"
    "</script></body></html>");
}

// pageDashboard : construite morceau par morceau (pas de concat raw literal)
String pageDashboard() {
  String ip = wifiConnected ? WiFi.localIP().toString() : "192.168.4.1";

  // JSON actions
  String aj = "[";
  for(int i=0;i<N_ACTIONS;i++){
    if(i) aj+=",";
    aj+="{\"id\":\""; aj+=ALL_ACTIONS[i].id;
    aj+="\",\"label\":\""; aj+=ALL_ACTIONS[i].label;
    aj+="\",\"icon\":\""; aj+=ALL_ACTIONS[i].icon;
    aj+="\",\"sec\":\""; aj+=ALL_ACTIONS[i].labelSec; aj+="\"}";
  }
  aj+="]";

  // JSON slots
  String sj = "[";
  for(int i=0;i<MAX_SLOTS;i++){
    if(i) sj+=",";
    sj+="{\"pos\":"; sj+=i;
    sj+=",\"id\":\""; sj+=slots[i].actionId;
    sj+="\",\"hs\":"; sj+=(slots[i].hasSec?"true":"false"); sj+="}";
  }
  sj+="]";

  String h;
  h.reserve(6000);
  h += F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Tesla DIAL</title>"
    "<style>*{box-sizing:border-box;margin:0;padding:0;}"
    "body{background:#07080a;color:#c8cdd8;font-family:monospace;min-height:100vh;padding:1rem;}"
    "h1{color:#E31937;font-size:1.2rem;text-align:center;padding:.7rem 0 .1rem;letter-spacing:.2em;}"
    ".sub{font-size:.55rem;color:#2e3040;text-align:center;margin-bottom:1rem;letter-spacing:.3em;}"
    "h2{color:#E31937;font-size:.65rem;letter-spacing:.15em;margin:1rem 0 .5rem;text-transform:uppercase;}"
    ".palette{display:flex;flex-wrap:wrap;gap:.4rem;margin-bottom:1rem;}"
    ".chip{display:flex;align-items:center;gap:.3rem;background:#0e0f12;"
    "border:1px solid #1e2028;border-radius:5px;padding:.4rem .6rem;"
    "cursor:grab;font-size:.72rem;user-select:none;}"
    ".chip:hover{border-color:#E31937;}"
    ".slots{display:grid;grid-template-columns:repeat(2,1fr);gap:.5rem;margin-bottom:1rem;}"
    ".slot{background:#0e0f12;border:2px dashed #1e2028;border-radius:7px;"
    "padding:.6rem;min-height:68px;position:relative;transition:border-color .2s;}"
    ".slot.ok{border-style:solid;border-color:#2a2e38;}"
    ".slot.over{border-color:#E31937;background:#130507;}"
    ".sn{font-size:.52rem;color:#2e3040;margin-bottom:.25rem;}"
    ".si{display:flex;align-items:center;gap:.4rem;}"
    ".sl{font-size:.75rem;}"
    ".ss{font-size:.58rem;color:#444a58;margin-top:.1rem;}"
    ".sd{position:absolute;top:.3rem;right:.3rem;background:none;border:none;"
    "color:#2e3040;cursor:pointer;}"
    ".sd:hover{color:#E31937;}"
    ".stog{display:flex;align-items:center;gap:.3rem;margin-top:.3rem;font-size:.58rem;color:#3a3e4a;}"
    ".stog input{width:auto;}"
    ".save{width:100%;background:#E31937;border:none;color:#fff;"
    "font-size:.68rem;letter-spacing:.1em;padding:.8rem;border-radius:5px;"
    "cursor:pointer;text-transform:uppercase;margin-bottom:1rem;}"
    ".qg{display:grid;grid-template-columns:repeat(2,1fr);gap:.5rem;margin-bottom:1rem;}"
    ".qb{background:#0e0f12;border:1px solid #1e2028;border-radius:7px;"
    "padding:.6rem;cursor:pointer;text-align:center;}"
    ".qb:active{background:#1c0204;border-color:#E31937;}"
    ".qi{font-size:1.3rem;margin-bottom:.2rem;}"
    ".ql{font-size:.65rem;}"
    "footer{text-align:center;font-size:.52rem;color:#1e2028;padding:1rem 0;}"
    "#toast{position:fixed;bottom:1rem;left:50%;transform:translateX(-50%);"
    "background:#0e0f12;border:1px solid #E31937;color:#c8cdd8;"
    "padding:.5rem 1rem;border-radius:5px;font-size:.72rem;"
    "opacity:0;transition:opacity .3s;pointer-events:none;}"
    "#toast.on{opacity:1;}"
    "</style></head><body>"
    "<h1>TESLA DIAL</h1>"
    "<div class='sub'>CONFIGURATION & CONTROLE</div>"
    "<h2>Configuration des slots</h2>"
    "<p style='font-size:.6rem;color:#3a3e4a;margin-bottom:.7rem'>"
    "Glissez une action sur un slot. Cochez 2x pour le double-clic.</p>"
    "<div class='palette' id='pal'></div>"
    "<div class='slots' id='slg'></div>"
    "<button class='save' onclick='save()'>Sauvegarder</button>"
    "<h2>Actions rapides</h2>"
    "<div class='qg' id='qg'></div>"
    "<footer>Tesla DIAL ");
  h += FW_VERSION;
  h += F(" &bull; ");
  h += ip;
  h += F(" &bull; <a href='/update' style='color:#333'>OTA</a>"
    " &bull; <a href='/reboot' style='color:#333'>Reboot</a></footer>"
    "<div id='toast'></div>"
    "<script>"
    "const A=");
  h += aj;
  h += F(";const S=");
  h += sj;
  h += F(";"
    "let cfg=S.map(s=>({pos:s.pos,id:s.id,hs:s.hs}));"
    "let drag='';"
    "function rPal(){"
    "const p=document.getElementById('pal');"
    "p.innerHTML=A.map(a=>"
    "`<div class='chip' draggable='true' data-id='${a.id}'>${a.icon} ${a.label}</div>`"
    ").join('');"
    "p.querySelectorAll('.chip').forEach(el=>{"
    "el.ondragstart=ev=>{drag=el.dataset.id;ev.dataTransfer.effectAllowed='move';};});}"
    "function rSlots(){"
    "const g=document.getElementById('slg');"
    "g.innerHTML=cfg.map(s=>{"
    "const a=A.find(x=>x.id===s.id);"
    "let inner=a?"
    "`<div class='si'><span>${a.icon}</span><div><div class='sl'>${a.label}</div>`"
    "+(a.sec?`<div class='ss'>2x: ${a.sec}</div>`:'')+'</div></div>'"
    "+(a.sec?`<label class='stog'><input type='checkbox' ${s.hs?'checked':''}> Activer 2x</label>`:'')"
    "+`<button class='sd' onclick='clr(${s.pos})'>x</button>`"
    ":'<div style=\"font-size:.6rem;color:#2e3040\">-- vide --</div>';"
    "return `<div class='slot ${a?'ok':''}' id='sl${s.pos}'>`"
    "+`<div class='sn'>Pos ${s.pos+1}</div>${inner}</div>`;"
    "}).join('');"
    "cfg.forEach(s=>{"
    "const el=document.getElementById('sl'+s.pos);"
    "el.ondragover=ev=>ev.preventDefault();"
    "el.ondrop=ev=>{ev.preventDefault();cfg[s.pos].id=drag;cfg[s.pos].hs=false;rSlots();toast('Pose en pos '+(s.pos+1));};"
    "const cb=el.querySelector('input');"
    "if(cb)cb.onchange=ev=>cfg[s.pos].hs=ev.target.checked;"
    "});}"
    "function rQ(){"
    "document.getElementById('qg').innerHTML=A.map(a=>"
    "`<div class='qb' onclick='qa(\"${a.id}\")'>"
    "<div class='qi'>${a.icon}</div><div class='ql'>${a.label}</div></div>`"
    ").join('');}"
    "function clr(p){cfg[p].id='';cfg[p].hs=false;rSlots();}"
    "function save(){"
    "fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({slots:cfg.map(s=>({pos:s.pos,actionId:s.id,hasSec:s.hs}))})})"
    ".then(()=>toast('Config sauvegardee!')).catch(()=>toast('Erreur',true));}"
    "function qa(id){"
    "toast('Envoi '+id+'...');"
    "fetch('/action?cmd='+id+'&sec=0').then(()=>toast('OK: '+id)).catch(()=>toast('Erreur',true));}"
    "let tt;"
    "function toast(m,e=false){"
    "const el=document.getElementById('toast');"
    "el.textContent=m;el.style.borderColor=e?'#ff6b35':'#E31937';"
    "el.classList.add('on');clearTimeout(tt);tt=setTimeout(()=>el.classList.remove('on'),2500);}"
    "rPal();rSlots();rQ();"
    "</script></body></html>");
  return h;
}

String pageOTA() {
  return F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<title>OTA</title><style>body{background:#07080a;color:#eee;font-family:monospace;"
    "display:flex;align-items:center;justify-content:center;height:100vh;text-align:center;}"
    ".c{background:#0e0f12;border:1px solid #1e2028;border-radius:10px;padding:2rem;}"
    "h2{color:#E31937;margin-bottom:1rem;}input{margin:1rem 0;}"
    ".b{background:#E31937;border:none;color:#fff;padding:.8rem 2rem;"
    "border-radius:5px;cursor:pointer;font-size:.9rem;}"
    "</style></head><body><div class='c'><h2>MISE A JOUR OTA</h2>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='update' accept='.bin'><br>"
    "<input type='submit' value='Flash' class='b'></form></div></body></html>");
}

// ═══════════════════════════════════════════════════════════════
//  HANDLERS HTTP
// ═══════════════════════════════════════════════════════════════

void handleWifiScan() {
  int n=WiFi.scanNetworks();
  String j="[";
  for(int i=0;i<n;i++){if(i)j+=","; j+="{\"ssid\":\""+WiFi.SSID(i)+"\",\"rssi\":"+WiFi.RSSI(i)+"}";}
  j+="]";
  server.send(200,"application/json",j);
}
void handleWifiConnect() {
  String s=server.arg("ssid"), p=server.arg("pass");
  prefs.begin("wifi",false); prefs.putString("ssid",s); prefs.putString("pass",p); prefs.end();
  server.send(200,"text/plain","OK");
  delay(500); ESP.restart();
}
void handleAction() {
  String cmd=server.arg("cmd");
  bool sec=server.arg("sec")=="1";
  execAction(cmd.c_str(),sec);
  server.send(200,"text/plain","OK");
}
void handleConfig() {
  String body=server.arg("plain");
  for(int i=0;i<MAX_SLOTS;i++){slots[i].actionId[0]='\0'; slots[i].hasSec=false;}
  int p=0;
  while((p=body.indexOf("\"pos\":",p))>=0) {
    p+=6; int pos=body.substring(p).toInt();
    if(pos<0||pos>=MAX_SLOTS){p++;continue;}
    int ai=body.indexOf("\"actionId\":\"",p);
    if(ai<0) break;
    ai+=12; int ae=body.indexOf("\"",ai);
    String aid=body.substring(ai,ae);
    int hi=body.indexOf("\"hasSec\":",p);
    bool hs=false;
    if(hi>=0) hs=body.substring(hi+9,hi+13).indexOf("true")>=0;
    if(aid.length()>0 && aid!="null" && aid!="") {
      strncpy(slots[pos].actionId,aid.c_str(),19);
      slots[pos].hasSec=hs;
    }
    p=ae;
  }
  saveSlots(); drawIdle();
  server.send(200,"text/plain","OK");
}
void handleGetConfig() {
  String j="[";
  for(int i=0;i<MAX_SLOTS;i++){
    if(i)j+=",";
    j+="{\"pos\":"+String(i)+",\"actionId\":\""+String(slots[i].actionId)+"\",\"hasSec\":"+(slots[i].hasSec?"true":"false")+"}";
  }
  j+="]";
  server.send(200,"application/json",j);
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  pinMode(PIN_HOLD,OUTPUT); digitalWrite(PIN_HOLD,HIGH);

  Serial.begin(115200); delay(400);
  pinMode(PIN_BUZZ,OUTPUT);
  pinMode(ENC_A,INPUT_PULLUP); pinMode(ENC_B,INPUT_PULLUP); pinMode(ENC_BTN,INPUT_PULLUP);
  encLast=digitalRead(ENC_A);
  attachInterrupt(digitalPinToInterrupt(ENC_A),encISR,CHANGE);

  display.begin();
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);
  canvas.createSprite(240,240);
  canvas.setTextDatum(MC_DATUM);

  // Splash
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,120,100,C_RED);
  canvas.setTextColor(TFT_WHITE,C_RED);
  canvas.setTextSize(2);
  canvas.drawCentreString("TESLA",120,100,1);
  canvas.drawCentreString("DIAL",120,125,1);
  canvas.setTextSize(1);
  canvas.setTextColor(0xAAAA,C_RED);
  canvas.drawCentreString(FW_VERSION,120,155,1);
  canvas.pushSprite(0,0);
  buzz(100); delay(1200);

  // CAN TWAI
  twai_general_config_t g=TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN,CAN_RX_PIN,TWAI_MODE_NORMAL);
  twai_timing_config_t  t=TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t  f=TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if(twai_driver_install(&g,&t,&f)==ESP_OK) twai_start();

  loadSlots();

  prefs.begin("wifi",true);
  savedSSID=prefs.getString("ssid","");
  savedPASS=prefs.getString("pass","");
  prefs.end();

  if(savedSSID.length()) startSTA();
  if(!wifiConnected)     startAP();

  server.on("/",       HTTP_GET,  [](){ server.send(200,"text/html",wifiConnected?pageDashboard():pageSetup()); });
  server.on("/scan",   HTTP_GET,  handleWifiScan);
  server.on("/connect",HTTP_POST, handleWifiConnect);
  server.on("/action", HTTP_GET,  handleAction);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/config", HTTP_GET,  handleGetConfig);
  server.on("/update", HTTP_GET,  [](){ server.send(200,"text/html",pageOTA()); });
  server.on("/reboot", HTTP_GET,  [](){ server.send(200,"text/plain","OK"); delay(300); ESP.restart(); });
  server.on("/update", HTTP_POST,
    [](){
      server.send(200,"text/html",Update.hasError()?"<h2>ERREUR</h2>":"<h2>OK - Reboot...</h2>");
      delay(1000); ESP.restart();
    },
    [](){
      HTTPUpload &up=server.upload();
      if(up.status==UPLOAD_FILE_START)  Update.begin(UPDATE_SIZE_UNKNOWN);
      if(up.status==UPLOAD_FILE_WRITE)  Update.write(up.buf,up.currentSize);
      if(up.status==UPLOAD_FILE_END)    Update.end(true);
    }
  );

  server.begin();
  drawIdle();
  Serial.println("DIAL pret.");
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();
  handleEncoder();
  handleButton();

  if(actionClearAt>0 && millis()>=actionClearAt) {
    actionClearAt=0; menuMode=false; drawIdle();
  }
  if(battHeatActive && millis()-lastHeatMs>=500) {
    lastHeatMs=millis();
    uint8_t d[8]={0x05,0,0,0,0,0,0,0};
    canSend(0x082,d,8);
  }
  twai_message_t rx;
  twai_receive(&rx,0);
}
