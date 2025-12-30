#include <stdint.h>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_netif.h>

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ====================== DISPLAY ======================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Deine SPI-Pins (ESP32-C3)
#define OLED_MOSI 4
#define OLED_CLK  2
#define OLED_DC   1
#define OLED_CS   5
#define OLED_RST  0

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RST, OLED_CS);

// ====================== PONG LAYOUT ======================
static const int TOP_UI_H   = 12;
static const int FIELD_Y0   = TOP_UI_H;

static const int PADDLE_W   = 3;
static const int PADDLE_H   = 14;
static const int PADDLE_XL  = 4;
static const int PADDLE_XR  = SCREEN_WIDTH - 4 - PADDLE_W;

static const int BALL_R     = 2;
static const int SCORE_TO_WIN = 10;

// Speed Up
static const int BALL_VY_MAX = 2;   // 1 = sehr gerade, 2 = klassisch, 3 = steiler
static const int BALL_VX_START = 1;
static const int BALL_VX_MAX   = 5;
static const int SPEEDUP_EVERY_HITS = 4; // alle 4 Paddle-Hits +1 Speed

// Buttons
static const uint8_t BTN_START = 0x01;
static const uint8_t BTN_RESET = 0x02;

// Long press start -> reset
static const uint32_t START_LONGPRESS_MS = 900;

// ====================== CONTROL PACKET (ESP-NOW) ======================
#pragma pack(push, 1)
struct CtrlPacket {
  uint32_t seq;
  uint16_t p1;
  uint16_t p2;
  uint8_t  buttons; // bit0=start, bit1=reset
  uint8_t  crc8;
};
#pragma pack(pop)

// ====================== GAME STATE ======================
enum GameMode {
  MODE_IDLE,      // wartet auf Start
  MODE_RUNNING,   // Spiel läuft
  MODE_GAMEOVER   // Gewinneranzeige + Press Start
};

struct GameState {
  int paddleYL;
  int paddleYR;

  int ballX;
  int ballY;
  int ballVX;
  int ballVY;

  int scoreL;
  int scoreR;

  int rallyHits;          // Hits seit letztem Punkt
  uint32_t lastFrameMs;

  GameMode mode;
};

GameState gs;

// ====================== RX STATE (volatile) ======================
volatile uint16_t g_rxP1 = 2048;
volatile uint16_t g_rxP2 = 2048;
volatile uint8_t  g_rxButtons = 0;
volatile uint32_t g_lastRxMs = 0;

// RX debug
volatile uint32_t dbg_rx_ok = 0;
volatile uint32_t dbg_rx_badlen = 0;
volatile uint32_t dbg_rx_badcrc = 0;
volatile uint32_t dbg_rx_other = 0;
volatile uint8_t  dbg_last_src[6] = {0};

// Button edge tracking (local)
static uint8_t lastButtons = 0;
static uint32_t startPressMs = 0;
static bool startLongHandled = false;

// UI blink
static bool blinkOn = false;
static uint32_t lastBlinkMs = 0;

// ====================== HELPERS ======================
static int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static uint8_t crc8_simple(const uint8_t* d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) c ^= d[i];
  return c;
}

static void printMac(const uint8_t* m) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
}

static const char* en(esp_err_t e) { return esp_err_to_name(e); }

static void dumpWiFiState() {
  wifi_mode_t m;
  esp_wifi_get_mode(&m);
  uint8_t ch; wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  Serial.printf("[WIFI] mode=%d ch=%u\n", (int)m, (unsigned)ch);
}

static int mapPotiToPaddleY(uint16_t adc) {
  int yMin = FIELD_Y0;
  int yMax = SCREEN_HEIGHT - PADDLE_H;
  int span = yMax - yMin;
  int y = yMin + (int)((uint32_t)adc * (uint32_t)span / 4095UL);
  return clampi(y, yMin, yMax);
}

// ====================== DRAW ======================
static void drawScoreTop() {
  display.fillRect(0, 0, SCREEN_WIDTH, TOP_UI_H, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 2);
  display.print(gs.scoreL);
  display.setCursor(SCREEN_WIDTH - 10, 2);
  display.print(gs.scoreR);
}

static void drawCenterLine() {
  for (int y = FIELD_Y0; y < SCREEN_HEIGHT; y += 6) {
    display.drawFastVLine(SCREEN_WIDTH/2, y, 3, SSD1306_WHITE);
  }
}

static void drawPaddle(int x, int y) {
  display.fillRect(x, y, PADDLE_W, PADDLE_H, SSD1306_WHITE);
}
static void erasePaddle(int x, int y) {
  display.fillRect(x, y, PADDLE_W, PADDLE_H, SSD1306_BLACK);
}

static void drawBall(int x, int y) { display.fillCircle(x, y, BALL_R, SSD1306_WHITE); }
static void eraseBall(int x, int y) { display.fillCircle(x, y, BALL_R, SSD1306_BLACK); }

static void showCenterBanner(const char* line1, const char* line2) {
  display.fillRect(8, 20, 112, 30, SSD1306_BLACK);
  display.drawRect(8, 20, 112, 30, SSD1306_WHITE);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(14, 26);
  display.print(line1);

  if (line2 && line2[0]) {
    display.setCursor(14, 38);
    display.print(line2);
  }
}

static void showPressStartBlink(bool on) {
  display.fillRect(16, 52, 96, 10, SSD1306_BLACK);
  if (on) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(22, 54);
    display.print("PRESS START");
  }
}

// ====================== GAME CORE ======================
static void resetBall(bool toRight, int vxAbs) {
  gs.ballX = SCREEN_WIDTH / 2;
  gs.ballY = FIELD_Y0 + (SCREEN_HEIGHT - FIELD_Y0) / 2;

  int vx = vxAbs;
  if (vx < 1) vx = 1;
  if (vx > BALL_VX_MAX) vx = BALL_VX_MAX;

  gs.ballVX = toRight ? vx : -vx;
  gs.ballVY = (random(0, 2) ? 1 : -1);
  gs.rallyHits = 0;
}

static void resetEverythingToIdle() {
  gs.paddleYL = FIELD_Y0 + (SCREEN_HEIGHT - FIELD_Y0)/2 - PADDLE_H/2;
  gs.paddleYR = FIELD_Y0 + (SCREEN_HEIGHT - FIELD_Y0)/2 - PADDLE_H/2;

  gs.scoreL = 0;
  gs.scoreR = 0;

  resetBall(true, BALL_VX_START);

  gs.mode = MODE_IDLE;
  gs.lastFrameMs = millis();

  display.clearDisplay();
  drawScoreTop();
  drawPaddle(PADDLE_XL, gs.paddleYL);
  drawPaddle(PADDLE_XR, gs.paddleYR);
  drawBall(gs.ballX, gs.ballY);
  drawCenterLine();
  showCenterBanner("PONG", "READY");
  showPressStartBlink(true);
  display.display();
}

static void startNewRoundFromIdleOrGameOver() {
  // Scores bleiben, wenn Idle? -> wir starten “neues Spiel” komplett:
  gs.scoreL = 0;
  gs.scoreR = 0;
  gs.mode = MODE_RUNNING;

  resetBall(random(0,2), BALL_VX_START);

  display.clearDisplay();
  drawScoreTop();
  drawPaddle(PADDLE_XL, gs.paddleYL);
  drawPaddle(PADDLE_XR, gs.paddleYR);
  drawBall(gs.ballX, gs.ballY);
  drawCenterLine();
  display.display();
}

static void gameOver(bool leftWon) {
  gs.mode = MODE_GAMEOVER;
  display.clearDisplay();
  drawScoreTop();
  drawCenterLine();
  showCenterBanner("GAME OVER", leftWon ? "WINNER LEFT" : "WINNER RIGHT");
  showPressStartBlink(true);
  display.display();

  blinkOn = true;
  lastBlinkMs = millis();
}

static void pointToLeft() {
  gs.scoreL++;
  if (gs.scoreL >= SCORE_TO_WIN) {
    gameOver(true);
  } else {
    resetBall(false, BALL_VX_START);
  }
}
static void pointToRight() {
  gs.scoreR++;
  if (gs.scoreR >= SCORE_TO_WIN) {
    gameOver(false);
  } else {
    resetBall(true, BALL_VX_START);
  }
}

static void applySpeedUpIfNeeded() {
  // alle SPEEDUP_EVERY_HITS Treffer -> Speed erhöhen
  int steps = gs.rallyHits / SPEEDUP_EVERY_HITS;
  int vxAbs = BALL_VX_START + steps;
  if (vxAbs > BALL_VX_MAX) vxAbs = BALL_VX_MAX;

  int dir = (gs.ballVX >= 0) ? 1 : -1;
  gs.ballVX = dir * vxAbs;

  // VY etwas “lebendiger” aber begrenzt
  gs.ballVY = clampi(gs.ballVY, -2, 2);
  if (gs.ballVY == 0) gs.ballVY = 1;
}

static void stepBall() {
  int oldX = gs.ballX;
  int oldY = gs.ballY;

  gs.ballX += gs.ballVX;
  gs.ballY += gs.ballVY;

  // top/bottom bounce
  if (gs.ballY - BALL_R <= FIELD_Y0) {
    gs.ballY = FIELD_Y0 + BALL_R;
    gs.ballVY = -gs.ballVY;
  }
  if (gs.ballY + BALL_R >= SCREEN_HEIGHT - 1) {
    gs.ballY = (SCREEN_HEIGHT - 1) - BALL_R;
    gs.ballVY = -gs.ballVY;
  }

  // left paddle collision or miss
  if (gs.ballVX < 0 && gs.ballX - BALL_R <= (PADDLE_XL + PADDLE_W)) {
    if (gs.ballY >= gs.paddleYL && gs.ballY <= (gs.paddleYL + PADDLE_H)) {
      gs.ballX = (PADDLE_XL + PADDLE_W) + BALL_R;
      gs.ballVX = -gs.ballVX;
      gs.rallyHits++;

      // kleine Variation
      // nur manchmal VY ändern (z.B. 1 von 4 Treffern)
      if ((random(0, 4) == 0)) {
        // nur +/-1, aber danach begrenzen
        gs.ballVY += (random(0, 2) ? 1 : -1);
      }
      gs.ballVY = clampi(gs.ballVY, -2, 2);
      if (gs.ballVY == 0) gs.ballVY = (random(0, 2) ? 1 : -1);

      applySpeedUpIfNeeded();
    } else {
      eraseBall(oldX, oldY);
      pointToRight();
      return;
    }
  }

  // right paddle collision or miss
  if (gs.ballVX > 0 && gs.ballX + BALL_R >= PADDLE_XR) {
    if (gs.ballY >= gs.paddleYR && gs.ballY <= (gs.paddleYR + PADDLE_H)) {
      gs.ballX = PADDLE_XR - BALL_R;
      gs.ballVX = -gs.ballVX;
      gs.rallyHits++;

      if (random(0, 2)) gs.ballVY += (random(0, 2) ? 1 : -1);
      gs.ballVY = clampi(gs.ballVY, -3, 3);

      applySpeedUpIfNeeded();
    } else {
      eraseBall(oldX, oldY);
      pointToLeft();
      return;
    }
  }

  // redraw ball
  eraseBall(oldX, oldY);
  drawBall(gs.ballX, gs.ballY);
}

// ====================== REMOTE INPUT ======================
static void updatePaddlesFromRemote() {
  int newYL = mapPotiToPaddleY(g_rxP1);
  int newYR = mapPotiToPaddleY(g_rxP2);

  if (newYL != gs.paddleYL) {
    erasePaddle(PADDLE_XL, gs.paddleYL);
    gs.paddleYL = newYL;
    drawPaddle(PADDLE_XL, gs.paddleYL);
  }
  if (newYR != gs.paddleYR) {
    erasePaddle(PADDLE_XR, gs.paddleYR);
    gs.paddleYR = newYR;
    drawPaddle(PADDLE_XR, gs.paddleYR);
  }
}

static void handleButtons() {
  uint8_t b = g_rxButtons;

  bool startNow = (b & BTN_START) != 0;
  bool resetNow = (b & BTN_RESET) != 0;

  bool startEdgeDown = (startNow && !(lastButtons & BTN_START));
  bool startEdgeUp   = (!startNow && (lastButtons & BTN_START));

  // Reset sofort
  if (resetNow) {
    resetEverythingToIdle();
    lastButtons = b;
    return;
  }

  // Long press tracking
  if (startEdgeDown) {
    startPressMs = millis();
    startLongHandled = false;
  }

  if (startNow && !startLongHandled) {
    if (millis() - startPressMs >= START_LONGPRESS_MS) {
      // Long press START -> Hard reset
      resetEverythingToIdle();
      startLongHandled = true;
      lastButtons = b;
      return;
    }
  }

  // Short press (release) -> Start / New Game
  if (startEdgeUp && !startLongHandled) {
    if (gs.mode == MODE_IDLE) {
      startNewRoundFromIdleOrGameOver();
    } else if (gs.mode == MODE_GAMEOVER) {
      startNewRoundFromIdleOrGameOver();
    } else if (gs.mode == MODE_RUNNING) {
      // optional: könnte Pause togglen – lassen wir bewusst weg
    }
  }

  lastButtons = b;
}

static void updateGameOverBlink() {
  if (gs.mode != MODE_GAMEOVER && gs.mode != MODE_IDLE) return;

  uint32_t now = millis();
  if (now - lastBlinkMs >= 450) {
    lastBlinkMs = now;
    blinkOn = !blinkOn;

    // nur unteren Text updaten
    showPressStartBlink(blinkOn);
    display.display();
  }
}

// ====================== ESPNOW RX CALLBACK ======================
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || !data) { dbg_rx_other++; return; }

  memcpy((void*)dbg_last_src, info->src_addr, 6);

  if (len != (int)sizeof(CtrlPacket)) {
    dbg_rx_badlen++;
    return;
  }

  CtrlPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  uint8_t c = crc8_simple((const uint8_t*)&pkt, sizeof(pkt) - 1);
  if (c != pkt.crc8) {
    dbg_rx_badcrc++;
    return;
  }

  g_rxP1 = pkt.p1;
  g_rxP2 = pkt.p2;
  g_rxButtons = pkt.buttons;
  g_lastRxMs = millis();
  dbg_rx_ok++;
}

// ====================== WIFI/ESPNOW INIT (HARD STA) ======================
static void wifiInitStaHard(uint8_t channel) {
  Serial.println("=== wifiInitStaHard ===");

  static bool netif_done = false;
  if (!netif_done) {
    esp_err_t e;
    e = esp_netif_init();
    Serial.printf("esp_netif_init() => %d (%s)\n", (int)e, en(e));
    e = esp_event_loop_create_default();
    Serial.printf("esp_event_loop_create_default() => %d (%s)\n", (int)e, en(e));
    esp_netif_create_default_wifi_sta();
    netif_done = true;
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

  e = esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("esp_wifi_set_ps(NONE) => %d (%s)\n", (int)e, en(e));

  // Channel setzen
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
  Serial.print("Receiver STA MAC (eFuse): ");
  printMac(mac);
  Serial.println();
}

static void setupEspNowReceiver() {
  Serial.println("=== RECEIVER INIT ===");
  wifiInitStaHard(1);

  esp_err_t e = esp_now_init();
  Serial.printf("esp_now_init() => %d (%s)\n", (int)e, en(e));
  if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
    Serial.println("ESP-NOW init failed!");
    while (1) delay(100);
  }

  e = esp_now_register_recv_cb(onDataRecv);
  Serial.printf("esp_now_register_recv_cb() => %d (%s)\n", (int)e, en(e));

  Serial.println("=== RECEIVER READY ===");
}

// ====================== SETUP/LOOP ======================
void setup() {
  Serial.begin(115200);
  delay(300);

  randomSeed((uint32_t)esp_random());

  setupEspNowReceiver();

  SPI.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);
  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println("SSD1306 begin failed!");
    while (1) delay(100);
  }
  display.clearDisplay();
  display.display();

  // Initial state
  gs.mode = MODE_IDLE;
  resetEverythingToIdle();
}

void loop() {
  static uint32_t lastDbg = 0;

  // Paddles immer live aktualisieren (auch im Idle/GameOver)
  updatePaddlesFromRemote();

  // Buttons (Start/Reset, Long-Press)
  handleButtons();

  // GameOver/Idle: blink "PRESS START"
  updateGameOverBlink();

  // Running: Ball + Score rendern
  uint32_t now = millis();
  if (gs.mode == MODE_RUNNING) {
    if (now - gs.lastFrameMs >= 18) {  // ~55 FPS für smooth
      gs.lastFrameMs = now;

      drawScoreTop();
      stepBall();
      drawCenterLine();
      display.display();
    }
  }

  // Debug jede Sekunde
  if (now - lastDbg > 1000) {
    lastDbg = now;
    uint32_t age = (g_lastRxMs == 0) ? 0xFFFFFFFFUL : (now - g_lastRxMs);
    Serial.printf("[RX] ok=%lu badlen=%lu badcrc=%lu other=%lu age_ms=%lu last_src=",
                  (unsigned long)dbg_rx_ok,
                  (unsigned long)dbg_rx_badlen,
                  (unsigned long)dbg_rx_badcrc,
                  (unsigned long)dbg_rx_other,
                  (unsigned long)age);
    printMac((const uint8_t*)dbg_last_src);
    Serial.printf(" p1=%u p2=%u btn=%u mode=%d score=%d:%d vx=%d vy=%d hits=%d\n",
                  (unsigned)g_rxP1, (unsigned)g_rxP2, (unsigned)g_rxButtons,
                  (int)gs.mode, gs.scoreL, gs.scoreR, gs.ballVX, gs.ballVY, gs.rallyHits);
    dumpWiFiState();
  }

  delay(1);
}
