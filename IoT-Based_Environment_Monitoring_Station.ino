// ============================================================
//  IoT-BASED ENVIRONMENT MONITORING STATION
//  Board  : ESP32 (Xtensa LX6 dual-core 240 MHz)
//  Display: SH1106 128×64 OLED (I2C, SDA=21, SCL=22)
//  Sensors: DHT22 · BMP180 · BH1750 · MQ135 · Rain sensor
//
//  TYPOGRAPHY SYSTEM (consistent across all pages):
//    Plain_10  →  Page header, section labels, status words, units
//    Plain_16  →  Primary sensor value (temp, hum, rain%, ppm)
//    Plain_24  →  (reserved — not used, screen too small)
//
//  BAR RANGES:
//    Temperature : 0 – 50 °C
//    Humidity    : 0 – 100 %
//    PPM (air)   : 0 – 500 ppm
//    Rain        : 0 – 100 %
// ============================================================

// ── Credentials ─────────────────────────────────────────────
// WiFi + Blynk credentials live in secrets.h, which is NOT committed
// to git (see .gitignore). Copy secrets.h.example to secrets.h and
// fill in your own values before flashing.
#include "secrets.h"

#define BLYNK_TEMPLATE_ID   SECRET_BLYNK_TEMPLATE_ID
#define BLYNK_TEMPLATE_NAME SECRET_BLYNK_TEMPLATE_NAME
#define BLYNK_AUTH_TOKEN    SECRET_BLYNK_AUTH_TOKEN

#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <BH1750.h>
#include <DHT.h>
#include <SH1106Wire.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <math.h>

// ============================================================
//  MQ135 CALIBRATION
// ============================================================
//
//  HOW PPM IS CALCULATED:
//  The MQ135 changes resistance (Rs) based on gas concentration.
//  Ro = Rs measured in clean outdoor air = your sensor's baseline.
//  Formula: PPM = 116.6 x (Rs/Ro)^(-2.769)  [CO2-equivalent]
//
//  RIGHT NOW (burn-in < 24h):
//    Leave MQ135_USE_OVERRIDE = false.
//    On every boot, code samples 30 seconds and estimates Ro.
//    Values are approximate but functional.
//
//  AFTER FULL 24-48 HR BURN-IN — how to get your real Ro:
//  1. Take device outdoors or near an open window.
//  2. Power it on. Wait for the 30s warmup to finish.
//  3. Open Arduino IDE → Serial Monitor → 115200 baud.
//  4. Look for this exact line:
//       >> MQ135 Ro = XX.XX kO
//  5. Write down that number.
//  6. Set MQ135_USE_OVERRIDE  →  true
//  7. Set MQ135_RO_OVERRIDE   →  your number
//  8. Reflash. The 30s boot warmup disappears permanently.
//     Serial will print: "MQ135: using fixed Ro = XX.XX kO"
//
//  Typical clean-air Ro for MQ135: 3.0 – 10.0 kΩ
//  Datasheet reference: ~10.0 kΩ (used as default if no cal)

#define MQ135_USE_OVERRIDE   true
#define MQ135_RO_OVERRIDE    30.12f   // kΩ — burned-in calibration value

#define MQ135_A              116.6020682f   // CO2-equivalent curve constants
#define MQ135_B             -2.769034857f   // from MQ135 datasheet
#define MQ135_CLEAN_RATIO    3.6f           // Rs/Ro in clean outdoor air
#define MQ135_RL             10.0f          // Load resistor on your board (kΩ)
#define MQ135_VCC            3.3f           // ESP32 ADC reference voltage

// ============================================================
//  PRESSURE TREND — 10-minute rolling window
//  One pressure reading stored per minute.
//  Trend = newest reading − oldest reading in window.
//  Eliminates the 2-second noise that caused false storm alerts.
// ============================================================
#define PRESSURE_SAMPLES     10
#define PRESSURE_LOG_MS      60000UL

float         pressureHistory[PRESSURE_SAMPLES];
int           pressureIdx    = 0;
bool          pressureFull   = false;
unsigned long lastPressureLog = 0;

// ============================================================
//  PINS
// ============================================================
#define DHT_PIN    4
#define DHT_TYPE   DHT22
#define RAIN_AO    34
#define RAIN_DO    35
#define MQ135_AO   32
#define MQ135_DO   33

// ============================================================
//  OBJECTS
// ============================================================
// (no WiFiMulti object needed — single network uses WiFi.begin() directly)
DHT             dht(DHT_PIN, DHT_TYPE);
Adafruit_BMP085 bmp;
BH1750          lightMeter;
SH1106Wire      display(0x3C, 21, 22);

// ============================================================
//  TIMING
// ============================================================
unsigned long lastSwitch = 0;
unsigned long lastRead   = 0;
unsigned long lastBlynk  = 0;
int page = 0;

// ============================================================
//  SENSOR STATE
// ============================================================
float  temp              = 0;
float  humidity          = 0;
float  pressure          = 0;
float  lux               = 0;
float  ppm               = 0;
float  mq135Ro           = 10.0f;
float  pressureTrend     = 0;
int    rainPercent       = 0;
String rainLevel         = "Dry";
String airQuality        = "Good";
String weatherPrediction = "Stable";
String overallStatus     = "Reading...";

// ============================================================
//  STABLE ADC — 10-sample average
//  Smooths ESP32 ADC noise on pins 32, 34
// ============================================================
int readStableADC(int pin) {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(pin); delay(3); }
  return (int)(sum / 10);
}

// ============================================================
//  MQ135 — ADC value → sensor resistance Rs (kΩ)
//  Circuit assumes: VCC → [sensor] → Vout → [RL 10kΩ] → GND
// ============================================================
float mq135_Rs(int adcVal) {
  float vout = adcVal * (MQ135_VCC / 4095.0f);
  vout = max(vout, 0.01f);
  return MQ135_RL * (MQ135_VCC - vout) / vout;
}

// ============================================================
//  MQ135 BOOT CALIBRATION
//  30-second sampling in assumed-clean air → computes Ro.
//  Skipped entirely when MQ135_USE_OVERRIDE = true.
// ============================================================
void calibrateMQ135() {
  if (MQ135_USE_OVERRIDE) {
    mq135Ro = MQ135_RO_OVERRIDE;
    Serial.printf("MQ135: using fixed Ro = %.2f kO\n", mq135Ro);
    return;
  }

  float rsSum = 0;
  int   count = 0;
  const unsigned long DUR = 30000UL;
  unsigned long start = millis();

  while (millis() - start < DUR) {
    rsSum += mq135_Rs(readStableADC(MQ135_AO));
    count++;
    int pct = (int)((millis() - start) * 100UL / DUR);

    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 4,  "MQ135 WARMUP");
    display.drawString(64, 18, "Keep in clean air");
    display.drawRect(4, 34, 120, 8);
    int fill = map(pct, 0, 100, 0, 118);
    if (fill > 2) display.fillRect(5, 35, fill, 6);
    display.drawString(64, 46, String(pct) + "%");
    display.display();
    delay(800);
  }

  if (count > 0) {
    mq135Ro = constrain((rsSum / count) / MQ135_CLEAN_RATIO, 0.5f, 50.0f);
  }

  Serial.printf(">> MQ135 Ro = %.2f kO  (%d samples)\n", mq135Ro, count);
  Serial.println("   After 24h burn-in: copy this value to MQ135_RO_OVERRIDE");

  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 12, "Calibrated!");
  display.drawString(64, 28, "Ro = " + String(mq135Ro, 2) + " kO");
  display.drawString(64, 44, "Burn 24h for accuracy");
  display.display();
  delay(2500);
}

// ============================================================
//  MQ135 → PPM  (CO2-equivalent)
// ============================================================
float mq135_ppm(int adcVal) {
  float ratio = mq135_Rs(adcVal) / mq135Ro;
  ratio = max(ratio, 0.01f);
  return constrain(MQ135_A * pow(ratio, MQ135_B), 0.0f, 9999.0f);
}

// ============================================================
//  AIR QUALITY WORD  (recalibrated for real-world Ro = 13.26 kΩ)
//  <50    = Fresh
//  50-80  = Good
//  80-100 = Fair
//  100-150= Mod      (shown as "Mod" on OLED to avoid overlap)
//  150-300= Poor
//  >300   = Danger
// ============================================================
String getAirQuality(float p) {
  if (p < 50)  return "Fresh";
  if (p < 80)  return "Good";
  if (p < 100) return "Fair";
  if (p < 150) return "Mod";
  if (p < 300) return "Poor";
  return "Danger";
}

// Full label for Blynk (no display width constraint)
String getAirQualityFull(float p) {
  if (p < 50)  return "Fresh";
  if (p < 80)  return "Good";
  if (p < 100) return "Fair";
  if (p < 150) return "Moderate";
  if (p < 300) return "Poor";
  return "Danger";
}

// ============================================================
//  PRESSURE TREND — ring buffer, one reading per minute
// ============================================================
void logPressure(float p) {
  pressureHistory[pressureIdx] = p;
  pressureIdx = (pressureIdx + 1) % PRESSURE_SAMPLES;
  if (pressureIdx == 0) pressureFull = true;
  int count     = pressureFull ? PRESSURE_SAMPLES : pressureIdx;
  int oldestIdx = pressureFull ? pressureIdx : 0;
  if (count < 2) { pressureTrend = 0; return; }
  pressureTrend = p - pressureHistory[oldestIdx];
}

// ============================================================
//  OVERALL STATUS — synthesised phrase for Page 1
//  Priority: danger → discomfort → weather → air → ok
// ============================================================
String computeStatus() {
  if (ppm > 200)                              return "Danger: Bad Air";
  if (rainPercent > 60)                       return "Heavy Rain";
  if (ppm > 150)                              return "Poor Air Quality";
  if (temp > 35 && humidity > 70)             return "Hot & Humid";
  if (temp > 38)                              return "Extreme Heat";
  if (humidity > 85 && rainPercent < 10)      return "Very Stuffy";
  if (pressureTrend < -2.0f && humidity > 65) return "Storm Likely";
  if (rainPercent > 20)                       return "Rain Detected";
  if (pressureTrend > 2.0f && humidity < 50)  return "Clearing Up";
  if (ppm > 110)                              return "Ventilate Now";
  if (lux < 50  && rainPercent < 10)          return "Overcast";
  if (lux > 50000)                            return "Bright & Sunny";
  if (temp >= 20 && temp <= 28 &&
      humidity >= 35 && humidity <= 65 &&
      ppm < 80)                               return "Comfortable";
  return "Stable";
}

// ============================================================
//  WIFI CONNECT — animated dots
// ============================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASSWORD);

  int dotCount = 0, attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    dotCount = (dotCount % 4) + 1;
    String dots = "";
    for (int i = 0; i < dotCount; i++) dots += " .";
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 20, "WiFi Connecting");
    display.drawString(64, 36, dots);
    display.display();
    delay(600);
    if (++attempts > 50) return;
  }
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 20, "WiFi Connected!");
  display.drawString(64, 36, WiFi.SSID());
  display.display();
  delay(2000);
}

void showReconnecting() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 20, "WiFi Lost...");
  display.drawString(64, 35, "Reconnecting");
  display.display();
}

// ============================================================
//  ICONS  (drawn within ~16×16 px bounding box)
// ============================================================
void drawCloud(int x, int y) {
  display.fillCircle(x + 4,  y + 5, 4);
  display.fillCircle(x + 9,  y + 3, 5);
  display.fillCircle(x + 14, y + 5, 4);
  display.fillRect(x + 2, y + 5, 14, 5);
}
void drawSun(int x, int y) {
  display.fillCircle(x + 8, y + 8, 4);
  display.drawLine(x + 8, y + 1,  x + 8, y - 2);
  display.drawLine(x + 8, y + 15, x + 8, y + 18);
  display.drawLine(x + 1, y + 8,  x - 2, y + 8);
  display.drawLine(x + 15,y + 8,  x + 18,y + 8);
  display.drawLine(x + 3, y + 3,  x + 1, y + 1);
  display.drawLine(x + 13,y + 13, x + 15,y + 15);
  display.drawLine(x + 13,y + 3,  x + 15,y + 1);
  display.drawLine(x + 3, y + 13, x + 1, y + 15);
}
void drawRainIcon(int x, int y) {
  display.fillCircle(x + 4,  y + 4, 3);
  display.fillCircle(x + 8,  y + 2, 4);
  display.fillCircle(x + 13, y + 4, 3);
  display.fillRect(x + 2, y + 4, 13, 4);
  display.drawLine(x + 3,  y + 9,  x + 2,  y + 13);
  display.drawLine(x + 8,  y + 9,  x + 7,  y + 13);
  display.drawLine(x + 13, y + 9,  x + 12, y + 13);
}

// ============================================================
//  PROGRESS BAR
// ============================================================
void drawBar(int x, int y, int w, int h, int pct) {
  int fill = constrain(map(pct, 0, 100, 0, w), 0, w);
  display.drawRect(x, y, w, h);
  if (fill > 2) display.fillRect(x + 1, y + 1, fill - 2, h - 2);
}

// ============================================================
//  SHARED UI
// ============================================================
void drawWiFiDot() {
  if (WiFi.status() == WL_CONNECTED) display.fillCircle(124, 4, 3);
  else                               display.drawCircle(124, 4, 3);
}
void drawPageDots(int cur) {
  for (int i = 0; i < 3; i++) {
    int cx = 55 + i * 8;
    if (i == cur) display.fillCircle(cx, 61, 2);
    else          display.drawCircle(cx, 61, 2);
  }
}

// ============================================================
//  PAGE 1 — RIGHT NOW
//
//  TYPOGRAPHY:
//    Header  : Plain_10  (page title)
//    Label   : Plain_10  ("TEMP", "HUM")
//    Value   : Plain_16  (primary sensor reading)
//    Bar     : h=6       (visual fill indicator)
//    Status  : Plain_10  (synthesised phrase)
//
//  PIXEL MAP (128×64):
//  y= 0– 9   "ENV. MONITOR" Plain_10 centred     WiFi dot (124,4)
//  y=10      horizontal divider
//  ── left (x=2..61) ────────── right (x=67..126) ──
//  y=12–21   "TEMP" Plain_10        "HUM"  Plain_10
//  y=23–38   temp   Plain_16        hum    Plain_16
//  y=40–45   temp bar h=6 (0–50°C)  hum bar h=6 (0–100%)
//  ─────────────────────────────────────────────────
//  y=47      horizontal divider
//  y=49–58   overallStatus Plain_10 centred
//  y=59–63   page dots (center y=61)
// ============================================================
void drawPageNow() {
  display.clear();

  // ── HEADER (Plain_10) ──────────────────────────────────────
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(60, 0, "ENV. MONITOR");
  display.drawLine(0, 10, 128, 10);
  drawWiFiDot();

  // vertical divider between left and right columns
  display.drawLine(64, 12, 64, 45);

  // ── LEFT — TEMPERATURE ────────────────────────────────────
  // Label (Plain_10)
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(2, 12, "TEMP");
  // Primary value (Plain_16)
  display.setFont(ArialMT_Plain_16);
  display.drawString(2, 23, String(temp, 1) + "C");
  // Bar  0–50 °C
  drawBar(2, 40, 60, 6, constrain(map((int)temp, 0, 50, 0, 100), 0, 100));

  // ── RIGHT — HUMIDITY ──────────────────────────────────────
  // Label (Plain_10)
  display.setFont(ArialMT_Plain_10);
  display.drawString(67, 12, "HUM");
  // Primary value (Plain_16)
  display.setFont(ArialMT_Plain_16);
  display.drawString(67, 23, String(humidity, 0) + "%");
  // Bar  0–100 %
  drawBar(67, 40, 59, 6, constrain(map((int)humidity, 0, 100, 0, 100), 0, 100));

  // ── DIVIDER ───────────────────────────────────────────────
  display.drawLine(0, 47, 128, 47);

  // ── STATUS (Plain_10) ─────────────────────────────────────
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 49, overallStatus);

  drawPageDots(0);
  display.display();
}

// ============================================================
//  PAGE 2 — ENVIRONMENT
//
//  TYPOGRAPHY:
//    Header  : Plain_10  (page title)
//    Label   : Plain_10  ("AIR QUALITY", "LIGHT")
//    Value   : Plain_16  (ppm — primary air quality reading)
//    Status  : Plain_10  (airQuality word, right-aligned beside label)
//    Secondary value: Plain_10  (lux — supplementary reading)
//
//  PIXEL MAP (128×64):
//  y= 0– 9   "ENVIRONMENT"  Plain_10 centred     WiFi dot
//  y=10      divider
//  y=12–21   "AIR QUALITY"  Plain_10 left   airQuality  Plain_10 right
//  y=23–38   ppm value      Plain_16 left
//  y=40–45   ppm bar h=6  (0–500 ppm)
//  y=47      divider
//  y=49–58   "LIGHT" Plain_10 left   lux value+unit Plain_10 right
//  y=59–63   page dots (center y=61)
// ============================================================
void drawPageEnv() {
  display.clear();

  // ── HEADER (Plain_10) ──────────────────────────────────────
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, "ENVIRONMENT");
  display.drawLine(0, 10, 128, 10);
  drawWiFiDot();

  // ── AIR QUALITY SECTION ───────────────────────────────────
  // Label (Plain_10) left  +  status word (Plain_10) right — same row
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(2, 12, "AIR QUALITY");
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(126, 12, airQuality);

  // Primary value (Plain_16)
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(2, 23, String((int)ppm) + " ppm");

  // Bar  0–500 ppm
  drawBar(2, 40, 124, 6, constrain(map((int)ppm, 0, 500, 0, 100), 0, 100));

  // ── DIVIDER ───────────────────────────────────────────────
  display.drawLine(0, 47, 128, 47);

  // ── LIGHT SECTION ─────────────────────────────────────────
  // Label (Plain_10) left  +  value (Plain_10) right — same row
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(2, 49, "LIGHT");
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(126, 49, String((int)lux) + " lx");

  drawPageDots(1);
  display.display();
}

// ============================================================
//  PAGE 3 — WEATHER
//
//  TYPOGRAPHY:
//    Header  : Plain_10  (page title)
//    Label   : Plain_10  ("RAIN", "FORECAST")
//    Value   : Plain_16  (rainPercent — primary reading)
//    Status  : Plain_10  (rainLevel right of label; weatherPrediction)
//    Secondary value: Plain_10  (pressure + trend)
//    Icon    : 16×16px  (drawn right side of Plain_16 value zone)
//
//  PIXEL MAP (128×64):
//  y= 0– 9   "WEATHER"  Plain_10 centred        WiFi dot
//  y=10      divider
//  y=12–21   "RAIN" Plain_10 left   rainLevel Plain_10 right
//  y=23–38   rain%  Plain_16 left   icon 16px (x=88, y=23) right
//  y=40–45   rain bar h=6  (0–100%)
//  y=47      divider
//  y=49–58   weatherPrediction Plain_10 left   pressure+trend Plain_10 right
//  y=59–63   page dots (center y=61)
// ============================================================
void drawPageWeather() {
  display.clear();

  // ── HEADER (Plain_10) ──────────────────────────────────────
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, "WEATHER");
  display.drawLine(0, 10, 128, 10);
  drawWiFiDot();

  // ── RAIN SECTION ──────────────────────────────────────────
  // Label (Plain_10) left  +  rainLevel status (Plain_10) right
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(2, 12, "RAIN");
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(126, 12, rainLevel);

  // Primary value (Plain_16) left
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(2, 23, String(rainPercent) + "%");

  // Icon — right side of value zone (x=88 keeps it clear of "99%" text ~x=0–32)
  if      (rainPercent > 30 || weatherPrediction == "Rain Likely")             drawRainIcon(88, 23);
  else if (weatherPrediction == "Clear" || weatherPrediction == "Clearing Up") drawSun(88, 23);
  else                                                                          drawCloud(88, 23);

  // Bar  0–100 %
  drawBar(2, 40, 124, 6, rainPercent);

  // ── DIVIDER ───────────────────────────────────────────────
  display.drawLine(0, 47, 128, 47);

  // ── FORECAST SECTION ──────────────────────────────────────
  // weatherPrediction (Plain_10) left  +  pressure+trend (Plain_10) right
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(2, 49, weatherPrediction);

  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  String trend = String((int)pressure) + "hPa";
  if      (pressureTrend >  2.0f) trend += " ^";
  else if (pressureTrend < -2.0f) trend += " v";
  else                            trend += " -";
  display.drawString(126, 49, trend);

  drawPageDots(2);
  display.display();
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  dht.begin();
  if (!bmp.begin())        Serial.println("[ERROR] BMP180 not found!");
  if (!lightMeter.begin()) Serial.println("[ERROR] BH1750 not found!");

  display.init();
  display.flipScreenVertically();
  display.clear();

  // Splash
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64,  8, "ENVIRONMENTAL");
  display.drawString(64, 24, "MONITORING");
  display.drawString(64, 40, "STATION");
  display.display();
  delay(2500);

  calibrateMQ135();   // 30s warmup → skipped if USE_OVERRIDE = true
  connectWiFi();      // animated dots → connected screen

  if (WiFi.status() == WL_CONNECTED) {
    Blynk.config(BLYNK_AUTH_TOKEN);
    Blynk.connect();
  }

  pinMode(RAIN_DO,  INPUT);
  pinMode(MQ135_DO, INPUT);
  for (int i = 0; i < PRESSURE_SAMPLES; i++) pressureHistory[i] = 0;
}

// ============================================================
//  LOOP
// ============================================================
void loop() {

  if (WiFi.status() != WL_CONNECTED) {
    showReconnecting();
    WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASSWORD);
    delay(1000);
    if (WiFi.status() == WL_CONNECTED) {
      Blynk.config(BLYNK_AUTH_TOKEN);
      Blynk.connect();
    }
    return;
  }

  Blynk.run();

  // Sensor read — every 2 seconds
  if (millis() - lastRead > 2000UL) {
    lastRead = millis();

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temp     = t;
    if (!isnan(h)) humidity = h;

    pressure = bmp.readPressure() / 100.0f;
    lux      = lightMeter.readLightLevel();

    int rainADC  = readStableADC(RAIN_AO);
    int mq135ADC = readStableADC(MQ135_AO);
    Serial.printf("MQ135 ADC: %d\n", mq135ADC);

    rainPercent = constrain(map(rainADC, 4095, 0, 0, 100), 0, 100);
    if      (rainPercent < 10) rainLevel = "Dry";
    else if (rainPercent < 30) rainLevel = "Moist";
    else if (rainPercent < 60) rainLevel = "Light Rain";
    else if (rainPercent < 85) rainLevel = "Moderate";
    else                       rainLevel = "Heavy Rain";

    ppm        = mq135_ppm(mq135ADC);
    airQuality = getAirQuality(ppm);

    if      (pressureTrend < -2.0f && humidity > 70) weatherPrediction = "Storm Likely";
    else if (pressureTrend < -1.5f)                  weatherPrediction = "Rain Likely";
    else if (pressureTrend >  2.0f && humidity < 50) weatherPrediction = "Clearing Up";
    else if (lux < 100 && rainPercent < 10)          weatherPrediction = "Overcast";
    else if (lux > 30000)                            weatherPrediction = "Clear";
    else                                             weatherPrediction = "Stable";

    overallStatus = computeStatus();

    Serial.printf("T:%.1fC H:%.0f%% P:%.1fhPa(trend:%.2f) Lux:%.0f Rain:%d%%[%s] PPM:%.0f[%s] => %s\n",
      temp, humidity, pressure, pressureTrend, lux,
      rainPercent, rainLevel.c_str(), ppm, airQuality.c_str(), overallStatus.c_str());
  }

  // Pressure trend log — every 1 minute
  if (millis() - lastPressureLog > PRESSURE_LOG_MS) {
    lastPressureLog = millis();
    logPressure(pressure);
  }

  // Blynk push — every 5 seconds
  if (millis() - lastBlynk > 5000UL) {
    lastBlynk = millis();
    Blynk.virtualWrite(V0, temp);
    Blynk.virtualWrite(V1, humidity);
    Blynk.virtualWrite(V2, pressure);
    Blynk.virtualWrite(V3, lux);
    Blynk.virtualWrite(V4, rainPercent);
    Blynk.virtualWrite(V5, ppm);
    Blynk.virtualWrite(V6, weatherPrediction);
    Blynk.virtualWrite(V7, getAirQualityFull(ppm));
    Blynk.virtualWrite(V8, rainLevel);
  }

  // Page switch — every 4 seconds
  if (millis() - lastSwitch > 4000UL) {
    page = (page + 1) % 3;
    lastSwitch = millis();
  }

  if      (page == 0) drawPageNow();
  else if (page == 1) drawPageEnv();
  else                drawPageWeather();
}
