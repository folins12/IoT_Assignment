# IoT Individual Assignment – Walkthrough

## Hardware Required

| Component | Qty | Role |
|---|---|---|
| Heltec WiFi LoRa 32 V3 (ESP32-S3) | 2 | Target Node + Monitor Node |
| INA219 power sensor | 1 | Current/power measurement |
| Jumper wires | — | INA219 wiring |

### INA219 Wiring (Monitor Node)
```
INA219 VCC  -> Heltec 3V3
INA219 GND  -> Heltec GND
INA219 SDA  -> Heltec Pin 41
INA219 SCL  -> Heltec Pin 42
```
Place the INA219 **in series** with the Target Node's power supply line (Vin+).

### Serial Link (Target → Monitor)
```
Target TX (Pin 4)  -> Monitor RX (Pin 4)
Target GND         -> Monitor GND
```

---

## Repository Structure

```
src/
  main.cpp        # Target: sampling, FFT, aggregation, MQTT, LoRaWAN
  bonus.cpp       # Target: Z-score & Hampel anomaly benchmark
  monitor.cpp     # Monitor: INA219 power logging
  config.h        # Shared constants and structs
  credentials.h   # WiFi/MQTT/TTN keys (NOT committed – see .gitignore)
platformio.ini    # Two build envs: target / monitor
docs/
  technical.md    # Full technical report (answers all assignment points)
  prompts.md      # LLM prompt log
  logs/           # Serial logs per signal mode
  screenshots/    # HiveMQ and TTN dashboard screenshots
```

---

## Setup

### 1. Clone and open in PlatformIO (VSCode)
```bash
git clone https://github.com/folins12/IoT_Individual_Assignment
cd IoT_Individual_Assignment
```

### 2. Create `src/credentials.h` (already in `.gitignore`)
```cpp
#pragma once
#define WIFI_SSID       "YourSSID"
#define WIFI_PASSWORD   "YourPassword"
#define MQTT_SERVER     "broker.hivemq.com"
#define MQTT_PORT       1883
#define MQTT_TOPIC      "iot_assignment/folins12/average"
#define MQTT_CLIENT_ID  "HeltecTarget"
#define LORAWAN_JOIN_EUI  0x0000000000000000ULL
#define LORAWAN_DEV_EUI   0x70B3D57ED0076985ULL
#define LORAWAN_APP_KEY   { 0xE0,0x9A,0x93,0xC8,0xAE,0x12,0x13,0x47, \
                            0x75,0x8F,0x38,0xCA,0x40,0xA0,0xDA,0xFD }
```

### 3. Configure the signal and bonus mode in `src/config.h`
```cpp
#define SIGNAL_MODE   2    // 1, 2, or 3
#define RUN_BONUS     1    // 1=enabled, 0=disabled
```

### 4. Register on TTN
- Create an application on [console.thethingsnetwork.org](https://console.thethingsnetwork.org)
- Add a device with OTAA activation
- Copy `DevEUI`, `AppEUI`, `AppKey` into `credentials.h` (MSB format)

### 5. Flash the boards
```
PlatformIO menu -> env:target  -> Upload  (first ESP32)
PlatformIO menu -> env:monitor -> Upload  (second ESP32, connected to INA219)
```

### 6. Open serial monitors
Open **two** serial monitors at 115200 baud – one per board.

---

## Expected Output

### Target Node (env:target)
```
[BOOT] Signal=2 | Bonus=ON | InitHz=100
[WIFI] Connecting to YourSSID... OK
[BENCH] Max ADC freq: 84521 Hz  |  Configured: 100 Hz
[LORA] Attempting OTAA join...
[LORA] Join SUCCESS
[FFT] Peak=8.00Hz | 100Hz->21Hz | exec=1823us
--------------------------------------------------
[WIN#1] avg=0.0023 | hz=21 | samples=105(vs 500) | exec=5001ms | e2e=5043ms | data saving=79.0%
[MQTT] 0.0023 -> OK
[LORA] Duty-cycle: 1/6
--------------------------------------------------
```

### Monitor Node (env:monitor)
```
[INA219] Connected
[PHASE] -> ADAPTIVE
[INA219] Phase=ADAPTIVE   |   68.3 mA |   59.8 mW |  59.800 mJ
[ENERGY SUMMARY]
  OVERSAMPLE: 63.1 mW avg | 1890.0 mJ total | 30 samples
  ADAPTIVE  : 42.5 mW avg |  127.5 mJ total | 30 samples
  Saving    : 32.7%
```

### HiveMQ (MQTT)
Subscribe to `iot_assignment/folins12/average` on `broker.hivemq.com:1883` with any MQTT client (e.g. MQTT Explorer).

---

## Changing Signal Mode
Edit `SIGNAL_MODE` in `config.h`, re-flash `env:target`:

| Mode | Signal | Max Freq | Expected Adaptive Hz |
|---|---|---|---|
| 1 | 5·sin(2π·1·t) | 1 Hz | ~3 Hz |
| 2 | 3·sin(2π·4·t)+1.5·sin(2π·8·t) | 8 Hz | ~21 Hz |
| 3 | 2·sin(2π·35·t) | 35 Hz | ~89 Hz |

---

## Disabling the Bonus Benchmark
Set `RUN_BONUS 0` in `config.h`. The `bonus.cpp` file is still compiled but `TaskBonus` is not created.