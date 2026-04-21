# IoT Individual Assignment

This project implements an adaptive IoT system based on two ESP32 boards that work together to monitor signals and energy consumption.

## 1. Repository Structure
* **`src/`**
  * `main.cpp`: Firmware for the **Target Node**. Handles adaptive sampling, FFT, DSP filters, and MQTT/LoRaWAN transmissions.
  * `monitor.cpp`: Firmware for the **Monitor Node**. Exclusively handles power consumption reading via the INA219 sensor.
* **`docs/`**
  * `README_docs.md`: Detailed technical report on the architecture, Nyquist logic, and performance analysis.
  * `prompts.md`: List of prompts used with the LLM during development.
  * Folders `base/`, `bonus_1/`, `bonus_2/`, `power_consumption/`: Contain execution logs and The Things Network (TTN) dashboard screenshots.
* **`platformio.ini`**: Configuration of the build environments (`target` and `monitor`) and library dependencies.

## 2. Hardware and Wiring
The system uses two **Heltec WiFi LoRa 32 V3** (ESP32-S3) boards.

### Node A: Target Node
Performs signal monitoring and data analysis.
* **Components**: ESP32-S3.
* **Functions**: Signal sampling, FFT, data transmission to HiveMQ (WiFi) and TTN (LoRaWAN).

### Node B: Monitor Node
Measures the energy absorbed by the Target Node.
* **Components**: ESP32-S3 + **INA219** Power Sensor.
* **INA219 Wiring**:
  * `VCC` -> Heltec `3V3`
  * `GND` -> Heltec `GND`
  * `SDA` -> Heltec `Pin 41`
  * `SCL` -> Heltec `Pin 42`

## 3. Setup and Configuration
This project is developed using **PlatformIO**. Before compiling, you must configure your credentials:

1. Open `src/main.cpp`.
2. **WiFi**: Insert your network's `ssid` and `password`.
3. **LoRaWAN**: Update the TTN keys (`joinEUI`, `devEUI`, `appKey`) in MSB format.
4. **Operating Modes**:
   * Modify `SIGNAL_MODE` to test different input signals.
   * Set `RUN_BONUS_8_2` to `1` to run the DSP filter benchmarks against anomalies.

## 4. Execution
Select the correct environment from the PlatformIO menu to upload the firmware to the respective board:
* Upload `env:target` to the first ESP32 (the node that samples the signal).
* Upload `env:monitor` to the second ESP32 (the one connected to the INA219).