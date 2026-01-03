#include <stdint.h>
#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_MOSI 4
#define OLED_CLK  2
#define OLED_DC   1
#define OLED_CS   5
#define OLED_RST  0
#define MASTER_PAIR_PIN 10 

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RST, OLED_CS);

// --- Spiel-Konfiguration ---
static const int TOP_UI_H = 12;
static const int FIELD_Y0 = TOP_UI_H + 1;
static const int PADDLE_W = 3;
static const int PADDLE_H = 14;
static const int PADDLE_XL = 4;
static const int PADDLE_XR = SCREEN_WIDTH - 4 - PADDLE_W;
static const int BALL_R = 2;
static const int SCORE_TO_WIN = 10;

enum GameMode { MODE_IDLE, MODE_COUNTDOWN, MODE_RUNNING, MODE_GAMEOVER };

struct Particle { float x, y, vx, vy; bool active; };
Particle particles[20];

struct GameState {
  float paddleYL, paddleYR, ballX, ballY, ballVX, ballVY;
  int scoreL, scoreR;
  uint32_t lastFrameMs, countdownStartMs;
  int countdownValue;
  GameMode mode;
};
GameState gs;

#pragma pack(push, 1)
struct CtrlPacket { uint32_t seq; uint16_t p1, p2; uint8_t buttons, crc8; };
#pragma pack(pop)

volatile uint16_t g_rxP1 = 2048, g_rxP2 = 2048;
volatile uint8_t g_rxButtons = 0;

uint8_t crc8_simple(const uint8_t* d, size_t n) {
  uint8_t c = 0; for (size_t i = 0; i < n; i++) c ^= d[i]; return c;
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len == sizeof(CtrlPacket)) {
    CtrlPacket pkt; memcpy(&pkt, data, sizeof(pkt));
    if (crc8_simple((const uint8_t*)&pkt, sizeof(pkt) - 1) == pkt.crc8) {
      g_rxP1 = pkt.p1; g_rxP2 = pkt.p2; g_rxButtons = pkt.buttons;
    }
  }
}

void initParticles(bool leftSide) {
  for(int i=0; i<20; i++) {
    particles[i].active = true;
    particles[i].x = leftSide ? 30 : 90; // Startpunkt Gewinnerseite
    particles[i].y = 35;
    
    // DEUTLICH LANGSAMER: vx zwischen -1.0 und +1.0, vy nach oben (-1.5 bis -0.5)
    particles[i].vx = (random(-10, 10) / 10.0f); 
    particles[i].vy = (random(-15, -5) / 10.0f);
  }
}

void updateParticles() {
  for(int i=0; i<20; i++) {
    if(!particles[i].active) continue;
    
    // Bewegung anwenden
    particles[i].x += particles[i].vx;
    particles[i].y += particles[i].vy;
    
    // SCHWERKRAFT: Erhöht (0.05f sorgt für sanfteres Fallen)
    particles[i].vy += 0.05f; 
    
    // Deaktivieren, wenn aus dem Bild gefallen
    if(particles[i].y > 64 || particles[i].x < 0 || particles[i].x > 128) {
      particles[i].active = false;
    }
    
    // Partikel zeichnen
    display.drawPixel((int)particles[i].x, (int)particles[i].y, SSD1306_WHITE);
  }
}

void drawUI() {
  display.drawFastHLine(0, TOP_UI_H, SCREEN_WIDTH, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 2);  display.print(gs.scoreL);
  display.setCursor(110, 2); display.print(gs.scoreR);
  for (int y = FIELD_Y0; y < SCREEN_HEIGHT; y += 6) 
    display.drawFastVLine(SCREEN_WIDTH/2, y, 3, SSD1306_WHITE);
}

void resetBall(bool toRight) {
  gs.ballX = SCREEN_WIDTH / 2.0f;
  gs.ballY = FIELD_Y0 + (SCREEN_HEIGHT - FIELD_Y0) / 2.0f;
  gs.ballVX = toRight ? 1.4f : -1.4f;
  gs.ballVY = (random(-12, 12) / 10.0f);
}

void setup() {
  Serial.begin(115200);
  pinMode(MASTER_PAIR_PIN, INPUT_PULLUP);
  SPI.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);
  display.begin(SSD1306_SWITCHCAPVCC);
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();

  WiFi.mode(WIFI_STA);
  uint8_t mac[6];
  WiFi.macAddress(mac); // Eigene MAC-Adresse holen [cite: 300]

// PAIRING MODE AKTIV?
  if (digitalRead(MASTER_PAIR_PIN) == LOW) {
    esp_now_init();
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_peer_info_t p = {}; 
    memcpy(p.peer_addr, bcast, 6);
    esp_now_add_peer(&p);
    
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    for(int i = 0; i < 40; i++) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(20, 5);
      display.print("--- PAIRING ---");
      
      // MAC-Adresse anzeigen
      display.setCursor(10, 20);
      display.print("MY MAC:");
      display.setCursor(10, 30);
      display.print(macStr);
      
      // Sende-Animation (Blinkendes Icon)
      if (i % 2 == 0) {
        display.fillTriangle(110, 10, 120, 15, 110, 20, SSD1306_WHITE);
      }
      
      // Fortschrittsbalken
      display.drawRect(10, 45, 108, 8, SSD1306_WHITE);
      display.fillRect(10, 45, (i * 108) / 40, 8, SSD1306_WHITE);
      
      display.setCursor(15, 55);
      display.print("SENDING BEACON...");
      display.display();

      const char* msg = "PONG_MASTER";
      esp_now_send(bcast, (uint8_t*)msg, strlen(msg)); // Beacon senden 
      delay(200);
    }
    ESP.restart();
    }
  esp_now_init();
  esp_now_register_recv_cb(onDataRecv);
  gs.mode = MODE_IDLE;
}

void loop() {
  uint32_t now = millis();
  int targetYL = map(g_rxP1, 0, 4095, FIELD_Y0, SCREEN_HEIGHT - PADDLE_H);
  int targetYR = map(g_rxP2, 0, 4095, FIELD_Y0, SCREEN_HEIGHT - PADDLE_H);

  display.clearDisplay();
  drawUI();
  display.fillRect(PADDLE_XL, targetYL, PADDLE_W, PADDLE_H, SSD1306_WHITE);
  display.fillRect(PADDLE_XR, targetYR, PADDLE_W, PADDLE_H, SSD1306_WHITE);

  if (gs.mode == MODE_IDLE) {
    display.setCursor(35, 30); display.print("PONG READY");
    if ((now / 500) % 2) { display.setCursor(30, 45); display.print("PRESS START"); }
    if (g_rxButtons & 0x01) { gs.mode = MODE_COUNTDOWN; gs.countdownStartMs = now; gs.scoreL = 0; gs.scoreR = 0; resetBall(true); }
  } 
  else if (gs.mode == MODE_GAMEOVER) {
    if (now - gs.lastFrameMs >= 30) { // Animation mit ca. 30 FPS laufen lassen
      gs.lastFrameMs = now;
      updateParticles();
    }
    display.setTextSize(2);
    display.setCursor(gs.scoreL >= SCORE_TO_WIN ? 10 : 75, 25);
    display.print("WIN!");
    display.setTextSize(1);
    if ((now / 500) % 2) { display.setCursor(30, 50); display.print("START AGAIN"); }
    if (g_rxButtons & 0x01) { gs.mode = MODE_COUNTDOWN; gs.countdownStartMs = now; gs.scoreL = 0; gs.scoreR = 0; resetBall(true); }
  }
  else if (gs.mode == MODE_COUNTDOWN) {
    int val = 3 - (int)((now - gs.countdownStartMs) / 1000);
    if (val <= 0) gs.mode = MODE_RUNNING;
    else { display.setTextSize(3); display.setCursor(55, 25); display.print(val); }
  }
  else if (gs.mode == MODE_RUNNING) {
    if (now - gs.lastFrameMs >= 16) {
      gs.lastFrameMs = now;
      gs.ballX += gs.ballVX; gs.ballY += gs.ballVY;
      if (gs.ballY <= FIELD_Y0 + BALL_R || gs.ballY >= SCREEN_HEIGHT - BALL_R) gs.ballVY *= -1;
      if (gs.ballVX < 0 && gs.ballX <= PADDLE_XL + PADDLE_W + BALL_R) {
        if (gs.ballY >= targetYL && gs.ballY <= targetYL + PADDLE_H) {
          gs.ballVX *= -1.08f; gs.ballVY = ((gs.ballY - targetYL) / (float)PADDLE_H - 0.5f) * 4.0f;
          gs.ballX = PADDLE_XL + PADDLE_W + BALL_R + 1;
        } else if (gs.ballX < 0) { 
          gs.scoreR++; resetBall(true); 
          if(gs.scoreR >= SCORE_TO_WIN) { gs.mode = MODE_GAMEOVER; initParticles(false); }
        }
      }
      if (gs.ballVX > 0 && gs.ballX >= PADDLE_XR - BALL_R) {
        if (gs.ballY >= targetYR && gs.ballY <= targetYR + PADDLE_H) {
          gs.ballVX *= -1.08f; gs.ballVY = ((gs.ballY - targetYR) / (float)PADDLE_H - 0.5f) * 4.0f;
          gs.ballX = PADDLE_XR - BALL_R - 1;
        } else if (gs.ballX > SCREEN_WIDTH) { 
          gs.scoreL++; resetBall(false); 
          if(gs.scoreL >= SCORE_TO_WIN) { gs.mode = MODE_GAMEOVER; initParticles(true); }
        }
      }
    }
    display.fillCircle((int)gs.ballX, (int)gs.ballY, BALL_R, SSD1306_WHITE);
  }
  display.display();
}