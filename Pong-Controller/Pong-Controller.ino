#include <Arduino.h>
#include <stdint.h>

#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_netif.h>

// ===================== PINOUT (Controller ESP32-C3) =====================
// ADC Pins (bitte sicherstellen, dass es echte ADC-Pins sind!)
static const int PIN_POT1  = 0;   // ADC
static const int PIN_POT2  = 1;   // ADC

// Buttons gegen GND
static const int PIN_START = 8;   // Button -> GND
static const int PIN_RESET = 9;   // Button -> GND

// ===================== ESPNOW CONFIG =====================
static const uint8_t ESPNOW_CH = 1;

// Receiver MAC (deine)
uint8_t receiverMac[6] = { 0x64, 0xE8, 0x33, 0xB6, 0x08, 0x08 };

// Debug: wenn true, sendet Broadcast statt Peer (hilfreich fürs Testen)
// static const bool USE_BROADCAST_DEBUG = true;
static const bool USE_BROADCAST_DEBUG = false;
static const uint8_t bcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Speed: so schnell wie sinnvoll (aber stabil)
static const uint32_t SEND_INTERVAL_MS = 6;    // ~160 Hz
static const uint16_t ADC_DELTA_MIN    = 6;    // nur senden wenn Änderung > Delta

// ===================== PACKET =====================
#pragma pack(push, 1)
struct CtrlPacket {
  uint32_t seq;
  uint16_t p1;
  uint16_t p2;
  uint8_t  buttons; // bit0=start, bit1=reset
  uint8_t  crc8;
};
#pragma pack(pop)

static uint8_t crc8_simple(const uint8_t* d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) c ^= d[i];
  return c;
}

static const char* en(esp_err_t e) { return esp_err_to_name(e); }

static void printMac(const uint8_t* m) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
}

static void dumpWiFiState() {
  wifi_mode_t m;
  esp_wifi_get_mode(&m);
  uint8_t ch; wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  Serial.printf("[WIFI] mode=%d ch=%u\n", (int)m, (unsigned)ch);
}

// ===================== TX FLOW CONTROL =====================
volatile bool tx_inflight = false;
volatile uint32_t tx_ok = 0;
volatile uint32_t tx_fail = 0;
volatile esp_now_send_status_t tx_last_status = ESP_NOW_SEND_FAIL;

static void onSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  (void)info;
  tx_last_status = status;
  if (status == ESP_NOW_SEND_SUCCESS) tx_ok++;
  else tx_fail++;
  tx_inflight = false;
}

// ===================== BUTTON DEBOUNCE =====================
static uint8_t readButtonsStable() {
  static uint8_t stable = 0x00;
  static uint8_t lastRaw = 0x00;
  static uint32_t lastChange = 0;

  uint8_t raw = 0;
  if (digitalRead(PIN_START) == LOW) raw |= 0x01;
  if (digitalRead(PIN_RESET) == LOW) raw |= 0x02;

  if (raw != lastRaw) {
    lastRaw = raw;
    lastChange = millis();
  }
  if (millis() - lastChange > 18) stable = lastRaw;
  return stable;
}

// ===================== WIFI INIT (HARD STA) =====================
static void wifiInitStaHard(uint8_t channel) {
  static bool inited = false;
  if (!inited) {
    esp_err_t e;
    e = esp_netif_init();
    Serial.printf("esp_netif_init() => %d (%s)\n", (int)e, en(e));
    e = esp_event_loop_create_default();
    Serial.printf("esp_event_loop_create_default() => %d (%s)\n", (int)e, en(e));
    esp_netif_create_default_wifi_sta();
    inited = true;
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t e;

  e = esp_wifi_init(&cfg);
  Serial.printf("esp_wifi_init() => %d (%s)\n", (int)e, en(e));

  e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  Serial.printf("esp_wifi_set_storage() => %d (%s)\n", (int)e, en(e));

  e = esp_wifi_set_mode(WIFI_MODE_STA);
  Serial.printf("esp_wifi_set_mode(STA) => %d (%s)\n", (int)e, en(e));

  e = esp_wifi_start();
  Serial.printf("esp_wifi_start() => %d (%s)\n", (int)e, en(e));

  // PS aus = bessere Latenz / Stabilität
  e = esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("esp_wifi_set_ps(NONE) => %d (%s)\n", (int)e, en(e));

  // Channel setzen (manchmal stabiler mit promisc toggle)
  esp_wifi_set_promiscuous(true);
  e = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  Serial.printf("esp_wifi_set_channel(%u) => %d (%s)\n", (unsigned)channel, (int)e, en(e));

  // warten bis channel != 0
  uint32_t t0 = millis();
  while (1) {
    uint8_t ch; wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
    if (ch != 0) break;
    if (millis() - t0 > 1500) break;
    delay(10);
  }

  dumpWiFiState();

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.print("Sender STA MAC: ");
  printMac(mac);
  Serial.println();
}

// ===================== ESPNOW SETUP =====================
static void setupEspNowSender() {
  Serial.println("=== SENDER INIT ===");
  wifiInitStaHard(ESPNOW_CH);

  esp_err_t e = esp_now_init();
  Serial.printf("esp_now_init() => %d (%s)\n", (int)e, en(e));
  if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
    Serial.println("ESP-NOW init failed -> stop");
    while (1) delay(100);
  }

  e = esp_now_register_send_cb(onSent);
  Serial.printf("esp_now_register_send_cb() => %d (%s)\n", (int)e, en(e));

  const uint8_t* dst = USE_BROADCAST_DEBUG ? bcastMac : receiverMac;
  Serial.print("DST MAC: ");
  printMac(dst);
  Serial.println();

  if (esp_now_is_peer_exist(dst)) {
    e = esp_now_del_peer(dst);
    Serial.printf("esp_now_del_peer() => %d (%s)\n", (int)e, en(e));
  }

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, dst, 6);
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0; // current channel

  e = esp_now_add_peer(&peer);
  Serial.printf("esp_now_add_peer() => %d (%s)\n", (int)e, en(e));

  Serial.println("=== SENDER READY ===");
}

// ===================== SEND LOGIC =====================
static uint32_t seq = 0;
static uint16_t lastP1 = 0xFFFF;
static uint16_t lastP2 = 0xFFFF;
static uint8_t  lastBtn = 0xFF;

static bool shouldSend(uint16_t p1, uint16_t p2, uint8_t btn) {
  // send if buttons changed
  if (btn != lastBtn) return true;

  // send if pots changed enough
  int d1 = (int)p1 - (int)lastP1; if (d1 < 0) d1 = -d1;
  int d2 = (int)p2 - (int)lastP2; if (d2 < 0) d2 = -d2;
  if (d1 > ADC_DELTA_MIN || d2 > ADC_DELTA_MIN) return true;

  return false;
}

static void sendPacket(uint16_t p1, uint16_t p2, uint8_t btn) {
  CtrlPacket pkt;
  pkt.seq = ++seq;
  pkt.p1 = p1;
  pkt.p2 = p2;
  pkt.buttons = btn;
  pkt.crc8 = crc8_simple((const uint8_t*)&pkt, sizeof(pkt) - 1);

  const uint8_t* dst = USE_BROADCAST_DEBUG ? bcastMac : receiverMac;

  tx_inflight = true;
  esp_err_t r = esp_now_send(dst, (uint8_t*)&pkt, sizeof(pkt));
  if (r != ESP_OK) {
    tx_inflight = false;
    Serial.printf("[SEND] rc=%d (%s)\n", (int)r, en(r));
  }

  lastP1 = p1;
  lastP2 = p2;
  lastBtn = btn;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_START, INPUT_PULLUP);
  pinMode(PIN_RESET, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // 0..3.3V besser

  setupEspNowSender();
}

void loop() {
  static uint32_t lastSendMs = 0;
  static uint32_t lastDbgMs = 0;

  uint32_t now = millis();
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;

    uint16_t p1 = analogRead(PIN_POT1);
    uint16_t p2 = analogRead(PIN_POT2);
    uint8_t btn = readButtonsStable();

    // Nur senden, wenn Queue frei
    if (!tx_inflight) {
      if (shouldSend(p1, p2, btn)) {
        sendPacket(p1, p2, btn);
      }
    }
  }

  // Debug 1x/s
  if (now - lastDbgMs > 1000) {
    lastDbgMs = now;
    Serial.printf("[SEND] ok=%lu fail=%lu inflight=%d last=%s p1=%u p2=%u btn=%u\n",
                  (unsigned long)tx_ok,
                  (unsigned long)tx_fail,
                  tx_inflight ? 1 : 0,
                  (tx_last_status == ESP_NOW_SEND_SUCCESS) ? "SUCCESS" : "FAIL",
                  (unsigned)lastP1, (unsigned)lastP2, (unsigned)lastBtn);
    dumpWiFiState();
  }

  delay(1);
}
