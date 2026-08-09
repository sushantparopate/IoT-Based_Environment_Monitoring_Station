# IoT-Based Environment Monitoring Station

An ESP32-based environmental monitoring station that reads temperature, humidity, barometric pressure, ambient light, air quality (CO₂-equivalent), and rainfall, then displays live readings on a 128×64 OLED and streams them to the Blynk IoT dashboard.

The station cycles through three OLED pages — **Now** (temp/humidity), **Environment** (air quality/light), and **Weather** (rain/pressure trend/forecast) — and synthesizes a plain-language status ("Comfortable", "Storm Likely", "Ventilate Now"...) from all sensor readings combined.

## Features

- **Multi-sensor fusion** — DHT22, BMP180, BH1750, MQ135, and a resistive rain sensor feed a single status engine
- **Local pressure trend tracking** — a 10-minute rolling window distinguishes real weather trends from sensor noise
- **MQ135 self-calibration** — estimates its own clean-air baseline (Ro) on boot, or use a burned-in fixed value for instant startup
- **On-device OLED UI** — three auto-cycling pages with bar graphs, a live weather icon, and a WiFi status indicator
- **Blynk Cloud sync** — all readings pushed to a mobile dashboard every 5 seconds
- **Credentials kept out of git** — WiFi and Blynk secrets live in a gitignored `secrets.h`, not in the sketch itself

## Hardware

| Component | Notes |
|---|---|
| ESP32 DevKit (30/38-pin) | Any ESP32 dev board with enough GPIO |
| DHT22 | Temperature + humidity |
| BMP180 | Barometric pressure |
| BH1750 | Ambient light (lux) |
| MQ135 | Air quality (CO₂-equivalent, analog) |
| Resistive rain sensor | Analog rainfall sensing |
| SH1106 128×64 OLED | I2C display |

### Wiring

| Signal | ESP32 Pin |
|---|---|
| I2C SDA (OLED, BMP180, BH1750) | GPIO 21 |
| I2C SCL (OLED, BMP180, BH1750) | GPIO 22 |
| DHT22 data | GPIO 4 |
| Rain sensor analog out | GPIO 34 |
| Rain sensor digital out | GPIO 35 |
| MQ135 analog out | GPIO 32 |
| MQ135 digital out | GPIO 33 |

OLED, BMP180, and BH1750 all share the same I2C bus at address defaults (OLED `0x3C`, BH1750 `0x23`).

## Getting Started

### 1. Clone the repo

```bash
git clone https://github.com/sushantparopate/IoT-Based_Environment_Monitoring_Station.git
```

Arduino IDE requires the sketch filename to match its containing folder, so keep this folder name and `.ino` filename in sync if you rename anything.

### 2. Install Arduino IDE + ESP32 board support

Arduino IDE 2.x, with the ESP32 board package added via **Boards Manager**.

### 3. Install required libraries

Via **Library Manager**:

- `Adafruit BMP085 Library`
- `BH1750`
- `DHT sensor library` (Adafruit)
- `ESP8266 and ESP32 OLED driver for SSD1306/SH1106` (ThingPulse)
- `Blynk` (BlynkSimpleEsp32)

`WiFi` ships with the ESP32 core.

### 4. Set up your credentials

WiFi and Blynk credentials are kept in `secrets.h`, which is excluded from git so nothing personal ever ends up in the repo.

```bash
cp secrets.h.example secrets.h
```

Then open `secrets.h` and fill in:

- `SECRET_WIFI_SSID` / `SECRET_WIFI_PASSWORD` — your network (the sketch connects to one network only)
- `SECRET_BLYNK_TEMPLATE_ID` / `SECRET_BLYNK_TEMPLATE_NAME` / `SECRET_BLYNK_AUTH_TOKEN` — from your Blynk.Cloud project's Device Info tab

`secrets.h` must sit in the same folder as the `.ino` file. It will never be picked up by git as long as `.gitignore` is present.

### 5. Flash

Select your ESP32 board + port, then upload. On first boot the MQ135 runs a 30-second clean-air calibration (skippable — see below).

### 6. Blynk dashboard

Set up virtual pins `V0`–`V8` on your Blynk template to match: temperature, humidity, pressure, lux, rain %, ppm, forecast text, air quality text, rain level text.

## MQ135 Calibration

The MQ135's baseline resistance (`Ro`) drifts with the sensor's burn-in time, so out-of-the-box PPM readings are approximate. To lock in an accurate value after a 24–48h burn-in:

1. Set `MQ135_USE_OVERRIDE` to `false` and flash
2. Place the device outdoors or near an open window, power on, and let the 30-second warmup finish
3. Open the Serial Monitor at 115200 baud and note the printed `Ro` value
4. Set `MQ135_USE_OVERRIDE` back to `true` and `MQ135_RO_OVERRIDE` to that value
5. Reflash — the boot warmup is skipped permanently

## Project Structure

```
IoT-Based_Environment_Monitoring_Station/
├── IoT-Based_Environment_Monitoring_Station.ino   # Main sketch
├── secrets.h.example                              # Credential template (committed)
├── secrets.h                                      # Your real credentials (gitignored — not committed)
├── README.md
├── LICENSE
└── .gitignore
```

## License

MIT — see [LICENSE](LICENSE).
