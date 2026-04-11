#pragma once
// dyal3_network.h – Surcouche réseau DYÁL3 M5Stack
// SSID   : Dyal3-CAN (caché)
// Clé    : tesladyal3
// IP     : 192.168.20.1
// HTTP   : GET /?bus=A&id=0x273&data=C1A00000C8023002
// UDP RX : port 20001, format "BUS_A:0x273:C1A0...\n"

#include <WiFi.h>
#include "secrets.h"
#include <WiFiUdp.h>
#include <HTTPClient.h>

// ─── CONFIG ──────────────────────────────────────────────────
#define NET_SSID         "Dyal3-CAN"
#define NET_PASS         "tesladyal3"
#define NET_ESP_IP       "192.168.20.1"
#define NET_ESP_PORT     80
#define UDP_LISTEN_PORT  20001
#define HTTP_TIMEOUT_MS  400   // timeout HTTP par requête

// ─── ÉTAT ────────────────────────────────────────────────────
bool      netConnected = false;
bool      netEspOnline = false;
uint32_t  netRxFrames  = 0;
uint32_t  netTxFrames  = 0;

WiFiUDP   netUdp;

// ─── RING BUFFER RX ──────────────────────────────────────────
#define CAN_BUF_SIZE 64
struct CanFrame {
  char     bus;
  uint16_t id;
  uint8_t  data[8];
  uint8_t  dlc;
  uint32_t ts;
};
CanFrame canRxBuf[CAN_BUF_SIZE];
uint8_t  canRxHead = 0, canRxTail = 0, canRxCount = 0;

void canBufPush(char bus, uint16_t id, const uint8_t *data, uint8_t dlc) {
  canRxBuf[canRxHead] = {bus, id, {}, dlc, millis()};
  memcpy(canRxBuf[canRxHead].data, data, min((int)dlc,8));
  canRxHead = (canRxHead+1) % CAN_BUF_SIZE;
  if(canRxCount < CAN_BUF_SIZE) canRxCount++;
  else canRxTail = (canRxTail+1) % CAN_BUF_SIZE;
}
bool canBufPop(CanFrame &out) {
  if(!canRxCount) return false;
  out = canRxBuf[canRxTail];
  canRxTail = (canRxTail+1) % CAN_BUF_SIZE;
  canRxCount--;
  return true;
}

// ═════════════════════════════════════════════════════════════
//  FILE D'ENVOI NON-BLOQUANTE
//  Les trames à envoyer sont mises dans une queue FreeRTOS.
//  Une tâche dédiée (core 0) fait les HTTP sans bloquer loop().
// ═════════════════════════════════════════════════════════════

struct NetTxJob {
  char    bus;
  uint16_t id;
  uint8_t  data[8];
  uint8_t  dlc;
};

static QueueHandle_t  txQueue   = nullptr;
static TaskHandle_t   txTask    = nullptr;
static volatile bool  txRunning = false;

// Tâche sur core 0 — fait les GET HTTP
static void netTxWorker(void*) {
  txRunning = true;
  HTTPClient http;
  NetTxJob job;
  while(true) {
    if(xQueueReceive(txQueue, &job, portMAX_DELAY) == pdTRUE) {
      if(!netConnected) continue;
      char hex[17]="";
      for(int i=0;i<job.dlc&&i<8;i++)
        snprintf(hex+i*2, sizeof(hex)-i*2, "%02X", job.data[i]);
      char url[128];
      snprintf(url,sizeof(url),
        "http://%s/?bus=%c&id=0x%03X&data=%s",
        NET_ESP_IP, job.bus, job.id, hex);
      http.begin(url);
      http.setTimeout(HTTP_TIMEOUT_MS);
      int code = http.GET();
      http.end();
      if(code==200) { netTxFrames++; netEspOnline=true; }
      else           { netEspOnline=false; }
    }
  }
}

// ═════════════════════════════════════════════════════════════
//  CONNEXION
// ═════════════════════════════════════════════════════════════
bool netBegin() {
  // Créer la queue et la tâche si pas encore fait
  if(!txQueue) {
    txQueue = xQueueCreate(16, sizeof(NetTxJob));
    xTaskCreatePinnedToCore(netTxWorker,"netTX",4096,nullptr,1,&txTask,0);
  }

  // Mode AP_STA : connecté au Major (STA) ET AP interne pour config web
  // L'AP interne (192.168.10.1) est géré par dyal3.ino
  // Ici on se connecte uniquement au Major en STA
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname("DYAL3-M5");
  WiFi.begin(NET_SSID, NET_PASS);
  uint32_t t = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t<10000) delay(200);

  if(WiFi.status()==WL_CONNECTED) {
    netConnected = true;
    netUdp.begin(UDP_LISTEN_PORT);
    Serial.printf("[NET] Connecté Major %s\n", NET_ESP_IP);
    return true;
  }
  Serial.println("[NET] Echec connexion Major");
  return false;
}

// ═════════════════════════════════════════════════════════════
//  ENVOI TRAME – non-bloquant (met dans la queue)
// ═════════════════════════════════════════════════════════════
bool netCanSend(char bus, uint16_t id, const uint8_t *data, uint8_t dlc) {
  if(!netConnected || !txQueue) return false;
  NetTxJob job;
  job.bus=bus; job.id=id; job.dlc=min((int)dlc,8);
  memcpy(job.data,data,job.dlc);
  return xQueueSend(txQueue, &job, 0) == pdTRUE;  // 0 = non-bloquant
}

bool netCanSendA(uint16_t id, const uint8_t *data, uint8_t dlc) {
  return netCanSend('A', id, data, dlc);
}

// ═════════════════════════════════════════════════════════════
//  LECTURE UDP – appelé dans loop(), non-bloquant
//  Ping Major : fait par la tâche TX (retour HTTP 200 ou pas)
// ═════════════════════════════════════════════════════════════
void netParseUdp(const char *pkt, int len) {
  char buf[80]; int l=min(len,79);
  memcpy(buf,pkt,l); buf[l]='\0';
  if(l>0&&buf[l-1]=='\n') buf[--l]='\0';
  if(l<7||strncmp(buf,"BUS_",4)!=0) return;
  char bus=buf[4];
  if(bus!='A'&&bus!='B') return;
  char *p1=strchr(buf,':'); if(!p1) return;
  char *p2=strchr(p1+1,':'); if(!p2) return;
  char idStr[8]; int il=min((int)(p2-p1-1),7);
  strncpy(idStr,p1+1,il); idStr[il]='\0';
  uint16_t canId=(uint16_t)strtol(idStr,nullptr,16);
  char *h=p2+1; int hl=strlen(h);
  uint8_t d[8]={0}; uint8_t dlc=min(hl/2,8);
  for(int i=0;i<dlc;i++){char c[3]={h[i*2],h[i*2+1],'\0'};d[i]=(uint8_t)strtol(c,nullptr,16);}
  canBufPush(bus,canId,d,dlc);
  netRxFrames++;
}

void netLoop() {
  if(!netConnected) return;
  int sz;
  while((sz=netUdp.parsePacket())>0) {
    char pkt[80]={0};
    int len=netUdp.read(pkt,sizeof(pkt)-1);
    if(len>0) netParseUdp(pkt,len);
  }
  // Surveiller déconnexion WiFi
  if(WiFi.status()!=WL_CONNECTED) netConnected=false;
}

// ─── ACCESSEURS ──────────────────────────────────────────────
bool     netIsConnected()  { return netConnected; }
bool     netIsEspOnline()  { return netEspOnline; }
uint8_t  netRxAvailable()  { return canRxCount; }
uint32_t netGetRxCount()   { return netRxFrames; }
uint32_t netGetTxCount()   { return netTxFrames; }
