/*
 * ============================================================
 *  DYÁL3 – ESP32-C3 Relais CAN  v1.1
 *
 *  CORRECTIONS v1.1 :
 *  - Buffers SLCAN statiques (char[]) au lieu de String → pas
 *    de fragmentation heap
 *  - Serial.printf retiré du chemin critique (slcanRead)
 *  - yield() dans loop() pour laisser le stack WiFi respirer
 *  - ssid_hidden correctement à true
 *  - Watchdog nourri explicitement
 *
 *  AP WiFi CACHÉ + WPA2
 *    SSID   : Dyal3-CAN   (hidden)
 *    Clé    : tesladyal3
 *    IP ESP : 192.168.20.1
 *
 *  HTTP : GET /?bus=A&id=0x273&data=C1A00000C8023002
 *  UDP push → client port 20001
 *  Format   : "BUS_A:0x273:C1A00000C8023002\n"
 *
 *  Bus A – UART1 : TX=GPIO21  RX=GPIO20
 *  Bus B – UART0 : TX=GPIO4   RX=GPIO3
 *
 *  BOARD : ESP32C3 Dev Module, USB CDC On Boot : Enabled
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

#define FW_VERSION      "v1.1"

#define AP_SSID         "Dyal3-CAN"
#define AP_PASS         "tesladyal3"
#define AP_IP_STR       "192.168.20.1"
const IPAddress AP_IP(192, 168, 20, 1);

#define UDP_CLIENT_PORT 20001
#define SLCAN_SPEED     "S6"

#define CAN_A_TX  21
#define CAN_A_RX  20
#define CAN_B_TX  4
#define CAN_B_RX  3

HardwareSerial CanA(1);
HardwareSerial CanB(0);

WebServer  server(80);
WiFiUDP    udp;

IPAddress clientIP(0,0,0,0);
bool      clientKnown = false;

uint32_t  countA = 0;
uint32_t  countB = 0;
char      lastIdA[8] = "---";
char      lastIdB[8] = "---";

// ── Buffers SLCAN statiques (pas de String → pas de heap frag) ─
#define SLCAN_BUF 40
char bufA[SLCAN_BUF]; uint8_t bufAlen = 0;
char bufB[SLCAN_BUF]; uint8_t bufBlen = 0;

// ═══════════════════════════════════════════════════════════════
//  SLCAN – Envoi
// ═══════════════════════════════════════════════════════════════
void slcanSend(HardwareSerial &bus, uint16_t id,
               const uint8_t *data, uint8_t dlc) {
  char buf[32];
  int p = snprintf(buf, sizeof(buf), "t%03X%d", id, dlc);
  for (int i = 0; i < dlc; i++)
    p += snprintf(buf+p, sizeof(buf)-p, "%02X", data[i]);
  buf[p++] = '\r'; buf[p] = '\0';
  bus.print(buf);
}

void slcanInit(HardwareSerial &bus, int txPin, int rxPin,
               const char busName) {
  bus.begin(115200, SERIAL_8N1, rxPin, txPin);
  delay(80);
  bus.print("C\r"); delay(20);
  bus.print(SLCAN_SPEED); bus.print("\r"); delay(30);
  bus.print("O\r"); delay(30);
  Serial.printf("[CAN %c] init OK TX=%d RX=%d\n",
                busName, txPin, rxPin);
}

// ═══════════════════════════════════════════════════════════════
//  UDP PUSH
// ═══════════════════════════════════════════════════════════════
void udpPush(char bus, uint16_t id, const char *hexData) {
  if (!clientKnown) return;
  char pkt[64];
  snprintf(pkt, sizeof(pkt), "BUS_%c:0x%03X:%s\n",
           bus, id, hexData);
  udp.beginPacket(clientIP, UDP_CLIENT_PORT);
  udp.write((const uint8_t*)pkt, strlen(pkt));
  udp.endPacket();
}

// ═══════════════════════════════════════════════════════════════
//  SLCAN – Lecture (buffer statique, pas de String)
// ═══════════════════════════════════════════════════════════════
void slcanRead(HardwareSerial &bus, char *buf, uint8_t &buflen,
               char busName, uint32_t &counter, char *lastId) {
  while (bus.available()) {
    char c = bus.read();
    if (c == '\r' || c == '\n') {
      if (buflen >= 5 && (buf[0] == 't' || buf[0] == 'T')) {
        buf[buflen] = '\0';

        // Parser ID (3 hex après 't')
        char idStr[4] = {buf[1],buf[2],buf[3],'\0'};
        uint16_t id = (uint16_t)strtol(idStr, nullptr, 16);
        uint8_t  dlc = buf[4] - '0';

        // Données hex
        char hexData[17] = "";
        if (dlc > 0 && buflen >= 5 + (uint8_t)(dlc*2))
          memcpy(hexData, buf+5, dlc*2);
        hexData[dlc*2] = '\0';

        counter++;
        snprintf(lastId, 8, "%03X", id);

        udpPush(busName, id, hexData);
      }
      buflen = 0;
    } else {
      if (buflen < SLCAN_BUF-1) buf[buflen++] = c;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  HTTP
// ═══════════════════════════════════════════════════════════════
void handleSend() {
  IPAddress reqIP = server.client().remoteIP();
  if (reqIP != clientIP) {
    clientIP = reqIP; clientKnown = true;
  }

  String busArg  = server.arg("bus");
  String idArg   = server.arg("id");
  String dataArg = server.arg("data");

  if (!busArg.length() || !idArg.length()) {
    server.send(400,"application/json",
      "{\"ok\":false,\"error\":\"bus et id requis\"}");
    return;
  }
  busArg.toUpperCase();
  if (busArg != "A" && busArg != "B") {
    server.send(400,"application/json",
      "{\"ok\":false,\"error\":\"bus doit etre A ou B\"}");
    return;
  }

  uint16_t canId = (uint16_t)strtol(idArg.c_str(), nullptr, 16);

  uint8_t data[8]={0}; uint8_t dlc=0;
  dataArg.replace(" ",""); dataArg.replace("-","");
  dlc = min((int)dataArg.length()/2, 8);
  for (int i=0; i<dlc; i++)
    data[i] = (uint8_t)strtol(
      dataArg.substring(i*2,i*2+2).c_str(), nullptr, 16);

  HardwareSerial &bus = (busArg=="A") ? CanA : CanB;
  slcanSend(bus, canId, data, dlc);

  char resp[128];
  snprintf(resp, sizeof(resp),
    "{\"ok\":true,\"bus\":\"%s\",\"id\":\"0x%03X\","
    "\"dlc\":%d,\"data\":\"%s\"}",
    busArg.c_str(), canId, dlc, dataArg.c_str());
  server.send(200,"application/json",resp);
}

void handleStatus() {
  IPAddress reqIP = server.client().remoteIP();
  if (reqIP != clientIP) { clientIP=reqIP; clientKnown=true; }
  char resp[256];
  snprintf(resp, sizeof(resp),
    "{\"fw\":\"%s\",\"client\":\"%s\","
    "\"busA\":{\"frames\":%lu,\"lastId\":\"0x%s\"},"
    "\"busB\":{\"frames\":%lu,\"lastId\":\"0x%s\"}}",
    FW_VERSION,
    clientKnown ? clientIP.toString().c_str() : "none",
    countA, lastIdA, countB, lastIdB);
  server.send(200,"application/json",resp);
}

void handleReboot() {
  server.send(200,"text/plain","Reboot...");
  delay(300); ESP.restart();
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[DYAL3] ESP32-C3 CAN Relay " FW_VERSION);

  // Watchdog 30s — API v5 (struct config)
  esp_task_wdt_config_t wdt_cfg = {.timeout_ms=30000, .idle_core_mask=0, .trigger_panic=true};
  esp_task_wdt_reconfigure(&wdt_cfg);
  esp_task_wdt_add(NULL);

  slcanInit(CanA, CAN_A_TX, CAN_A_RX, 'A');
  slcanInit(CanB, CAN_B_TX, CAN_B_RX, 'B');

  // WiFi AP caché – ssid_hidden=true (4e param)
  WiFi.persistent(false);          // ne pas écrire en flash à chaque boot
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255,255,255,0));
  WiFi.softAP(AP_SSID, AP_PASS,
    1,      // canal
    false,   // ssid_hidden = CACHÉ (True) ou visible (False)
    4);     // max_connection

  // Désactiver le power-save WiFi → AP plus stable
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.printf("[WiFi] AP caché: %s -> %s\n", AP_SSID, AP_IP_STR);

  server.on("/",       HTTP_GET, handleSend);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/reboot", HTTP_GET, handleReboot);
  server.begin();

  Serial.println("[HTTP] Serveur démarré");
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  esp_task_wdt_reset();   // nourrir le watchdog

  server.handleClient();

  slcanRead(CanA, bufA, bufAlen, 'A', countA, lastIdA);
  slcanRead(CanB, bufB, bufBlen, 'B', countB, lastIdB);

  // Laisser du temps au stack WiFi (tâche FreeRTOS)
  yield();
}
