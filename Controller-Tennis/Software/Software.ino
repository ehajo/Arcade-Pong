#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Preferences.h>

static const int PIN_POT1 = 0, PIN_POT2 = 1, PIN_START = 8;

Preferences prefs;
uint8_t masterMac[6];
volatile bool pairingDone = false;

#pragma pack(push, 1)
struct CtrlPacket {
  uint32_t seq;
  uint16_t p1, p2;
  uint8_t  buttons, crc8;
};
#pragma pack(pop)

uint8_t crc8_simple(const uint8_t* d, size_t n) {
  uint8_t c = 0; for (size_t i = 0; i < n; i++) c ^= d[i]; return c;
}

void onPairingRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len >= 11 && strncmp((char*)data, "PONG_MASTER", 11) == 0) {
    memcpy(masterMac, info->src_addr, 6);
    pairingDone = true;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_START, INPUT_PULLUP);
  
  prefs.begin("pong", false);
  WiFi.mode(WIFI_STA);
  esp_now_init();

  if (digitalRead(PIN_START) == LOW) {
    Serial.println("[PAIRING] Aktiv...");
    esp_now_register_recv_cb(onPairingRecv);
    uint32_t startWait = millis();
    while(!pairingDone && (millis() - startWait < 15000)) { delay(100); Serial.print("."); }
    if(pairingDone) {
      prefs.putBytes("mac", masterMac, 6);
      Serial.println("\n[ERFOLG]");
      delay(1000);
      ESP.restart();
    }
  }

  if (prefs.getBytes("mac", masterMac, 6) == 0) memset(masterMac, 0xFF, 6);

  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, masterMac, 6);
  esp_now_add_peer(&p);
}

void loop() {
  static uint32_t lastSend = 0;
  if (millis() - lastSend >= 10) {
    lastSend = millis();
    CtrlPacket pkt;
    pkt.seq = lastSend;
    pkt.p1 = analogRead(PIN_POT1);
    pkt.p2 = analogRead(PIN_POT2);
    pkt.buttons = (digitalRead(PIN_START) == LOW) ? 0x01 : 0x00;
    pkt.crc8 = crc8_simple((uint8_t*)&pkt, sizeof(pkt)-1);
    esp_now_send(masterMac, (uint8_t*)&pkt, sizeof(pkt));
  }
}