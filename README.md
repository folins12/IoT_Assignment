# Hands-on Walkthrough: Setup & Run

### 1. Repository Structure
* **`src/`**
  * `main.cpp`: It contains the adaptive sampling, FFT, MQTT and LoRa part.
  * `max_freq.cpp`: Standalone script to test the ESP32 ADC maximum capabilities.
  * `bonus.cpp`: The DSP benchmarking tool for anomaly injection and filtering evaluation.
* **`docs/`**
  * `README.md`: The main documentation file describing all the project parts.
  * `prompts.md`: Contains the list of LLM prompts used during the development.
  * `max_freq_log.txt` & `bonus_log.txt`: Store the output logs respectively of `max_freq.cpp` and `bonus.cpp`.
  * `plots/`: Folder containing Teleplot screenshots.

### 2. Hardware
* **Heltec WiFi LoRa 32 V3** (ESP32-S3)
* **INA219 I2C Power Sensor**
* **Wiring:**
  * INA219 `VCC` -> Heltec `3V3`
  * INA219 `GND` -> Heltec `GND`
  * INA219 `SDA` -> Heltec `Pin 41`
  * INA219 `SCL` -> Heltec `Pin 42`

### 3. Configuration
Before compiling you must insert your WiFi and TTN credentials into the code:
1. Open `src/main.cpp`.
2. Update the WiFi credentials: `ssid` and `password`.
3. Update the LoRaWAN TTN Keys (`joinEUI`, `devEUI`, and `appKey`) using the MSB format provided by your The Things Network console.