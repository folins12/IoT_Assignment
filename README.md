# IoT Individual Assignment
**Hardware:** Heltec WiFi LoRa 32 V3 (ESP32-S3), INA219 Power Sensor  
**Framework:** FreeRTOS (PlatformIO)

This repository contains the firmware and evaluation for an IoT system that dynamically adapts its sampling frequency to save energy, performs local edge aggregation, and transmits data via WiFi (MQTT) and LoRaWAN (TTN). It also includes a DSP benchmarking environment to test anomaly detection filters (Z-Score and Hampel).

---

## Hands-on Walkthrough: Setup & Run

### 1. Hardware Prerequisites
* **Heltec WiFi LoRa 32 V3** (ESP32-S3)
* **INA219 I2C Power Sensor**
* **Wiring:**
  * INA219 `VCC` -> Heltec `3V3`
  * INA219 `GND` -> Heltec `GND`
  * INA219 `SDA` -> Heltec `Pin 41`
  * INA219 `SCL` -> Heltec `Pin 42`

### 2. Software Prerequisites
* **VSCode** with the **PlatformIO** extension installed.
* **Teleplot** extension in VSCode (for visualizing the real-time adaptive sampling graphs).

### 3. Configuration
Before compiling, you must insert your credentials into the code:
1. Open `src/main.cpp`.
2. Update the WiFi credentials: `ssid` and `password`.
3. Update the LoRaWAN TTN Keys (`joinEUI`, `devEUI`, and `appKey`) using the MSB format provided by your The Things Network console. 
   *(Note: For `RadioLib` v6.6.0 with LoRaWAN v1.0.3, the `appKey` is passed into both the nwkKey and appKey parameters during `beginOTAA`).*

### 4. Running the System (PlatformIO Environments)
This project uses two distinct PlatformIO environments configured in `platformio.ini` to separate the networking IoT application from the DSP benchmarking tool.

**To run the Main IoT System (Adaptive Sampling & Comms):**
1. In the VSCode PlatformIO bottom status bar, select the environment: `env:main`.
2. Click **Upload** (the right arrow icon) to flash `src/main.cpp`.
3. Open the **Teleplot** extension, select your COM port, set baud rate to `115200`, and connect. You will see real-time graphs of the input signal, the adapting sampling frequency, and the computed average.

**To run the Bonus DSP Benchmark (Anomaly Filtering):**
1. In the VSCode PlatformIO bottom status bar, switch the environment to: `env:bonus`.
2. Click **Upload** to flash `src/bonus.cpp`.
3. Open the standard **Serial Monitor** (plug icon). The board will output performance matrices comparing Z-Score and Hampel filters at various injection probabilities. You can edit the `ANOMALY_PROB` and `FILTER_WINDOW` constants at the top of `src/bonus.cpp` to test different scenarios.
