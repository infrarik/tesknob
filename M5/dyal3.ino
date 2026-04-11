/*
 * ============================================================
 *  DYÁL3 – M5Stack DIAL  v2.0
 *  ESP32-S3 + SN65HVD230 (PORT.B)
 *
 *  PINOUT M5DIAL (DYÁL3)
 *  Ecran GC9A01 : MOSI=5 SCLK=6 CS=7 DC=4 RST=8 BL=9
 *  Touch FT3267 : SDA=11 SCL=12 addr=0x38
 *  Encoder      : A=40 B=41
 *  Bouton front : GPIO42
 *  HOLD power   : GPIO46
 *  Buzzer       : GPIO3
 *  CAN TX       : GPIO2  (PORT.B)
 *  CAN RX       : GPIO1  (PORT.B)
 *
 *  MODES WIFI
 *  - STA : connexion au WiFi maison (dashboard accessible)
 *  - CONFIG AP : SSID="Dyal3" / WPA2="tesladyal3" / 192.168.10.1
 *    Accessible uniquement via icone roue dentee sur l'ecran
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
#include <M5GFX.h>
#include <Wire.h>
#include "secrets.h"
#include "dyal3_network.h"  // Surcouche réseau ESP32-C3

// ─── COULEURS DYAL3 ──────────────────────────────────────────
#define DYAL_BLUE    0x1A4D
#define DYAL_ORANGE  0xD300
#define C_RED        0xE800
#define C_DARK       0x18C3
#define C_GREEN      0x07E0
#define C_ORANGE     0xFD20
#define C_BLUE       0x001F
#define C_TEAL       0x07FF
#define C_PURPLE     0x781F

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

#define FW_VERSION "v2.0"

// ─── MODE CONFIG AP ───────────────────────────────────────────
#define CONFIG_AP_SSID  "Dyal3"
#define CONFIG_AP_PASS  "tesladyal3"
#define CONFIG_AP_IP    "192.168.10.1"
bool     configMode     = false;   // true = AP config actif


// ─── ICONE ROUE DENTEE ────────────────────────────────────────
// Zone tactile coin bas-droit (ecran 240x240)
// Ou tap detecté via touch FT3267 sur cette zone
#define GEAR_X  200
#define GEAR_Y  200
#define GEAR_R   28

// Touch simplifié via polling I2C FT3267
#define TOUCH_SDA  11
#define TOUCH_SCL  12
#define TOUCH_ADDR 0x38
bool     lastTouchGear = false;

// ═══════════════════════════════════════════════════════════════
//  ACTIONS DISPONIBLES
// ═══════════════════════════════════════════════════════════════

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
  char     actionId[20];
  bool     hasSec;
  uint16_t color;      // 0 = couleur par defaut de l'action
};

Slot     slots[MAX_SLOTS];
int      nSlots   = 0;
int      curSlot  = 0;

Preferences prefs;
WebServer   server(80);
String      savedSSID, savedPASS;
bool        wifiConnected = false;
uint32_t    actionClearAt = 0;
uint8_t     brightness    = 128;  // 0-255, persisté NVS
bool        teslaConnected = false; // connexion ESP32-C3 réussie
bool        brightMode     = false;  // true = mode réglage luminosité actif
bool        majorOk        = false;  // connexion Major (ESP32-C3) active
volatile bool longPressFlag  = false; // signalé par tâche bouton
volatile bool shortPressFlag = false; // clic court
volatile bool pingRequest    = false; // demande de ping Major

// ─── STATE MACHINE UI ─────────────────────────────────────────
enum UiMode { UI_HOME, UI_MENU, UI_CAN_A, UI_CAN_B };
volatile UiMode uiMode = UI_HOME;
// Fenêtre de garde : ignore tout clic pendant N ms après sortie d'un sous-mode
volatile uint32_t suppressUntil = 0;

// Reset complet de l'état clic — appeler après chaque transition UI
void uiResetClickState() {
  shortPressFlag = false;
  longPressFlag  = false;
  waitDblClick   = false;
  lastClickAt    = 0;
  btnPressedAt   = 0;
  suppressUntil  = millis() + 350;
}

// ── Prototypes ────────────────────────────────────────────────
void loadSlots(); void saveSlots(); void countSlots();
int  activeSlotIndex(int n);
const Action* findAction(const char *id);
void execAction(const char *id, bool secondary);
void canPulse(const uint8_t *active);
void drawIdle(); void drawAction(const char*, const char*, uint16_t);
void drawConfigMode(); void drawGearIcon(bool highlight);
void buzzClick(); void buzzBeep(); void buzz(int ms);
void handleEncoder(); void handleButton(); void checkTouch();
void startAP(); void startConfigAP(); void startSTA();
void setupRoutes();
String pageDashboard(); String pageConfig(); String pageOTA();

// ═══════════════════════════════════════════════════════════════
//  CAN TESLA
// ═══════════════════════════════════════════════════════════════

const uint8_t BASE_FRAME[8] = {0xC1,0xA0,0x00,0x00,0xC8,0x02,0x30,0x02};
#define TESLA_ID 0x273
#define PULSE_MS 100

void frameBase(uint8_t *dst) { memcpy(dst, BASE_FRAME, 8); }
void writeField(uint8_t *d,int s,int l,int v){
  for(int i=0;i<l;i++){int b=s+i;if(b<64)d[b/8]&=~(1<<(b%8));}
  for(int i=0;i<l;i++){if((v>>i)&1){int b=s+i;if(b<64)d[b/8]|=(1<<(b%8));}}
}
void setBit(uint8_t *d,int b){ if(b<64) d[b/8]|=(1<<(b%8)); }
// canSend() supprimé – remplacé par netCanSendA() / netCanSend()
// Le M5 n'est plus directement sur le bus CAN.
void canPulse(const uint8_t *active){
  netCanSendA(TESLA_ID,active,8); delay(PULSE_MS); netCanSendA(TESLA_ID,BASE_FRAME,8);
}

void execAction(const char *id,bool sec){
  Serial.printf("[ACT] %s sec=%d\n",id,sec);
  if(strcmp(id,"trunk")==0)         { uint8_t f[8]; frameBase(f); writeField(f,54,2,1); canPulse(f); drawAction("COFFRE",sec?"FERMER":"OUVRIR",C_RED); return; }
  if(strcmp(id,"frunk")==0)         { uint8_t f[8]; frameBase(f); setBit(f,5); canPulse(f); drawAction("FRUNK","OUVERT",C_ORANGE); return; }
  if(strcmp(id,"horn")==0)          { uint8_t f[8]; frameBase(f); setBit(f,61); canPulse(f); drawAction("KLAXON","BEEP!",C_RED); return; }
  if(strcmp(id,"lock")==0)          { uint8_t f[8]; frameBase(f); writeField(f,17,3,1); canPulse(f); drawAction("PORTES","LOCK",C_DARK); return; }
  if(strcmp(id,"unlock")==0)        { uint8_t f[8]; frameBase(f); writeField(f,17,3,2); canPulse(f); drawAction("PORTES","UNLOCK",C_GREEN); return; }
  if(strcmp(id,"mirror_fold")==0)   { uint8_t f[8]; frameBase(f); writeField(f,24,2,sec?2:1); canPulse(f); drawAction("RETROS",sec?"DEPLOYE":"REPLIE",C_TEAL); return; }
  if(strcmp(id,"battery_heat")==0)  { if(sec){battHeatActive=false;uint8_t d[8]={0};netCanSendA(0x082,d,8);drawAction("BAT HEAT","STOP",C_DARK);}else{battHeatActive=true;lastHeatMs=0;drawAction("BAT HEAT","ON",C_ORANGE);} return; }
  if(strcmp(id,"flash")==0)         { uint8_t d1[]={0x00,0x00,0xC8,0x38,0x84,0x0C,0x00,0x00}; uint8_t d0[]={0x00,0x00,0xC8,0x38,0x80,0x0C,0x00,0x00}; netCanSendA(0x3F5,d1,8); delay(200); netCanSendA(0x3F5,d0,8); drawAction("PHARES","FLASH",C_BLUE); return; }
  if(strcmp(id,"precond_on")==0)    { if(sec){uint8_t d[]={0x0E,0x0E,0x00,0x00,0x00};netCanSendA(0x2F3,d,5);drawAction("HVAC","OFF",C_DARK);}else{uint8_t d[]={0x0E,0x0E,0x0B,0x08,0x00};netCanSendA(0x2F3,d,5);drawAction("HVAC","ON",C_PURPLE);} return; }
  if(strcmp(id,"charge_port")==0)   { uint8_t d[]={sec?(uint8_t)0x02:(uint8_t)0x01,0x00,0x00,0x00}; netCanSendA(0x333,d,4); drawAction("PORT",sec?"FERME":"OUVERT",C_GREEN); return; }
}

// ═══════════════════════════════════════════════════════════════
//  AFFICHAGE
// ═══════════════════════════════════════════════════════════════

// Dessine l'icone roue dentee coin bas-droit
// highlight = true si mode config actif (orange vif)
void drawGearIcon(bool highlight) {
  uint16_t col = highlight ? DYAL_ORANGE : 0x4208;
  uint16_t bg  = TFT_BLACK;
  // Fond petit cercle
  canvas.fillCircle(GEAR_X, GEAR_Y, GEAR_R, bg);
  // Cercle exterieur
  canvas.drawCircle(GEAR_X, GEAR_Y, GEAR_R-2, col);
  canvas.drawCircle(GEAR_X, GEAR_Y, GEAR_R-8, col);
  // Dents (8 petits segments)
  for(int i=0;i<8;i++){
    float a = i * PI/4;
    int x1 = GEAR_X + (int)((GEAR_R-2)*cos(a));
    int y1 = GEAR_Y + (int)((GEAR_R-2)*sin(a));
    int x2 = GEAR_X + (int)((GEAR_R+4)*cos(a));
    int y2 = GEAR_Y + (int)((GEAR_R+4)*sin(a));
    canvas.drawLine(x1,y1,x2,y2,col);
    canvas.fillCircle(x2,y2,3,col);
  }
  // Texte engrenage centre
  canvas.setTextColor(col, bg);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("CFG", GEAR_X, GEAR_Y-3);
}

// Retourne le nombre total de slots navigables (sys + user)
// sys: REBOOT(-1), SETUP(-2), BRIGHT(-3) = 3 fixes
// user: nSlots

void drawAction(const char *label,const char *state,uint16_t color) {
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,108,82,color);
  { uint16_t rc=majorOk?C_GREEN:C_RED;
    canvas.drawCircle(120,108,84,rc); }
  canvas.setTextColor(TFT_WHITE,color);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString(label, 120, 86);
  canvas.setTextSize(3);
  canvas.setTextDatum(MC_DATUM); canvas.drawString(state, 120, 120);
  canvas.pushSprite(0,0);
  actionClearAt=millis()+3000;
}

// Ecran mode CONFIG AP
void drawConfigMode() {
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,110,108,DYAL_BLUE);
  canvas.drawCircle(120,110,110,DYAL_ORANGE);
  canvas.drawCircle(120,110,111,DYAL_ORANGE);

  canvas.setTextColor(DYAL_ORANGE,DYAL_BLUE);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("MODE CONFIG", 120, 30);

  canvas.drawLine(30,42,210,42,DYAL_ORANGE);

  canvas.setTextColor(TFT_WHITE,DYAL_BLUE);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("SSID:", 120, 55);
  canvas.setTextColor(DYAL_ORANGE,DYAL_BLUE);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString(CONFIG_AP_SSID, 120, 68);

  canvas.setTextColor(TFT_WHITE,DYAL_BLUE);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("CLE WPA2:", 120, 92);
  canvas.setTextColor(DYAL_ORANGE,DYAL_BLUE);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString(CONFIG_AP_PASS, 120, 104);

  canvas.drawLine(30,118,210,118,0x2104);

  canvas.setTextColor(TFT_WHITE,DYAL_BLUE);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("IP:", 120, 130);
  canvas.setTextColor(DYAL_ORANGE,DYAL_BLUE);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString(CONFIG_AP_IP, 120, 143);

  canvas.setTextColor(0x4208,DYAL_BLUE);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("Tapez CFG pour quitter", 120, 175);

  canvas.pushSprite(0,0);
}

// ═══════════════════════════════════════════════════════════════
//  SONS
// ═══════════════════════════════════════════════════════════════
void buzzClick() { tone(PIN_BUZZ,3200,8);  }
void buzzBeep()  { tone(PIN_BUZZ,1400,55); }
void buzz(int ms){ tone(PIN_BUZZ,1000,ms); }

// ═══════════════════════════════════════════════════════════════
//  SLOTS
// ═══════════════════════════════════════════════════════════════
const Action* findAction(const char *id){
  for(int i=0;i<N_ACTIONS;i++) if(strcmp(ALL_ACTIONS[i].id,id)==0) return &ALL_ACTIONS[i];
  return nullptr;
}
void countSlots(){ nSlots=0; for(int i=0;i<MAX_SLOTS;i++) if(slots[i].actionId[0]) nSlots++; }
int activeSlotIndex(int n){
  int c=0;
  for(int i=0;i<MAX_SLOTS;i++) { if(slots[i].actionId[0]){ if(c==n) return i; c++; } }
  return -1;
}
void loadSlots(){
  prefs.begin("slots",true);
  for(int i=0;i<MAX_SLOTS;i++){
    char key[8]; snprintf(key,sizeof(key),"s%d",i);
    String val=prefs.getString(key,"");
    if(val.length()>0){
      // format: actionId,hasSec,color(hex)
      int s1=val.indexOf(',');
      int s2=s1>0?val.indexOf(',',s1+1):-1;
      String aid=s1>0?val.substring(0,s1):val;
      bool hs=s1>0?(s2>0?val.substring(s1+1,s2):val.substring(s1+1))=="1":false;
      uint16_t col=s2>0?(uint16_t)strtol(val.substring(s2+1).c_str(),nullptr,16):0;
      strncpy(slots[i].actionId,aid.c_str(),19);
      slots[i].hasSec=hs;
      slots[i].color=col;
    } else { slots[i].actionId[0]='\0'; slots[i].hasSec=false; slots[i].color=0; }
  }
  brightness=prefs.getUChar("bright",128);
  prefs.end(); countSlots();
}
void saveSlots(){
  prefs.begin("slots",false);
  for(int i=0;i<MAX_SLOTS;i++){
    char key[8]; snprintf(key,sizeof(key),"s%d",i);
    if(slots[i].actionId[0]){
      char colbuf[8]; snprintf(colbuf,sizeof(colbuf),"%04X",slots[i].color);
      String val=String(slots[i].actionId)+","+(slots[i].hasSec?"1":"0")+","+colbuf;
      prefs.putString(key,val);
    } else prefs.remove(key);
  }
  prefs.putUChar("bright",brightness);
  prefs.end(); countSlots();
}

void saveBrightness(){
  prefs.begin("slots",false);
  prefs.putUChar("bright",brightness);
  prefs.end();
  display.setBrightness(brightness);
}

// ═══════════════════════════════════════════════════════════════
//  TOUCH SCREEN (FT3267 I2C)
// ═══════════════════════════════════════════════════════════════

// Lecture coordonnees touch via I2C brut
bool readTouch(int &tx, int &ty) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02); // registre nb de points
  if(Wire.endTransmission(false)!=0) return false;
  Wire.requestFrom(TOUCH_ADDR,1);
  if(!Wire.available()) return false;
  uint8_t npts=Wire.read()&0x0F;
  if(npts==0) return false;

  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x03);
  if(Wire.endTransmission(false)!=0) return false;
  Wire.requestFrom(TOUCH_ADDR,6);
  if(Wire.available()<6) return false;
  uint8_t b[6];
  for(int i=0;i<6;i++) b[i]=Wire.read();
  tx=((b[0]&0x0F)<<8)|b[1];
  ty=((b[2]&0x0F)<<8)|b[3];
  return true;
}

void checkTouch() {
  int tx,ty;
  if(!readTouch(tx,ty)) { lastTouchGear=false; return; }

  // Zone roue dentee ?
  int dx=tx-GEAR_X, dy=ty-GEAR_Y;
  bool onGear=(dx*dx+dy*dy)<=(GEAR_R+8)*(GEAR_R+8);

  if(onGear && !lastTouchGear) {
    lastTouchGear=true;
    toggleConfigMode();
  } else if(!onGear) {
    lastTouchGear=false;
  }
}

// ═══════════════════════════════════════════════════════════════
//  TOGGLE MODE CONFIG
// ═══════════════════════════════════════════════════════════════

void startConfigAP() {
  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192,168,10,1);
  WiFi.softAPConfig(apIP,apIP,IPAddress(255,255,255,0));
  WiFi.softAP(CONFIG_AP_SSID,CONFIG_AP_PASS);
  Serial.printf("Config AP: %s / %s -> %s\n",CONFIG_AP_SSID,CONFIG_AP_PASS,CONFIG_AP_IP);
}

void stopConfigAP() {
  WiFi.softAPdisconnect(true); delay(100);
  if(savedSSID.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
    uint32_t t=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t<12000) delay(300);
    wifiConnected=(WiFi.status()==WL_CONNECTED);
  } else {
    // Pas de STA configuré : repasser en AP normal
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig({192,168,4,1},{192,168,4,1},{255,255,255,0});
    WiFi.softAP("DYAL3-Setup","dyal3pwd");
    wifiConnected=false;
  }
}

void toggleConfigMode() {
  configMode=!configMode;
  buzz(60);
  if(configMode) {
    startConfigAP();
    drawConfigMode();
  } else {
    stopConfigAP();
    drawIdle();
  }
}

// ═══════════════════════════════════════════════════════════════
//  ENCODER + BOUTON
// ═══════════════════════════════════════════════════════════════

void IRAM_ATTR encISR(){
  int a=digitalRead(ENC_A),b=digitalRead(ENC_B);
  if(a!=encLast){ encCount+=(a!=b)?1:-1; encLast=a; }
}

// ═══════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════

void startAP(){
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig({192,168,4,1},{192,168,4,1},{255,255,255,0});
  WiFi.softAP("DYAL3-Setup","dyal3pwd");
}
void startSTA(){
  WiFi.setHostname(SECRET_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
  uint32_t t=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t<12000) delay(300);
  if(WiFi.status()==WL_CONNECTED){ wifiConnected=true; Serial.printf("WiFi STA: %s\n",WiFi.localIP().toString().c_str()); }
}

// ═══════════════════════════════════════════════════════════════
//  PAGES WEB
// ═══════════════════════════════════════════════════════════════

// ── CSS commun ────────────────────────────────────────────────
String commonCSS() {
  return F("<style>"
    "*{box-sizing:border-box;margin:0;padding:0;}"
    "body{background:#07080a;color:#c8cdd8;font-family:'Share Tech Mono',monospace;"
    "min-height:100vh;padding:0;"
    "background-image:radial-gradient(ellipse 80% 40% at 50% -5%,#1B3D6E22,transparent 60%);}"
    "h1{font-family:sans-serif;font-weight:900;font-size:1.3rem;letter-spacing:.15em;"
    "text-align:center;padding:.8rem 0 .1rem;}"
    ".l-b{color:#1B3D6E;}.l-o{color:#D4620A;}"
    ".sub{font-size:.55rem;color:#2e3040;text-align:center;margin-bottom:1.2rem;letter-spacing:.3em;}"
    ".card{background:#0e0f12;border:1px solid #1e2028;border-radius:10px;padding:1.2rem;}"
    "h2{font-size:.65rem;color:#D4620A;letter-spacing:.15em;margin-bottom:.8rem;text-transform:uppercase;}"
    "label{font-size:.62rem;color:#3a3e4a;display:block;margin-bottom:.25rem;}"
    "input,select{width:100%;background:#070809;border:1px solid #1e2028;color:#c8cdd8;"
    "padding:.65rem .8rem;border-radius:5px;font-family:monospace;font-size:.82rem;"
    "outline:none;margin-bottom:.9rem;}"
    "input:focus,select:focus{border-color:#D4620A;}"
    ".btn{width:100%;background:#D4620A;border:none;color:#fff;"
    "font-size:.68rem;letter-spacing:.13em;padding:.82rem;border-radius:5px;"
    "cursor:pointer;text-transform:uppercase;transition:background .2s;}"
    ".btn:hover{background:#b8520a;}"
    ".btn-b{background:#1B3D6E;}.btn-b:hover{background:#142d50;}"
    ".btn-sm{padding:.5rem .9rem;font-size:.62rem;width:auto;margin-bottom:.5rem;}"
    ".nets{border:1px solid #1c1e25;border-radius:6px;max-height:150px;overflow-y:auto;margin-bottom:.8rem;}"
    ".net{display:flex;justify-content:space-between;padding:.45rem .7rem;"
    "border-bottom:1px solid #12131a;cursor:pointer;font-size:.75rem;transition:background .15s;}"
    ".net:hover{background:#13151a;}"
    "footer{text-align:center;font-size:.52rem;color:#1e2028;padding:.7rem;border-top:1px solid #0d0e12;}"
    "#toast{position:fixed;bottom:1rem;left:50%;transform:translateX(-50%);"
    "background:#0e0f12;border:1px solid #D4620A;color:#c8cdd8;"
    "padding:.5rem 1rem;border-radius:5px;font-size:.72rem;"
    "opacity:0;transition:opacity .25s;pointer-events:none;z-index:999;}"
    "#toast.on{opacity:1;}"
    "main{padding:1rem;max-width:480px;margin:0 auto;display:flex;flex-direction:column;gap:1rem;}"
    "</style>");
}

String logoHTML() {
  return F("<h1><span class='l-b'>DY</span><span class='l-o'>&#193;</span>"
           "<span class='l-b'>L</span><span class='l-o'>3</span></h1>");
}

// ── Page config (accessible en mode CONFIG AP ou STA connecté) 
String pageConfig() {
  String h;
  h.reserve(4000);
  h += F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>DYÁL3 – Configuration</title>");
  h += commonCSS();
  h += F("</head><body>");
  h += logoHTML();
  h += F("<div class='sub'>CONFIGURATION</div><main>");

  // ── Section WiFi client ──────────────────────────────────────
  h += F("<div class='card'>"
    "<h2>&#127968; WiFi Maison (mode client)</h2>"
    "<button class='btn btn-b btn-sm' onclick='scan()'>&#128269; Scanner les réseaux</button>"
    "<div class='nets' id='nets'></div>"
    "<label>SSID</label>"
    "<input id='ssid' type='text' placeholder='Nom du réseau WiFi'>"
    "<label>Mot de passe WPA2</label>"
    "<input id='pass' type='password' placeholder='Laisser vide si ouvert'>"
    "<button class='btn' onclick='saveWifi()'>&#128190; Sauvegarder et connecter</button>"
    "</div>");

  // ── Section slots ────────────────────────────────────────────
  // JSON actions
  String aj="[";
  for(int i=0;i<N_ACTIONS;i++){
    if(i) aj+=",";
    aj+="{\"id\":\""; aj+=ALL_ACTIONS[i].id;
    aj+="\",\"label\":\""; aj+=ALL_ACTIONS[i].label;
    aj+="\",\"icon\":\""; aj+=ALL_ACTIONS[i].icon;
    aj+="\",\"sec\":\""; aj+=ALL_ACTIONS[i].labelSec; aj+="\"}";
  }
  aj+="]";
  String sj="[";
  for(int i=0;i<MAX_SLOTS;i++){
    if(i) sj+=",";
    sj+="{\"pos\":"; sj+=i;
    sj+=",\"id\":\""; sj+=slots[i].actionId; sj+="\""; // ferme id
    sj+=",\"hs\":"; sj+=(slots[i].hasSec?"true":"false");
    // Convertir RGB565 → #RRGGBB pour le color picker
    uint16_t rgb565=slots[i].color;
    uint8_t r5=(rgb565>>11)&0x1F, g6=(rgb565>>5)&0x3F, b5=rgb565&0x1F;
    uint8_t r8=(r5<<3)|(r5>>2), g8=(g6<<2)|(g6>>4), b8=(b5<<3)|(b5>>2);
    char cb[8]; snprintf(cb,sizeof(cb),"#%02X%02X%02X",r8,g8,b8);
    if(!slots[i].color) strcpy(cb,"#000000");
    sj+=",\"col\":\""; sj+=cb; sj+="\"}";
  }
  sj+="]";

  h += F("<div class='card'>"
    "<h2>&#9881; Actions de la molette</h2>"
    "<p style='font-size:.58rem;color:#3a3e4a;margin-bottom:.7rem'>"
    "Glissez une action sur un slot. 2x = double-clic.</p>"
    "<div style='display:flex;flex-wrap:wrap;gap:.35rem;margin-bottom:.8rem' id='pal'></div>"
    "<div style='display:grid;grid-template-columns:repeat(2,1fr);gap:.4rem' id='slg'></div>"
    "<br><button class='btn' onclick='saveSlots()'>&#128190; Sauvegarder les slots</button>"
    "</div>");

  h += F("<div class='card'>"
    "<h2>&#9889; Actions rapides</h2>"
    "<div style='display:grid;grid-template-columns:repeat(2,1fr);gap:.4rem' id='qg'></div>"
    "</div>");

  h += F("<div class='card'>"
    "<h2>&#9728; Luminosite</h2>"
    "<input type='range' id='bri' min='0' max='255' step='5' style='width:100%;accent-color:#D4620A;margin-bottom:.5rem'>"
    "<div style='display:flex;justify-content:space-between;font-size:.6rem;color:#444'>"
    "<span>Min</span><span id='bri-v'></span><span>Max</span></div>"
    "<br><button class='btn' onclick='saveBri()'>&#128190; Appliquer</button>"
    "</div>");

  h += F("<div class='card'>");
  h += F("<h2>&#9874; Systeme</h2>");
  h += majorOk
    ? F("<div style='margin-bottom:.7rem;background:#22c55e;color:#000;padding:.4rem .7rem;border-radius:5px;font-size:.72rem;font-weight:bold'>&#10010; MAJOR CONNECT&Eacute;</div>")
    : F("<div style='margin-bottom:.7rem;background:#e80000;color:#000;padding:.4rem .7rem;border-radius:5px;font-size:.72rem;font-weight:bold'>&#10006; MAJOR NON CONNECT&Eacute;</div>");
  h += F("<button class='btn btn-b btn-sm' onclick='teslaConnect()'>&#128246; Reconnexion Major</button>&nbsp;"
    "<button class='btn btn-sm' onclick='location.href=\"/update\"'>""&#128260; OTA</button>&nbsp;"
    "<button class='btn btn-sm' style='background:#c0392b'"" onclick='if(confirm(\"Reboot M5 ?\"))fetch(\"/reboot\").then(()=>toast(\"Reboot...\")')'>""&#9940; Reboot</button>"
    "<div id='tesla-status' style='font-size:.62rem;color:#3a3e4a;margin-top:.6rem'></div>"
    "</div>");

  h += F("</main><footer>DYÁL3 " FW_VERSION " &bull; Config &bull; <a href='/' style='color:#333'>Dashboard</a></footer>"
    "<div id='toast'></div><script>"
    "const A=");
  h += aj;
  h += F(";const S=");
  h += sj;
  h += F(";const BRI=");
  h += String(brightness);
  h += F(";"
    "let cfg=S.map(s=>({pos:s.pos,id:s.id,hs:s.hs,col:s.col}));let drag='';"
    // ── Scan WiFi
    "function scan(){"
    "const n=document.getElementById('nets');"
    "n.innerHTML='<div style=\"padding:.5rem;color:#555\">Scan...</div>';"
    "fetch('/wifi-scan').then(r=>r.json()).then(list=>{"
    "list.sort((a,b)=>b.rssi-a.rssi);"
    "n.innerHTML=list.map(x=>"
    "`<div class='net' onclick='pick(\"${x.ssid}\")'>`"
    "+`<span>${x.ssid}</span><span style='color:#444'>${x.rssi}dBm</span></div>`"
    ").join('');});}"
    "function pick(s){document.getElementById('ssid').value=s;}"
    // ── Save WiFi
    "function saveWifi(){"
    "const s=document.getElementById('ssid').value.trim();"
    "const p=document.getElementById('pass').value;"
    "if(!s){toast('SSID vide',true);return;}"
    "const f=new FormData();f.append('ssid',s);f.append('pass',p);"
    "fetch('/wifi-connect',{method:'POST',body:f}).then(()=>"
    "toast('WiFi sauvegardé – reconnexion en cours...')).catch(()=>toast('Erreur',true));}"
    // ── Palette
    "function rPal(){"
    "const p=document.getElementById('pal');"
    "p.innerHTML=A.map(a=>"
    "`<div style='display:flex;align-items:center;gap:.3rem;background:#070809;"
    "border:1px solid #1e2028;border-radius:4px;padding:.35rem .55rem;"
    "cursor:grab;font-size:.7rem;user-select:none' draggable='true' data-id='${a.id}'>"
    "${a.icon} ${a.label}</div>`).join('');"
    "p.querySelectorAll('[data-id]').forEach(el=>{"
    "el.ondragstart=ev=>{drag=el.dataset.id;ev.dataTransfer.effectAllowed='move';};});}"
    // ── Slots
    "function rSlots(){"
    "const g=document.getElementById('slg');"
    "g.innerHTML=cfg.map(s=>{"
    "const a=A.find(x=>x.id===s.id);"
    "let inner=a"
    "?`<div style='display:flex;align-items:center;gap:.3rem'>`"
    "+`<span>${a.icon}</span><div>`"
    "+`<div style='font-size:.72rem'>${a.label}</div>`"
    "+(a.sec?`<div style='font-size:.57rem;color:#444'>2x: ${a.sec}</div>`:'')"
    "+`</div></div>`"
    "+(a.sec?`<label style='display:flex;align-items:center;gap:.3rem;font-size:.57rem;margin-top:.25rem'>`"
    "+`<input type='checkbox' style='width:auto;margin:0' ${s.hs?'checked':''}> Activer 2x</label>`:'')"
    "+`<div style='display:flex;align-items:center;gap:.3rem;margin-top:.3rem'>`"
    "+`<span style='font-size:.55rem;color:#3a3e4a'>Couleur:</span>`"
    "+`<input type='color' value='${s.col&&s.col!=='#000000'?s.col:'#1A4D7B'}`"
    "+` style='width:30px;height:20px;padding:0;border:1px solid #1e2028;background:none;cursor:pointer'`"
    "+` onchange='cfg[${s.pos}].col=this.value'></div>`"
    "+`<button onclick='clr(${s.pos})' style='position:absolute;top:.3rem;right:.3rem;"
    "background:none;border:none;color:#333;cursor:pointer'>x</button>`"
    ":'<div style=\"font-size:.6rem;color:#252830\">-- vide --</div>';"
    "return `<div style='background:#070809;border:2px dashed #1a1c24;border-radius:6px;"
    "padding:.5rem;min-height:60px;position:relative' id='sl${s.pos}' data-pos='${s.pos}'>`"
    "+`<div style='font-size:.5rem;color:#252830;margin-bottom:.2rem'>Pos ${s.pos+1}</div>`"
    "+inner+'</div>';}).join('');"
    "cfg.forEach(s=>{"
    "const el=document.getElementById('sl'+s.pos);"
    "el.ondragover=ev=>ev.preventDefault();"
    "el.ondrop=ev=>{ev.preventDefault();cfg[s.pos].id=drag;cfg[s.pos].hs=false;rSlots();};"
    "const cb=el.querySelector('input[type=checkbox]');"
    "if(cb) cb.onchange=ev=>cfg[s.pos].hs=ev.target.checked;});}"
    // ── Quick
    "function rQ(){"
    "document.getElementById('qg').innerHTML=A.map(a=>"
    "`<div style='background:#070809;border:1px solid #1e2028;border-radius:6px;"
    "padding:.55rem;cursor:pointer;text-align:center' onclick='qa(\"${a.id}\")'>`"
    "+`<div style='font-size:1.1rem'>${a.icon}</div>`"
    "+`<div style='font-size:.62rem'>${a.label}</div></div>`).join('');}"
    "function clr(p){cfg[p].id='';cfg[p].hs=false;rSlots();}"
    "function saveSlots(){"
    "fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({slots:cfg.map(s=>({pos:s.pos,actionId:s.id,hasSec:s.hs,color:s.col}))})})"
    ".then(()=>toast('Slots sauvegardés !')).catch(()=>toast('Erreur',true));}"
    "function qa(id){fetch('/action?cmd='+id+'&sec=0').then(()=>toast('OK: '+id));}"
    "let tt;function toast(m,e=false){"
    "const el=document.getElementById('toast');el.textContent=m;"
    "el.style.borderColor=e?'#c0392b':'#D4620A';"
    "el.classList.add('on');clearTimeout(tt);tt=setTimeout(()=>el.classList.remove('on'),2500);}"
    "// Init brightness slider\n"
    "const bslider=document.getElementById('bri');if(bslider){"
    "bslider.value=BRI;"
    "document.getElementById('bri-v').textContent=Math.round(BRI*100/255)+'%';"
    "bslider.oninput=()=>document.getElementById('bri-v').textContent=Math.round(bslider.value*100/255)+'%';""}"
    "function saveBri(){"
    "const v=document.getElementById('bri').value;"
    "fetch('/brightness?v='+v).then(()=>toast('Luminosité: '+Math.round(v*100/255)+'%'));}"
    "function teslaConnect(){"
    "const s=document.getElementById('tesla-status');"
    "s.textContent='Connexion en cours...';"
    "fetch('/tesla-connect').then(r=>r.json()).then(d=>{"
    "s.textContent=d.ok?'✓ Connecté: '+d.ip:'✗ Echec connexion ESP32-C3';"
    "s.style.color=d.ok?'#22c55e':'#c0392b';"
    "toast(d.ok?'Tesla connecté !':'Echec connexion',!d.ok);"
    "});}"
    "rPal();rSlots();rQ();"
    "</script></body></html>");
  return h;
}

// ── Page Dashboard (mode STA connecté) ───────────────────────
String pageDashboard() {
  String ip = WiFi.localIP().toString();
  String aj="[";
  for(int i=0;i<N_ACTIONS;i++){
    if(i) aj+=",";
    aj+="{\"id\":\""; aj+=ALL_ACTIONS[i].id;
    aj+="\",\"label\":\""; aj+=ALL_ACTIONS[i].label;
    aj+="\",\"icon\":\""; aj+=ALL_ACTIONS[i].icon;
    aj+="\",\"sec\":\""; aj+=ALL_ACTIONS[i].labelSec; aj+="\"}";
  }
  aj+="]";
  String h;
  h.reserve(2500);
  h += F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>DYÁL3</title>");
  h += commonCSS();
  h += F("</head><body>");
  h += logoHTML();
  h += F("<div class='sub'>DASHBOARD</div><main>"
    "<div class='card'>"
    "<h2>&#9889; Actions rapides</h2>"
    "<div style='display:grid;grid-template-columns:repeat(2,1fr);gap:.5rem' id='qg'></div>"
    "</div>"
    "<div class='card'>"
    "<h2>&#9881; Configuration</h2>"
    "<a href='/config'><button class='btn'>Ouvrir la page de configuration</button></a>"
    "</div>"
    "</main>"
    "<footer>DYÁL3 " FW_VERSION " &bull; ");
  h += ip;
  h += F(" &bull; <a href='/update' style='color:#333'>OTA</a></footer>"
    "<div id='toast'></div><script>"
    "const A=");
  h += aj;
  h += F(";"
    "document.getElementById('qg').innerHTML=A.map(a=>"
    "`<div style='background:#070809;border:1px solid #1e2028;border-radius:6px;"
    "padding:.65rem;cursor:pointer;text-align:center' onclick='qa(\"${a.id}\")'>`"
    "+`<div style='font-size:1.3rem;margin-bottom:.2rem'>${a.icon}</div>`"
    "+`<div style='font-size:.65rem'>${a.label}</div></div>`).join('');"
    "function qa(id){fetch('/action?cmd='+id+'&sec=0').then(()=>toast('OK: '+id));}"
    "let tt;function toast(m,e=false){"
    "const el=document.getElementById('toast');el.textContent=m;"
    "el.style.borderColor=e?'#c0392b':'#D4620A';"
    "el.classList.add('on');clearTimeout(tt);tt=setTimeout(()=>el.classList.remove('on'),2500);}"
    "</script></body></html>");
  return h;
}

String pageOTA() {
  return F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<title>OTA</title><style>body{background:#07080a;color:#eee;font-family:monospace;"
    "display:flex;align-items:center;justify-content:center;height:100vh;text-align:center;}"
    ".c{background:#0e0f12;border:1px solid #1e2028;border-radius:10px;padding:2rem;}"
    "h2{color:#D4620A;margin-bottom:1rem;}"
    ".b{background:#D4620A;border:none;color:#fff;padding:.8rem 2rem;"
    "border-radius:5px;cursor:pointer;}</style></head><body>"
    "<div class='c'><h2>OTA – DYÁL3</h2>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='update' accept='.bin' style='margin:1rem 0'><br>"
    "<input type='submit' value='Flash' class='b'></form></div></body></html>");
}

// ═══════════════════════════════════════════════════════════════
//  HANDLERS HTTP
// ═══════════════════════════════════════════════════════════════

// Vérifie si la requête est autorisée
// En mode CONFIG AP : toujours autorisé (c'est l'objectif)
// En mode STA : autorisé uniquement si connecté
bool isAllowed() {
  // Toujours autorisé si un AP est actif ou si connecté WiFi maison
  wifi_mode_t m = WiFi.getMode();
  if(m == WIFI_AP || m == WIFI_AP_STA) return true;
  if(wifiConnected) return true;
  if(configMode) return true;
  return false;
}

void handleRoot() {
  if(!isAllowed()){ server.send(403,"text/plain","Non autorisé"); return; }
  if(configMode) {
    if(netIsConnected()) {
      HTTPClient hc;
      hc.begin("http://" NET_ESP_IP "/status");
      hc.setTimeout(600);
      majorOk = (hc.GET() == 200);
      hc.end();
    } else { majorOk=false; }
    server.send(200,"text/html",pageConfig());
  } else {
    server.send(200,"text/html",pageDashboard());
  }
}

void handleConfigPage() {
  if(!isAllowed()){ server.send(403,"text/plain","Non autorisé"); return; }
  // Ping Major avant de générer la page (timeout court)
  if(netIsConnected()) {
    HTTPClient hc;
    hc.begin("http://" NET_ESP_IP "/status");
    hc.setTimeout(600);
    majorOk = (hc.GET() == 200);
    hc.end();
  } else {
    majorOk = false;
  }
  server.send(200,"text/html",pageConfig());
}

void handleWifiScan() {
  int n=WiFi.scanNetworks();
  String j="[";
  for(int i=0;i<n;i++){
    if(i) j+=",";
    bool open=(WiFi.encryptionType(i)==WIFI_AUTH_OPEN);
    j+="{\"ssid\":\""+WiFi.SSID(i)+"\",\"rssi\":"+WiFi.RSSI(i)+",\"open\":"+(open?"true":"false")+"}";
  }
  j+="]";
  server.send(200,"application/json",j);
}

void handleWifiConnect() {
  String s=server.arg("ssid"), p=server.arg("pass");
  if(!s.length()){ server.send(400,"text/plain","SSID vide"); return; }
  prefs.begin("wifi",false);
  prefs.putString("ssid",s);
  prefs.putString("pass",p);
  prefs.end();
  savedSSID=s; savedPASS=p;
  server.send(200,"text/plain","OK");
  // Connexion en arrière-plan après réponse
  delay(300);
  if(configMode) {
    // Rester en AP config mais mémoriser pour après
    toast_serial("WiFi maison sauvegardé: "+s);
  } else {
    stopConfigAP(); // reconnecte en STA
  }
}

void toast_serial(String msg){ Serial.println("[DYAL3] "+msg); }

void handleAction() {
  if(!isAllowed()){ server.send(403,"text/plain","Non autorisé"); return; }
  String cmd=server.arg("cmd");
  bool sec=server.arg("sec")=="1";
  execAction(cmd.c_str(),sec);
  server.send(200,"text/plain","OK");
}

void handleConfig() {
  if(!isAllowed()){ server.send(403,"text/plain","Non autorisé"); return; }
  String body=server.arg("plain");
  for(int i=0;i<MAX_SLOTS;i++){ slots[i].actionId[0]='\0'; slots[i].hasSec=false; }
  int p=0;
  while((p=body.indexOf("\"pos\":",p))>=0){
    p+=6; int pos=body.substring(p).toInt();
    if(pos<0||pos>=MAX_SLOTS){p++;continue;}
    int ai=body.indexOf("\"actionId\":\"",p); if(ai<0) break;
    ai+=12; int ae=body.indexOf("\"",ai);
    String aid=body.substring(ai,ae);
    int hi=body.indexOf("\"hasSec\":",p);
    bool hs=false;
    if(hi>=0) hs=body.substring(hi+9,hi+13).indexOf("true")>=0;
    // couleur optionnelle
    int ci=body.indexOf("\"color\":\"",p);
    uint16_t col=0;
    if(ci>=0 && ci<body.indexOf("}",p)){
      ci+=9; int ce=body.indexOf("\"",ci);
      String cs=body.substring(ci,ce);
      if(cs.startsWith("#")) cs=cs.substring(1);
      // Convertir #RRGGBB → RGB565
      if(cs.length()==6){
        uint8_t r=(uint8_t)strtol(cs.substring(0,2).c_str(),nullptr,16);
        uint8_t g=(uint8_t)strtol(cs.substring(2,4).c_str(),nullptr,16);
        uint8_t b=(uint8_t)strtol(cs.substring(4,6).c_str(),nullptr,16);
        col=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
      }
    }
    if(aid.length()>0&&aid!="null"&&aid!=""){
      strncpy(slots[pos].actionId,aid.c_str(),19);
      slots[pos].hasSec=hs;
      slots[pos].color=col;
    }
    p=ae;
  }
  saveSlots(); drawIdle();
  server.send(200,"text/plain","OK");
}

void handleGetConfig() {
  if(!isAllowed()){ server.send(403,"text/plain","Non autorisé"); return; }
  String j="[";
  for(int i=0;i<MAX_SLOTS;i++){
    if(i) j+=",";
    j+="{\"pos\":"+String(i)+",\"actionId\":\""+String(slots[i].actionId)+"\",\"hasSec\":"+(slots[i].hasSec?"true":"false")+"}";
  }
  j+="]";
  server.send(200,"application/json",j);
}

void setupRoutes() {
  // Routes toujours accessibles (pas de isAllowed)
  // Connexion Tesla (ESP32-C3) à chaud
  server.on("/tesla-connect", HTTP_GET, [](){
    canvas.fillScreen(TFT_BLACK);
    canvas.fillCircle(120,120,90,C_RED);
    canvas.setTextColor(TFT_WHITE,C_RED);
    canvas.setTextSize(2);
    canvas.setTextDatum(MC_DATUM); canvas.drawString("TESLA", 120, 100);
    canvas.setTextSize(1);
    canvas.setTextDatum(MC_DATUM); canvas.drawString("Connexion...", 120, 130);
    canvas.pushSprite(0,0);
    bool ok=netBegin();
    teslaConnected=ok;
    if(ok){
      drawAction("TESLA","CONNECTE",C_GREEN);
      { String r="{\"ok\":true,\"ip\":\""+WiFi.localIP().toString()+"\"}"; server.send(200,"application/json",r); }
    } else {
      drawAction("TESLA","ECHEC",C_RED);
      server.send(200,"application/json","{\"ok\":false}");
    }
  });
  // Luminosité
  server.on("/brightness", HTTP_GET, [](){
    String v=server.arg("v");
    if(v.length()){
      brightness=(uint8_t)constrain(v.toInt(),0,255);
      saveBrightness();
      drawIdle();
    }
    char r[48]; snprintf(r,sizeof(r),"{\"brightness\":%d}",brightness);
    server.send(200,"application/json",r);
  });
  server.on("/cfg-on",  HTTP_GET, [](){
    if(!configMode){ configMode=true; startConfigAP(); drawConfigMode(); }
    server.send(200,"text/html",
      "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
      "<meta http-equiv='refresh' content='3;url=http://192.168.10.1'>"
      "<style>body{background:#07080a;color:#D4620A;font-family:monospace;"
      "display:flex;align-items:center;justify-content:center;height:100vh;"
      "text-align:center;font-size:1.1rem;}</style></head><body>"
      "<div>Mode CONFIG actif<br><br>"
      "Connectez-vous au WiFi <b>Dyal3</b><br>"
      "Cle : <b>tesladyal3</b><br><br>"
      "IP : <b>192.168.10.1</b><br><br>"
      "<small>Redirection dans 3s...</small></div></body></html>");
  });
  server.on("/cfg-off", HTTP_GET, [](){
    if(configMode){ configMode=false; stopConfigAP(); drawIdle(); }
    server.send(200,"text/plain","Mode config desactive");
  });
  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/config",     HTTP_GET,  handleConfigPage);
  server.on("/config",     HTTP_POST, handleConfig);
  server.on("/wifi-scan",  HTTP_GET,  handleWifiScan);
  server.on("/wifi-connect",HTTP_POST,handleWifiConnect);
  server.on("/action",     HTTP_GET,  handleAction);
  server.on("/update",     HTTP_GET,  [](){ server.send(200,"text/html",pageOTA()); });
  server.on("/reboot",     HTTP_GET,  [](){ server.send(200,"text/plain","OK"); delay(300); ESP.restart(); });
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
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();
  if(!configMode){
    handleEncoder();
    handleButton();
    // Feedback barre progression appui long (sans bloquer)
    static bool barVisible = false;
    if(digitalRead(ENC_BTN)==LOW && !longPressFlag) {
      uint32_t held=millis()-btnPressedAt;
      if(held>400 && held<2000) {
        suppressUntil = millis() + 400;
        barVisible = true;
        int w=(int)(180L*held/2000);
        canvas.fillScreen(TFT_BLACK);
        canvas.setTextColor(TFT_WHITE,TFT_BLACK);
        canvas.setTextSize(2);
        canvas.setTextDatum(MC_DATUM); canvas.drawString("SETUP", 120, 20);
        canvas.fillRoundRect(30,112,180,10,5,0x2104);
        if(w>0) canvas.fillRoundRect(30,112,w,10,5,TFT_WHITE);
        canvas.pushSprite(0,0);
      }
    } else if(barVisible && digitalRead(ENC_BTN)==HIGH && !longPressFlag) {
      // Relâché avant 2s : revenir à l'action sans l'activer
      barVisible = false;
      resetEncoderSync();
      drawIdle();
    }
  }
  checkTouch();

  if(actionClearAt>0 && millis()>=actionClearAt){
    actionClearAt=0;
    if(!configMode) drawIdle();
  }
  if(battHeatActive && millis()-lastHeatMs>=500){
    lastHeatMs=millis();
    uint8_t d[8]={0x05,0,0,0,0,0,0,0};
    netCanSendA(0x082,d,8);
  }
  netLoop();  // lecture UDP trames reçues de l'ESP

  // Ping déclenché par rotation molette (pingTask sur core 0)
}

// Cercle de connexion : vert=Major ok, rouge=non connecté
#define RING_COL (majorOk ? C_GREEN : C_RED)

void drawDots(int cur, int total) {
  if(total<=0) return;
  int startX = 120-(total*10)/2;
  for(int i=0;i<total;i++)
    canvas.fillCircle(startX+i*10+4,214,3,(i==cur)?TFT_WHITE:0x4208);
}

void drawIdle() {
  canvas.fillScreen(TFT_BLACK);

  if(nSlots==0) {
    canvas.setTextColor(0x4208,TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextDatum(MC_DATUM); canvas.drawString("Aucun slot", 120, 95);
    canvas.setTextSize(1);
    canvas.setTextColor(0x3084,TFT_BLACK);
    canvas.setTextDatum(MC_DATUM); canvas.drawString("Appui long = menu", 120, 130);
    canvas.drawCircle(120,108,90,RING_COL);
  } else {
    int idx = activeSlotIndex(curSlot);
    if(idx>=0) {
      const Slot &s = slots[idx];
      const Action *a = findAction(s.actionId);
      if(a) {
        uint16_t bg = s.color ? s.color : a->colorBg;
        canvas.fillCircle(120,108,82,bg);
        canvas.drawCircle(120,108,84,RING_COL);
        canvas.drawCircle(120,108,85,RING_COL);
        canvas.setTextColor(TFT_WHITE,bg);
        canvas.setTextSize(3);
        canvas.setTextDatum(MC_DATUM); canvas.drawString(a->icon, 120, 82);
        canvas.setTextSize(2);
        canvas.setTextDatum(MC_DATUM); canvas.drawString(a->label, 120, 128);
        if(s.hasSec && a->labelSec[0]) {
          canvas.setTextColor(0x8410,TFT_BLACK);
          canvas.setTextSize(1);
          String sec2="2x: "; sec2+=a->labelSec;
          canvas.setTextDatum(MC_DATUM); canvas.drawString(sec2.c_str(), 120, 218);
        }
      }
    } else {
      canvas.setTextColor(0x4208,TFT_BLACK);
      canvas.setTextSize(2);
      canvas.setTextDatum(MC_DATUM); canvas.drawString("Aucun slot", 120, 95);
    }
    drawDots(curSlot, nSlots);
  }

  // Numéro slot
  if(nSlots>0) {
    char buf[12]; snprintf(buf,sizeof(buf),"%d/%d",curSlot+1,nSlots);
    canvas.setTextColor(0x2104,TFT_BLACK);
    canvas.setTextSize(1);
    canvas.setTextDatum(MC_DATUM); canvas.drawString(buf, 120, 16);
  }

  canvas.pushSprite(0,0);
}

// ─── MENU APPUI LONG ─────────────────────────────────────────
// 0=BRIGHT 1=CAN-A 2=CAN-B 3=REBOOT 4=EXIT
#define MENU_COUNT 5
void drawLongMenu(uint8_t sel) {
  const char* labels[] = {"BRIGHT","CAN-A","CAN-B","REBOOT","EXIT"};
  uint16_t    colors[] = {0xFFE0, C_TEAL, C_TEAL, C_RED, 0x4208};
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,108,90,0x0C0E);
  canvas.drawCircle(120,108,92,DYAL_ORANGE);
  canvas.setTextColor(DYAL_ORANGE,0x0C0E);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("MENU", 120, 38);
  canvas.setTextColor(colors[sel],0x0C0E);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString(labels[sel], 120, 100);
  canvas.setTextColor(0x3084,TFT_BLACK);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("< tourner   clic = ok >", 120, 145);
  // 6 points
  for(int i=0;i<MENU_COUNT;i++)
    canvas.fillCircle(90+i*10,165,3,(i==sel)?DYAL_ORANGE:0x2104);
  canvas.pushSprite(0,0);
}

// Écran SETUP en cours
void drawSetupScreen() {
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,108,88,DYAL_BLUE);
  canvas.drawCircle(120,108,90,DYAL_ORANGE);
  canvas.setTextColor(TFT_WHITE,DYAL_BLUE);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("SETUP", 120, 76);
  canvas.setTextSize(1);
}

// Attendre un clic bouton (vide aussi les flags)
void waitClick() {
  shortPressFlag = false;
  longPressFlag  = false;
  while(!shortPressFlag) delay(20);
  shortPressFlag = false;
  longPressFlag  = false;
}

void activateSetup() {
  drawSetupScreen();
  canvas.setTextColor(DYAL_ORANGE,DYAL_BLUE);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("WiFi...", 120, 108);
  canvas.pushSprite(0,0);

  // En mode SETUP : se connecter au WiFi maison
  // On quitte le Major temporairement
  WiFi.softAPdisconnect(false);
  WiFi.disconnect(false); delay(100);

  // Tenter WiFi maison
  if(savedSSID.length()) {
    WiFi.setHostname(SECRET_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
    uint32_t t=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t<10000) delay(200);
    if(WiFi.status()==WL_CONNECTED) {
      wifiConnected=true;
      setupRoutes();
      server.begin();
      drawSetupScreen();
      canvas.setTextColor(C_GREEN,DYAL_BLUE);
      canvas.setTextDatum(MC_DATUM); canvas.drawString("WiFi OK", 120, 88);
      canvas.setTextSize(2);
      canvas.setTextDatum(MC_DATUM); canvas.drawString(WiFi.localIP().toString().c_str(), 120, 108);
      canvas.setTextColor(0x4208,DYAL_BLUE);
      canvas.setTextSize(1);
      canvas.setTextDatum(MC_DATUM); canvas.drawString("/config dans navigateur", 120, 132);
      canvas.setTextDatum(MC_DATUM); canvas.drawString("clic = quitter", 120, 150);
      canvas.pushSprite(0,0);
      buzz(40); delay(80); buzz(40);
      waitClick();
      drawIdle();
      return;
    }
  }
  // Pas de WiFi → AP DYAL3_setup
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig({192,168,4,1},{192,168,4,1},{255,255,255,0});
  WiFi.softAP(SECRET_SETUP_SSID,SECRET_SETUP_PASS);
  setupRoutes();
  server.begin();
  drawSetupScreen();
  canvas.setTextColor(DYAL_ORANGE,DYAL_BLUE);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("AP: DYAL3_setup", 120, 88);
  canvas.setTextColor(TFT_WHITE,DYAL_BLUE);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("192.168.4.1", 120, 108);
  canvas.setTextColor(0x4208,DYAL_BLUE);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("cle: tesladyal3", 120, 132);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("clic = quitter", 120, 150);
  canvas.pushSprite(0,0);
  waitClick();
  drawIdle();
}

// Écran réglage luminosité (standalone, appelé depuis le menu)
void drawBrightScreen() {
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,108,105,0x4010);  // fond bleu couvre tout
  canvas.drawCircle(120,108,107,DYAL_ORANGE);
  canvas.setTextColor(TFT_WHITE,0x4010);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("BRIGHT", 120, 52);
  // Barre de luminosité
  canvas.fillRoundRect(30,88,180,14,7,0x2104);
  int bw=(int)(180L*brightness/255);
  if(bw>0) canvas.fillRoundRect(30,88,bw,14,7,0xFFE0);
  // % affiché avec fond bleu
  char bb[8]; snprintf(bb,sizeof(bb),"%d%%",(int)(brightness*100/255));
  canvas.setTextColor(TFT_WHITE,0x4010);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString(bb, 120, 112);
  canvas.setTextColor(0xC618,0x4010);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("tourner = regler", 120, 130);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("clic = sauvegarder", 120, 144);
  canvas.pushSprite(0,0);
}

// Naviguer dans le menu — GPIO direct, state machine propre
uint8_t runMenu() {
  uiMode = UI_MENU;
  // Attendre relâchement physique + reset état clic
  while(digitalRead(ENC_BTN)==LOW) delay(10);
  uiResetClickState();
  // Attendre que la fenêtre de garde soit écoulée
  while(millis() < suppressUntil) delay(10);

  uint8_t sel = 0;
  int lastE = encCount;
  drawLongMenu(sel);

  while(true) {
    int enc = encCount;
    if(enc!=lastE) {
      int d=enc-lastE; lastE=enc;
      sel=(sel+d+MENU_COUNT)%MENU_COUNT;
      buzzClick();
      drawLongMenu(sel);
    }
    if(digitalRead(ENC_BTN)==LOW) {
      delay(40);
      if(digitalRead(ENC_BTN)==LOW) {
        while(digitalRead(ENC_BTN)==LOW) delay(10);
        uiResetClickState();
        while(millis() < suppressUntil) delay(10);
        buzzBeep();
        break;
      }
    }
    delay(20);
  }
  return sel;
}


// ── Moniteur CAN temps réel ──────────────────────────────────
// Affiche les dernières trames reçues depuis le Major via UDP
// bus : 'A' ou 'B'  — clic = retour au menu
void viewCanBus(char bus) {
  uiMode = (bus=='A') ? UI_CAN_A : UI_CAN_B;
  while(digitalRead(ENC_BTN)==LOW) delay(10);
  uiResetClickState();
  while(millis() < suppressUntil) delay(10);
  // Vider buffer CAN
  { CanFrame fr; while(canBufPop(fr)){} }

  #define CAN_LINES 8
  struct CanLine { char txt[28]; uint32_t ts; bool valid; };
  CanLine lines[CAN_LINES];
  memset(lines,0,sizeof(lines));
  int lineCount=0, lineNext=0;
  uint32_t lastDraw=0;

  // Afficher immédiatement un premier écran
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextColor(C_TEAL,TFT_BLACK);
  canvas.setTextSize(2);
  char title[8]; snprintf(title,sizeof(title),"CAN-%c",bus);
  canvas.setTextDatum(MC_DATUM); canvas.drawString(title, 120, 12);
  canvas.setTextColor(TFT_WHITE,TFT_BLACK);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("NO STREAM", 120, 108);
  canvas.setTextColor(0x3084,TFT_BLACK);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("clic = quitter", 120, 228);
  canvas.drawCircle(120,108,118,RING_COL);
  canvas.pushSprite(0,0);

  while(true) {
    // Sortie sur clic GPIO direct (pas shortPressFlag)
    if(digitalRead(ENC_BTN)==LOW) {
      delay(30);
      if(digitalRead(ENC_BTN)==LOW) {
        while(digitalRead(ENC_BTN)==LOW) delay(10);
        delay(200);
        shortPressFlag=false; longPressFlag=false;
        break;
      }
    }
    bool newFrame=false;
    CanFrame f;
    while(canBufPop(f)) {
      if(f.bus != bus) continue;
      char hex[17]="";
      for(int i=0;i<f.dlc&&i<8;i++)
        snprintf(hex+i*2,sizeof(hex)-i*2,"%02X",f.data[i]);
      snprintf(lines[lineNext].txt,sizeof(lines[lineNext].txt),
        "0x%03X [%d] %s",f.id,f.dlc,hex);
      lines[lineNext].ts=millis();
      lines[lineNext].valid=true;
      lineNext=(lineNext+1)%CAN_LINES;
      if(lineCount<CAN_LINES) lineCount++;
      newFrame=true;
    }

    if(newFrame || millis()-lastDraw>200) {
      lastDraw=millis();
      canvas.fillScreen(TFT_BLACK);
      canvas.setTextColor(C_TEAL,TFT_BLACK);
      canvas.setTextSize(2);
      canvas.setTextDatum(MC_DATUM); canvas.drawString(title, 120, 12);
      canvas.setTextColor(0x3084,TFT_BLACK);
      canvas.setTextSize(1);
      canvas.setTextDatum(MC_DATUM); canvas.drawString("clic = quitter", 120, 228);
      canvas.drawCircle(120,108,118,RING_COL);
      if(lineCount==0) {
        canvas.setTextColor(TFT_WHITE,TFT_BLACK);
        canvas.setTextSize(2);
        canvas.setTextDatum(MC_DATUM); canvas.drawString("NO STREAM", 120, 108);
      } else {
        int y=36;
        for(int i=0;i<lineCount&&i<CAN_LINES;i++){
          int idx=(lineNext-1-i+CAN_LINES)%CAN_LINES;
          uint16_t col=(millis()-lines[idx].ts<800)?TFT_WHITE:0x4208;
          canvas.setTextColor(col,TFT_BLACK);
          canvas.setTextSize(1);
          canvas.setTextDatum(TL_DATUM);
          canvas.drawString(lines[idx].txt,6,y);
          canvas.setTextDatum(MC_DATUM);
          y+=20;
        }
      }
      canvas.pushSprite(0,0);
    }
    delay(20);
  }
  // Sortie propre : reset état clic, fenêtre de garde
  uiMode = UI_HOME;
  uiResetClickState();
  while(millis() < suppressUntil) delay(10);
}

void handleLongPress() {
  buzz(80);
  uiMode = UI_MENU;
  uiResetClickState();
  while(millis() < suppressUntil) delay(10);

  // ── Boucle principale du menu ─────────────────────────────
  // On ne quitte cette boucle QUE sur EXIT (menuSel==4)
  // Toutes les autres actions reviennent ici après exécution
  while(true) {
    uint8_t menuSel = runMenu();

    if(menuSel==0) {
      // BRIGHT : réglage molette, clic = sauvegarder + retour menu
      uiResetClickState();
      while(millis() < suppressUntil) delay(10);
      int lastE2=encCount;
      drawBrightScreen();
      while(true) {
        int enc=encCount;
        if(enc!=lastE2) {
          int d=enc-lastE2; lastE2=enc;
          int b=(int)brightness+d*16;
          if(b<0) b=0; if(b>255) b=255;
          brightness=(uint8_t)b;
          display.setBrightness(brightness);
          buzzClick();
          drawBrightScreen();
        }
        if(shortPressFlag) {
          shortPressFlag=false; longPressFlag=false;
          break;
        }
        delay(20);
      }
      saveBrightness();
      uiResetClickState();
      while(millis() < suppressUntil) delay(10);
      // Retour au menu (continue la boucle)

    } else if(menuSel==1) {
      // CAN-A
      viewCanBus('A');
      uiMode = UI_MENU;
      uiResetClickState();
      while(millis() < suppressUntil) delay(10);

    } else if(menuSel==2) {
      // CAN-B
      viewCanBus('B');
      uiMode = UI_MENU;
      uiResetClickState();
      while(millis() < suppressUntil) delay(10);

    } else if(menuSel==3) {
      // REBOOT
      canvas.fillScreen(TFT_BLACK);
      canvas.fillCircle(120,108,88,C_RED);
      canvas.setTextColor(TFT_WHITE,C_RED);
      canvas.setTextSize(2);
      canvas.setTextDatum(MC_DATUM); canvas.drawString("REBOOT", 120, 100);
      canvas.pushSprite(0,0);
      buzz(60); delay(300);
      ESP.restart();

    } else {
      // EXIT (menuSel==4) : SEUL moyen de sortir du menu
      break;
    }
  }

  // Sortie propre vers UI_HOME
  uiMode = UI_HOME;
  uiResetClickState();
  while(millis() < suppressUntil) delay(10);
  resetEncoderSync();  // absorber rotations accumulées pendant le menu
  drawIdle();
}


// Force la resynchronisation de lastCnt dans handleEncoder
// À appeler après toute sortie de menu
void resetEncoderSync() {
  // On passe temporairement en UI_MENU le temps d'un appel
  // pour que handleEncoder fasse lastCnt=encCount sans déclencher d'action
  UiMode saved = uiMode;
  uiMode = UI_MENU;
  handleEncoder();
  uiMode = saved;
}

void handleEncoder(){
  static int lastCnt=0;
  // Pendant menu : resynchroniser lastCnt sans agir
  if(uiMode != UI_HOME) { lastCnt=encCount; return; }
  if(encCount==lastCnt) return;
  int d=encCount-lastCnt; lastCnt=encCount;

  if(brightMode) {
    int b=(int)brightness+d*16;
    if(b<0) b=0; if(b>255) b=255;
    brightness=(uint8_t)b;
    display.setBrightness(brightness);
    buzzClick(); drawIdle();
  } else if(nSlots>0) {
    curSlot=(curSlot+d+nSlots)%nSlots;
    buzzClick();
    pingRequest=true;  // ping Major au prochain slot
    drawIdle();
  }
}

// ── Tâche ping Major : non-bloquante pour loop() ────────────
// Déclenchée par pingRequest=true depuis handleEncoder
// Après 2 pings consécutifs sans réponse → reconnexion (max 3s)
static void pingTask(void*) {
  HTTPClient http;
  int failCount = 0;
  for(;;) {
    if(pingRequest) {
      pingRequest = false;
      bool ok = false;

      if(netIsConnected()) {
        http.begin("http://" NET_ESP_IP "/status");
        http.setTimeout(600);
        int code = http.GET();
        http.end();
        ok = (code == 200);
      }

      if(ok) {
        failCount = 0;
        if(!majorOk) { majorOk=true; drawIdle(); }
      } else {
        failCount++;
        if(majorOk) { majorOk=false; drawIdle(); }

        if(failCount >= 2) {
          // 2 échecs consécutifs → tenter reconnexion (max 3s)
          failCount = 0;
          Serial.println("[PING] 2 échecs → reconnexion Major...");
          WiFi.disconnect(false);
          WiFi.begin(NET_SSID, NET_PASS);
          uint32_t t = millis();
          while(WiFi.status()!=WL_CONNECTED && millis()-t<3000)
            vTaskDelay(pdMS_TO_TICKS(100));
          bool reconnected = (WiFi.status()==WL_CONNECTED);
          // Mettre à jour netConnected dans dyal3_network.h
          extern bool netConnected;
          netConnected = reconnected;
          if(reconnected) {
            // Vérifier que le Major répond
            http.begin("http://" NET_ESP_IP "/status");
            http.setTimeout(600);
            int code = http.GET();
            http.end();
            majorOk = (code == 200);
          } else {
            majorOk = false;
          }
          drawIdle();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ── Tâche FreeRTOS : surveille le bouton indépendamment de loop()
// Positionne longPressFlag ou shortPressFlag selon la durée
static void btnTask(void*) {
  bool prev = HIGH;
  bool longFired = false;
  for(;;) {
    bool now = digitalRead(ENC_BTN);
    uint32_t ms = millis();

    if(now==LOW && prev==HIGH) {
      btnPressedAt = ms;  // global, lu aussi par loop() pour la barre
      longFired = false;
    }

    if(now==LOW && !longFired) {
      uint32_t held = ms - btnPressedAt;
      if(held >= 2000) {
        longFired = true;
        longPressFlag = true;  // jamais bloqué par suppressUntil
      }
    }

    if(now==HIGH && prev==LOW) {
      uint32_t held = ms - btnPressedAt;
      if(!longFired && held >= 30 && millis() > suppressUntil)
        shortPressFlag = true;  // signaler à loop()
    }

    prev = now;
    vTaskDelay(pdMS_TO_TICKS(10));  // polling 10ms — précis, non-bloquant
  }
}

void handleButton(){
  // Appelé depuis loop() — traite les flags posés par btnTask
  // Guard : hors UI_HOME, ignorer tout (menus autogèrent)
  if(uiMode != UI_HOME) {
    shortPressFlag = false;
    longPressFlag  = false;
    return;
  }


  // ── Appui long ────────────────────────────────────────────────
  if(longPressFlag) {
    longPressFlag  = false;
    shortPressFlag = false;  // vider — le clic du menu ne doit pas déclencher une action
    // Attendre relâchement
    while(digitalRead(ENC_BTN)==LOW) delay(10);
    handleLongPress();
    uiMode = UI_HOME;
    uiResetClickState();
    while(millis() < suppressUntil) delay(10);
    return;
  }

  // ── Clic court ────────────────────────────────────────────────
  if(shortPressFlag) {
    shortPressFlag = false;
    // Ignorer si appui > 400ms (intention menu avortée)
    if(millis() < suppressUntil) return;
    if(brightMode) {
      brightMode=false;
      saveBrightness();
      drawIdle();
    } else {
      if(nSlots>0) {
        int idx=activeSlotIndex(curSlot);
        if(idx>=0 && slots[idx].actionId[0]) {
          majorOk=netIsConnected();
          buzzBeep();
          execAction(slots[idx].actionId,false);
        }
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  pinMode(PIN_HOLD,OUTPUT); digitalWrite(PIN_HOLD,HIGH);
  Serial.begin(115200); delay(300);
  pinMode(PIN_BUZZ,OUTPUT);
  pinMode(ENC_A,INPUT_PULLUP);
  pinMode(ENC_B,INPUT_PULLUP);
  pinMode(ENC_BTN,INPUT_PULLUP);
  encLast=digitalRead(ENC_A);
  attachInterrupt(digitalPinToInterrupt(ENC_A),encISR,CHANGE);
  Wire.begin(TOUCH_SDA,TOUCH_SCL);

  // Charger luminosité
  prefs.begin("slots",true);
  brightness=prefs.getUChar("bright",128);
  prefs.end();

  display.begin();
  display.setRotation(1);
  // Tâche surveillance bouton
  xTaskCreatePinnedToCore(btnTask,"btn",2048,nullptr,2,nullptr,0);
  // Tâche ping Major (non-bloquante)
  xTaskCreatePinnedToCore(pingTask,"ping",4096,nullptr,1,nullptr,0);
  display.setBrightness(brightness);
  display.fillScreen(TFT_BLACK);
  canvas.createSprite(240,240);
  canvas.setTextDatum(MC_DATUM);

  // ── Splash ─────────────────────────────────────────────────
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,120,105,DYAL_BLUE);
  canvas.drawCircle(120,120,106,DYAL_ORANGE);
  canvas.drawCircle(120,120,107,DYAL_ORANGE);
  canvas.setTextColor(TFT_WHITE,DYAL_BLUE);
  canvas.setTextSize(3);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("DYAL3", 120, 88);
  canvas.setTextColor(DYAL_ORANGE,DYAL_BLUE);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("CAN CONTROLLER " FW_VERSION, 120, 125);
  canvas.pushSprite(0,0);
  buzz(60); delay(120); buzz(60);
  delay(1000);

  // ── Connexion Major (ESP32-C3) ─────────────────────────────
  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(120,108,88,C_RED);
  canvas.drawCircle(120,108,90,DYAL_ORANGE);
  canvas.setTextColor(TFT_WHITE,C_RED);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("MAJOR", 120, 80);
  canvas.setTextSize(1);
  canvas.setTextColor(0xFD20,C_RED);
  canvas.setTextDatum(MC_DATUM); canvas.drawString("connexion...", 120, 112);
  canvas.pushSprite(0,0);

  majorOk = netBegin();

  // ── Charger config ─────────────────────────────────────────
  loadSlots();
  prefs.begin("wifi",true);
  savedSSID=prefs.getString("ssid","");
  savedPASS=prefs.getString("pass","");
  prefs.end();

  // AP interne pour accès page config (coexiste avec STA Major)
  // WIFI_AP_STA déjà activé par netBegin()
  WiFi.softAPConfig(
    IPAddress(192,168,10,1),
    IPAddress(192,168,10,1),
    IPAddress(255,255,255,0));
  WiFi.softAP(SECRET_AP_SSID,SECRET_AP_PASS,1,false,2);
  Serial.println("[AP] DYAL3-M5 -> 192.168.10.1");

  setupRoutes();
  server.begin();
  drawIdle();
  Serial.println("DYAL3 pret.");
}
