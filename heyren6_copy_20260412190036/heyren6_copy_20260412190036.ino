/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║   HEALTH MONITOR + FALL DETECTION  —  NodeMCU ESP8266       ║
 * ║   OPTIMISED VERSION — smooth OLED, non-blocking HTTP        ║
 * ║                                                             ║
 * ║  Wiring (all share I2C):                                    ║
 * ║    MAX30102  SDA:D2  SCL:D1  VCC:3.3V  GND                  ║
 * ║    MPU6050   SDA:D2  SCL:D1  VCC:3.3V  GND                  ║
 * ║    OLED      SDA:D2  SCL:D1  VCC:3.3V  GND                  ║
 * ║    BUTTON    D3 → GND   LED  D4 → 220Ω → GND               ║
 * ║                                                             ║
 * ║  Fall detection uses raw MPU6050 register reads             ║
 * ║    Free fall  : totalAcc < 5000  (raw ADC)                  ║
 * ║    Impact     : totalAcc > 20000 (raw ADC)                  ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 *  KEY OPTIMISATIONS vs original:
 *  1. OLED frame-rate capped at 30 FPS (33 ms) — was redrawing
 *     every loop tick (~kHz), causing severe lag.
 *  2. HTTP POST timeout set to 200 ms — original could block
 *     the loop for up to 5 s on every send.
 *  3. readBPM() now calls particleSensor.check() + available()
 *     before getIR(), so it only runs when fresh FIFO data exists
 *     and always advances the FIFO pointer with nextSample().
 *  4. tickSpO2() early-exit guards prevent unnecessary work.
 *  5. String concatenation in sendDataToServer() replaced with
 *     direct print to minimise heap fragmentation on ESP8266.
 */

#ifdef DEFAULT
#undef DEFAULT
#endif
#define DEFAULT 0

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

const char* serverUrl = "http://192.168.0.105:5000/data";

// ── Pins ─────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define BUTTON_PIN     D3
#define LED_PIN        D4

// ── I2C ──────────────────────────────────────────────────────────
#define I2C_FREQ     100000UL
#define MPU_ADDR     0x68

// ── OLED frame cap ────────────────────────────────────────────────
// Limits display.display() calls to ~30 FPS.
// Anything faster wastes CPU on I2C transfers the human eye can't see.
#define OLED_FPS_MS   33UL

// ── HTTP ─────────────────────────────────────────────────────────
// Abort the POST quickly so it doesn't freeze the main loop.
#define HTTP_TIMEOUT_MS  200

// ── Fall detection (raw ADC units) ───────────────────────────────
#define FREE_FALL_THRESHOLD   5000
#define IMPACT_THRESHOLD      20000
#define FALL_WINDOW_MS        1500UL
#define CANCEL_WINDOW         10000UL
#define ALERT_RESET_MS        40000UL

// ── BPM ──────────────────────────────────────────────────────────
#define WARMUP_MS             7000UL
#define RATE_SIZE             8
#define BPM_MIN               40
#define BPM_MAX               200
#define FINGER_THRESHOLD      30000L
#define FINGER_STABILIZE_TIME 3000UL
#define PRINT_INTERVAL        1000UL

// ── SpO2 ─────────────────────────────────────────────────────────
#define SPO2_BUFFER_SIZE  50
#define SPO2_REPEAT_MS    8000UL

// ── MAX30102 LED brightness ───────────────────────────────────────
#define MAX_LED_PWR  0x1F

// ── Median filter ────────────────────────────────────────────────
#define MEDIAN_SIZE  5

// ═════════════════════════════════════════════════════════════════
// Objects
// ═════════════════════════════════════════════════════════════════
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RoboEyes<Adafruit_SSD1306> roboEyes(display);
MAX30105 particleSensor;

bool oledReady = false;
bool mpuReady  = false;
bool maxReady  = false;

// ═════════════════════════════════════════════════════════════════
// State machine
// ═════════════════════════════════════════════════════════════════
enum State { MONITORING, COUNTDOWN, ALERT_SENT };
State currentState = MONITORING;

// ═════════════════════════════════════════════════════════════════
// MPU6050 raw data
// ═════════════════════════════════════════════════════════════════
int16_t ax, ay, az;
float   totalAcc = 0;

// ═════════════════════════════════════════════════════════════════
// BPM state
// ═════════════════════════════════════════════════════════════════
byte  rates[RATE_SIZE] = {0};
byte  rateSpot         = 0;
long  lastBeat         = 0;
int   beatAvg          = 0;
float beatsPerMinute   = 0;

bool          fingerDetected   = false;
bool          fingerWasOn      = false;
bool          readyForBPM      = false;
unsigned long fingerStartTime  = 0;
unsigned long lastPrint        = 0;

float medianBuf[MEDIAN_SIZE] = {0};
int   medianIdx  = 0;
bool  medianFull = false;

// ═════════════════════════════════════════════════════════════════
// SpO2 — non-blocking, one sample per loop tick
// ═════════════════════════════════════════════════════════════════
uint32_t irBuffer[SPO2_BUFFER_SIZE];
uint32_t redBuffer[SPO2_BUFFER_SIZE];
int      spo2CollectIdx = 0;
bool     spo2Collecting = false;
unsigned long lastSpo2Calc = 0;

int32_t spo2Val        = 0;
int8_t  validSPO2      = 0;
int32_t heartRate_spo2 = 0;
int8_t  validHeartRate = 0;

// ═════════════════════════════════════════════════════════════════
// UI / timing
// ═════════════════════════════════════════════════════════════════
unsigned long fallDetectedAt     = 0;
unsigned long freeFallDetectedAt = 0;
bool          inFreeFall         = false;
unsigned long lastButtonPress    = 0;
bool          showHealthScreen   = false;
unsigned long lastOledDraw       = 0;   // ← frame-rate cap timestamp

// ═════════════════════════════════════════════════════════════════
// Helpers
// ═════════════════════════════════════════════════════════════════
void pushMedian(float v) {
  medianBuf[medianIdx++] = v;
  if (medianIdx >= MEDIAN_SIZE) { medianIdx = 0; medianFull = true; }
}

float getMedian() {
  int n = medianFull ? MEDIAN_SIZE : medianIdx;
  if (n == 0) return 0;
  float s[MEDIAN_SIZE];
  for (int i = 0; i < n; i++) s[i] = medianBuf[i];
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (s[j] > s[j + 1]) { float t = s[j]; s[j] = s[j + 1]; s[j + 1] = t; }
  return s[n / 2];
}

void resetReadings() {
  for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
  rateSpot = 0; beatAvg = 0; beatsPerMinute = 0; lastBeat = 0;
  medianIdx = 0; medianFull = false;
  spo2Val = 0; validSPO2 = 0;
  spo2CollectIdx = 0; spo2Collecting = false;
}

void oledClear() { if (oledReady) display.clearDisplay(); }
void oledShow()  { if (oledReady) display.display(); }

// ═════════════════════════════════════════════════════════════════
// MPU6050 — raw register read
// ═════════════════════════════════════════════════════════════════
void mpuWakeup() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);
}

void readMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU_ADDR, (size_t)6, true);

  ax = Wire.read() << 8 | Wire.read();
  ay = Wire.read() << 8 | Wire.read();
  az = Wire.read() << 8 | Wire.read();

  totalAcc = sqrt((float)ax * ax + (float)ay * ay + (float)az * az);
}

// ═════════════════════════════════════════════════════════════════
// I2C scanner
// ═════════════════════════════════════════════════════════════════
void scanI2C() {
  Serial.println("\n── I2C Scan ──────────────────────");
  int n = 0;
  for (byte a = 8; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print("  0x"); Serial.print(a, HEX); Serial.print("  ");
      if (a == 0x3C || a == 0x3D) Serial.print("<-- OLED SSD1306");
      if (a == 0x57)               Serial.print("<-- MAX30102");
      if (a == 0x68 || a == 0x69) Serial.print("<-- MPU6050");
      Serial.println(); n++;
    }
    delay(3);
  }
  if (!n) Serial.println("  *** NOTHING FOUND — check wiring! ***");
  Serial.println("──────────────────────────────────\n");
}

// ═════════════════════════════════════════════════════════════════
// SpO2 — non-blocking, one sample per loop tick
// ═════════════════════════════════════════════════════════════════
void tickSpO2() {
  // Early-exit guards — skip all work when conditions aren't met
  if (!maxReady || !readyForBPM || !fingerDetected) return;

  if (!spo2Collecting && millis() - lastSpo2Calc > SPO2_REPEAT_MS) {
    spo2Collecting  = true;
    spo2CollectIdx  = 0;
    Serial.println("SpO2: collecting...");
  }
  if (!spo2Collecting) return;

  particleSensor.check();
  if (!particleSensor.available()) return;   // no new FIFO data — skip tick

  redBuffer[spo2CollectIdx] = particleSensor.getRed();
  irBuffer[spo2CollectIdx]  = particleSensor.getIR();
  particleSensor.nextSample();
  spo2CollectIdx++;
  yield();

  if (spo2CollectIdx >= SPO2_BUFFER_SIZE) {
    maxim_heart_rate_and_oxygen_saturation(
      irBuffer, SPO2_BUFFER_SIZE, redBuffer,
      &spo2Val, &validSPO2, &heartRate_spo2, &validHeartRate
    );
    spo2Collecting = false;
    lastSpo2Calc   = millis();
    Serial.print("SpO2: ");
    Serial.println((validSPO2 && spo2Val > 70 && spo2Val <= 100)
      ? String(spo2Val) + "%" : "invalid (keep finger still)");
  }
}

// ═════════════════════════════════════════════════════════════════
// BPM reader — OPTIMISED
//   • Calls particleSensor.check() before getIR() so fresh data
//     is always fetched from the FIFO.
//   • Returns immediately if no new sample is available, keeping
//     the loop fast.
//   • Always calls nextSample() to advance the FIFO pointer and
//     prevent the buffer from filling up.
// ═════════════════════════════════════════════════════════════════
void readBPM() {
  if (!maxReady) return;

  particleSensor.check();                    // fetch FIFO
  if (!particleSensor.available()) return;   // nothing new — skip

  long irValue = particleSensor.getIR();
  particleSensor.nextSample();               // advance FIFO pointer

  fingerDetected = (irValue >= FINGER_THRESHOLD);

  if (!fingerDetected) {
    if (fingerWasOn) {
      resetReadings();
      fingerWasOn = false;
      Serial.println("Finger removed.");
    }
    return;
  }

  if (!fingerWasOn) {
    fingerWasOn     = true;
    lastBeat        = millis();
    fingerStartTime = millis();
    readyForBPM     = false;
    Serial.println("Finger on — stabilising 3s...");
  }

  if (!readyForBPM) {
    if (millis() - fingerStartTime >= FINGER_STABILIZE_TIME) {
      readyForBPM  = true;
      lastSpo2Calc = 0;
      Serial.println("Reading BPM + SpO2...");
    }
    return;
  }

  if (checkForBeat(irValue)) {
    long  delta = millis() - lastBeat;
    lastBeat    = millis();
    float bpm   = 60000.0f / (float)delta;
    if (bpm >= BPM_MIN && bpm <= BPM_MAX) {
      pushMedian(bpm);
      float f = getMedian();
      rates[rateSpot++] = (byte)f;
      rateSpot %= RATE_SIZE;
      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
      beatsPerMinute = f;
    }
  }

  if (millis() - lastPrint > PRINT_INTERVAL) {
    lastPrint = millis();
    Serial.print("IR:"); Serial.print(irValue);
    Serial.print("  BPM:"); Serial.print(beatAvg > 0 ? String(beatAvg) : "--");
    Serial.print("  SpO2:");
    Serial.println((validSPO2 && spo2Val > 70) ? String(spo2Val) + "%" : "--");
  }
}

// ═════════════════════════════════════════════════════════════════
// OLED health screen
// ═════════════════════════════════════════════════════════════════
void showHealthOLED() {
  oledClear();
  display.setTextColor(SSD1306_WHITE);

  if (!fingerDetected) {
    display.setTextSize(1);
    display.setCursor(10, 16); display.print("Place finger on");
    display.setCursor(22, 30); display.print("the sensor");
    display.setCursor(0,  50); display.print("Button: toggle screen");
    oledShow();
    return;
  }

  // BPM (left side)
  display.setTextSize(1); display.setCursor(0, 0);  display.print("Heart Rate");
  display.setTextSize(3); display.setCursor(0, 12);
  display.print((!readyForBPM || beatAvg == 0) ? "--" : String(beatAvg));
  display.setTextSize(1); display.setCursor(0, 40); display.print("BPM");

  // SpO2 (right side)
  display.setTextSize(1); display.setCursor(72, 0);  display.print("SpO2");
  display.setTextSize(3); display.setCursor(72, 12);
  display.print((validSPO2 && spo2Val > 70 && spo2Val <= 100) ? String(spo2Val) : "--");
  display.setTextSize(1); display.setCursor(72, 40); display.print("%");

  if (spo2Collecting) {
    display.setCursor(0, 54);
    display.print("SpO2 measuring...");
  }
  oledShow();
}

// ═════════════════════════════════════════════════════════════════
// RoboEyes helpers
// ═════════════════════════════════════════════════════════════════
void setFaceMood() {
  if (!fingerDetected || beatAvg == 0) roboEyes.setMood(DEFAULT);
  else if (beatAvg > 100)              roboEyes.setMood(ANGRY);
  else if (beatAvg < 55)               roboEyes.setMood(TIRED);
  else                                 roboEyes.setMood(HAPPY);
}

void resetEyes() {
  roboEyes.setHFlicker(OFF, 0); roboEyes.setVFlicker(OFF, 0);
  roboEyes.setHeight(40, 40);   roboEyes.setMood(DEFAULT);
  roboEyes.setAutoblinker(ON, 3, 1); roboEyes.setIdleMode(ON, 4, 2);
}

// ═════════════════════════════════════════════════════════════════
// HTTP POST — OPTIMISED
//   • Short timeout (200 ms) prevents the POST from blocking the
//     main loop for seconds when the server is slow or unreachable.
//   • Uses direct print() calls instead of String concatenation to
//     reduce heap fragmentation on the ESP8266.
// ═════════════════════════════════════════════════════════════════
void sendDataToServer(int bpm, int spo2, bool fall) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  client.setTimeout(HTTP_TIMEOUT_MS);

  HTTPClient http;
  http.begin(client, serverUrl);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  // Build JSON without String heap churn
  char json[64];
  snprintf(json, sizeof(json),
    "{\"bpm\":%d,\"spo2\":%d,\"fall\":%s}",
    bpm, spo2, fall ? "true" : "false");

  int code = http.POST(json);
  Serial.println("----- SENDING DATA -----");
  Serial.println(json);
  Serial.print("Response: "); Serial.println(code);

  http.end();
  client.stop();
}

// ═════════════════════════════════════════════════════════════════
// Setup
// ═════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);
  delay(500);
  Serial.println("\n====== BOOT ======");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(D2, D1);
  Wire.setClock(I2C_FREQ);
  delay(200);

  scanI2C();

  // ── OLED ─────────────────────────────────────────────────────
  Serial.print("OLED... ");
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledReady = true;
    Serial.println("OK");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(20, 28);
    display.print("Starting up...");
    display.display();
  } else {
    Serial.println("FAILED");
  }
  delay(300);

  // ── RoboEyes ─────────────────────────────────────────────────
  if (oledReady) {
    roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
    roboEyes.setWidth(36, 36); roboEyes.setHeight(40, 40);
    roboEyes.setBorderradius(10, 10); roboEyes.setSpacebetween(12);
    roboEyes.setAutoblinker(ON, 3, 1); roboEyes.setIdleMode(ON, 4, 2);
    roboEyes.setMood(DEFAULT); roboEyes.anim_laugh();
    delay(1500);
  }

  // ── MPU6050 ──────────────────────────────────────────────────
  Serial.print("MPU6050... ");
  delay(50);
  mpuWakeup();
  Wire.beginTransmission(MPU_ADDR);
  if (Wire.endTransmission() == 0) {
    mpuReady = true;
    Serial.println("OK");
    Serial.println("  Fall: free-fall <5000 raw, impact >20000 raw");
  } else {
    Serial.println("FAILED — check wiring");
  }
  delay(100);

  // ── MAX30102 ─────────────────────────────────────────────────
  Serial.print("MAX30102... ");
  delay(50);
  if (particleSensor.begin(Wire, I2C_FREQ)) {
    particleSensor.setup(MAX_LED_PWR, 4, 2, 100, 411, 4096);
    particleSensor.setPulseAmplitudeRed(MAX_LED_PWR);
    particleSensor.setPulseAmplitudeIR(MAX_LED_PWR);
    particleSensor.setPulseAmplitudeGreen(0);
    maxReady = true;
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }

  // ── Warmup countdown ─────────────────────────────────────────
  Serial.println("Warming up...");
  for (int sec = (int)(WARMUP_MS / 1000); sec >= 1; sec--) {
    if (maxReady) {
      for (int i = 0; i < 50; i++) { particleSensor.getIR(); yield(); }
    }
    if (oledReady) {
      display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1); display.setCursor(22,  4); display.print("Warming up...");
      display.setTextSize(4); display.setCursor(48, 20); display.print(sec);
      display.setTextSize(1); display.setCursor(10, 54); display.print("Ready your finger");
      display.display();
    }
    delay(1000);
  }
  Serial.println("Ready!");

  // ── WiFi ─────────────────────────────────────────────────────
  Serial.println("Connecting to WiFi...");
  WiFi.begin("Ren", "21082108");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());
  Serial.println("==================================\n");
}

// ═════════════════════════════════════════════════════════════════
// Loop
// ═════════════════════════════════════════════════════════════════
void loop() {

  // ── Sensors (run every tick — fast, non-blocking) ────────────
  readBPM();
  tickSpO2();

  // ── MPU6050 raw read ─────────────────────────────────────────
  if (mpuReady) {
    readMPU();

    static unsigned long lastAccelPrint = 0;
    if (millis() - lastAccelPrint > 2000) {
      lastAccelPrint = millis();
      Serial.print("Accel raw: "); Serial.print(totalAcc, 0);
      if (totalAcc < FREE_FALL_THRESHOLD) Serial.print("  << FREE-FALL");
      if (totalAcc > IMPACT_THRESHOLD)    Serial.print("  << IMPACT");
      if (inFreeFall)                     Serial.print("  [waiting for impact]");
      Serial.println();
    }
  }

  // ════════════════════════════════════════════════════════════
  // State machine
  // ════════════════════════════════════════════════════════════
  switch (currentState) {

    // ── MONITORING ───────────────────────────────────────────
    case MONITORING: {

      // Button toggles health / robo-eyes screen
      if (digitalRead(BUTTON_PIN) == LOW && millis() - lastButtonPress > 500) {
        showHealthScreen = !showHealthScreen;
        lastButtonPress  = millis();
        oledClear();
      }

      // ── OLED draw — capped at OLED_FPS_MS (33 ms = ~30 FPS) ──
      // Previously drawn every loop tick which saturated the I2C
      // bus and was the primary cause of visible lag.
      if (millis() - lastOledDraw >= OLED_FPS_MS) {
        lastOledDraw = millis();
        if (showHealthScreen) {
          showHealthOLED();
        } else if (oledReady) {
          setFaceMood();
          roboEyes.update();
        }
      }

      // ── Fall detection ────────────────────────────────────
      if (mpuReady) {
        // Phase 1: free-fall
        if (totalAcc < FREE_FALL_THRESHOLD) {
          if (!inFreeFall) {
            inFreeFall         = true;
            freeFallDetectedAt = millis();
            Serial.print("Free-fall phase  raw="); Serial.println(totalAcc, 0);
          }
        }

        // Free-fall window expired without impact — reset
        if (inFreeFall && millis() - freeFallDetectedAt > FALL_WINDOW_MS) {
          inFreeFall = false;
        }

        // Phase 2: impact after free-fall → confirmed fall
        if (inFreeFall && totalAcc > IMPACT_THRESHOLD) {
          inFreeFall       = false;
          currentState     = COUNTDOWN;
          showHealthScreen = false;
          fallDetectedAt   = millis();
          Serial.print("FALL CONFIRMED — impact raw="); Serial.println(totalAcc, 0);
          if (oledReady) {
            roboEyes.setIdleMode(OFF, 0, 0);
            roboEyes.setAutoblinker(OFF, 0, 0);
            roboEyes.setHeight(48, 48);
            roboEyes.anim_confused();
          }
        }
      }
      break;
    }

    // ── COUNTDOWN ────────────────────────────────────────────
    case COUNTDOWN: {
      unsigned long elapsed = millis() - fallDetectedAt;
      int secsLeft = max(0, 10 - (int)(elapsed / 1000));

      // Button cancels alert
      if (digitalRead(BUTTON_PIN) == LOW && millis() - lastButtonPress > 500) {
        currentState     = MONITORING;
        lastButtonPress  = millis();
        digitalWrite(LED_PIN, LOW);
        if (oledReady) resetEyes();
        Serial.println("Fall alert cancelled.");
        delay(300);
        break;
      }

      // OLED — frame capped
      if (millis() - lastOledDraw >= OLED_FPS_MS) {
        lastOledDraw = millis();
        if (oledReady) {
          roboEyes.setMood(ANGRY);
          roboEyes.setPosition(N);
          roboEyes.setHFlicker(ON, 2);
          roboEyes.update();
          display.setTextSize(1);
          display.setTextColor(SSD1306_WHITE);
          display.setCursor(0, 54);
          display.print("Fall! Cancel: ");
          display.print(secsLeft);
          display.print("s  ");
          display.display();
        }
      }

      // LED blink
      digitalWrite(LED_PIN, (millis() / 200) % 2);

      // Timeout → send alert
      if (elapsed >= CANCEL_WINDOW) {
        currentState = ALERT_SENT;
        digitalWrite(LED_PIN, HIGH);
        if (oledReady) {
          roboEyes.setHFlicker(OFF, 0);
          roboEyes.setMood(TIRED);
          roboEyes.setAutoblinker(OFF, 0, 0);
          roboEyes.setIdleMode(OFF, 0, 0);
        }
        Serial.println("ALERT SENT!");
      }
      break;
    }

    // ── ALERT_SENT ───────────────────────────────────────────
    case ALERT_SENT: {
      // OLED — frame capped (no need to spam I2C with identical frames)
      if (millis() - lastOledDraw >= OLED_FPS_MS) {
        lastOledDraw = millis();
        if (oledReady) {
          display.clearDisplay();
          display.setTextColor(SSD1306_WHITE);
          display.setTextSize(2); display.setCursor(24,  8); display.print("FALL!");
          display.setTextSize(1); display.setCursor(10, 36); display.print("Sending alert...");
          display.setCursor(10, 50);                         display.print("HELP: 91025406");
          display.display();
        }
      }

      digitalWrite(LED_PIN, HIGH);

      if (millis() - fallDetectedAt > ALERT_RESET_MS) {
        currentState = MONITORING;
        digitalWrite(LED_PIN, LOW);
        if (oledReady) resetEyes();
        Serial.println("Alert reset. Back to monitoring.");
      }
      break;
    }
  }

  // ── Periodic HTTP send (every 3 s) ───────────────────────────
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 3000) {
    lastSend = millis();
    int safeBPM  = (beatAvg >= 50  && beatAvg  <= 160) ? beatAvg  : 0;
    int safeSpO2 = (spo2Val >= 80  && spo2Val  <= 100) ? spo2Val  : 0;
    sendDataToServer(safeBPM, safeSpO2, currentState != MONITORING);
  }

  yield();
}
