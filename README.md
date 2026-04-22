# IoT Individual Assignment - Andrea Folino

This IoT system basically collects sensor data, computes Fast Fourier Transform, and dynamically adapts its sampling frequency to minimize energy consumption. Aggregated data is transmitted to the edge (MQTT) and the cloud (LoRaWAN/TTN).

## Hardware Setup
This project requires two **Heltec WiFi LoRa 32 V3** boards:
1. **Target Node:** Runs the main firmware (sampling, FFT, MQTT, LoRaWAN).
2. **Monitor Node:** Through an **INA219 Sensor** measures the current drawn by the Target Node.
   * `VCC` -> Heltec `3V3`
   * `GND` -> Heltec `GND`
   * `SDA` -> Heltec `Pin 41`
   * `SCL` -> Heltec `Pin 42`
   * The INA219 is placed in series with the **3.7V LiPo battery** powering the Target Node.

## Project Structure
* `src/main.cpp`: Main FreeRTOS firmware for the Target Node.
* `src/bonus.cpp`: Anomaly injection and DSP filtering benchmarks (Z-Score & Hampel).
* `src/monitor.cpp`: Power monitoring firmware.
* `src/credentials.h`: Network and TTN API keys (hidden in github).
* `docs/`: output logs, and empirical benchmark data (screen included) separated in the respective case folders.

## Configuration & Deployment
This project is built with **PlatformIO**. 

1. Update `src/credentials.h` with your Wi-Fi SSID/Password and TTN keys.
2. In `src/main.cpp`, you can configure the behavior:
   * `#define SIGNAL_MODE` (Switches between Base (1), Slow (2), and Fast (3) input signals).
   * `#define RUN_BONUS_8_2` (Enables (1) anomaly filtering benchmark task).
3. Open the PlatformIO Core CLI or UI:
   * Upload to the main board: `pio run -e target -t upload`
   * Upload to the power monitor: `pio run -e monitor -t upload`

---

# System Performance & Evaluation 

This section provides an empirical analysis of the IoT adaptive system. All data points are derived from real execution logs stored in the `docs/` directory.

## 1. Physical Hardware Benchmarking
Before initializing the FreeRTOS scheduler, the system established its physical ceiling through a blocking ADC loop in the `setup()` phase.
* **Maximum Raw Sampling Rate:** ~16,000 Hz.
* **Assignment Baseline:** 100 Hz (Initial over-sampled state).

## 2. Dynamic Frequency Adaptation (FFT)
The system analyzes the signal's spectral density every 128 samples to adjust the sensing duty cycle.

| Signal Mode | Physical Freq | FFT Peak Detected | Adapted Sampling | Power State |
| :--- | :--- | :--- | :--- | :--- |
| **Mode 1 (Slow)** | 1.0 Hz | 1.01 Hz | **10 Hz** | Deep Idle |
| **Mode 2 (Base)** | 4.0 Hz | 4.03 Hz | **11 Hz** | Balanced |
| **Mode 3 (Fast)** | 35.0 Hz | 35.23 Hz | **90 Hz** | Active |

**Logic Applied:** $f_s = (f_{peak} \times 2.5) + 1$. This ensures the system stays above the Nyquist limit.

## 3. Communication & Network Efficiency
The aggregation process (5-second window) transforms high-frequency raw data into a single efficient payload.

* **Payload Compression:** A raw 100 Hz stream generates 500 floats (2,000 bytes) every 5 seconds. By adapting the frequency to 11 Hz, the raw stream drops to 220 bytes. Our edge aggregation further reduces this to **4 bytes** (a single float), a **99.8% reduction** in network overhead.
* **End-to-End Latency:** * **MQTT (WiFi):** Stable at **~4950 ms** (this matches the exact 5-second buffer aggregation time).
    * **LoRaWAN (TTN):** Latency spikes up to **6,000-8,600 ms** during Join procedures or uplink windows due to the radio hardware's blocking nature on the SPI bus.

## 4. Power Consumption Diagnostics
Logs from the INA219 sensor reveal a direct correlation between system tasks and energy absorption. Calculations are based on a **3.7V** LiPo battery source:

| Operation Mode | Current (mA) | Power (mW) | Diagnostic Explanation |
| :--- | :--- | :--- | :--- |
| **Boot / Oversampled**| **~60.90 mA** | ~225 mW | CPU highly active at 100Hz sensing, initial boot. |
| **Idle / Adaptive** | **~47.50 mA** | ~175 mW | CPU active at low freq (11Hz), radios in sleep. |
| **WiFi / MQTT TX** | **~105.80 mA** | ~391 mW | Periodic spike every 5s during WiFi radio wake-up. |
| **LoRaWAN Uplink** | **~107.40 mA** | ~397 mW | Long-range TX. |


### 4.1 Interpreting Power Consumption for Bonus Filters
In the Bonus implementation (`RUN_BONUS_8_2 = 1`), the Unfiltered, Z-Score, and Hampel evaluations run sequentially inside a dedicated background task. Because the INA219 prints every 5 seconds, it captures a "blended" current reading of these three states running together (fluctuating between **39mA and 48.9mA**). 

**How to evaluate their energy impact:**
Since the filters are heavily optimized for the ESP32's hardware FPU, they do not cause massive instantaneous current spikes. Instead, their energy cost is measured in **CPU execution time** (microseconds). 
* Z-Score takes **~320µs - ~650µs** per batch.
* Hampel takes **~660µs - ~3600µs** per batch. 
The true energy cost is the amount of time the processor is forced to stay awake computing these arrays instead of yielding to the FreeRTOS Idle task.

---

## 5. Anomaly Filtering Benchmark (Bonus Part 8.2)
The system was tested against 9 unique configurations to characterize the trade-off between statistical accuracy and computational effort.

### 5.1 Results Table (Average Metrics from Logs)

| Contamination ($P$) | Window ($W$) | Filter Type | TPR | FPR | MER | Exec Time (µs) | FFT Peak (Hz) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **0.01 (1%)** | **5** | Z-Score | 0.00 | 0.00 | 0.0% | ~321 µs | 4.02 Hz |
| | | Hampel | **0.40** | 0.00 | **17.1%** | ~660 µs | 4.00 Hz |
| **0.01 (1%)** | **15** | Z-Score | 0.33 | 0.00 | 21.6% | ~325 µs | 4.02 Hz |
| | | Hampel | **0.80** | 0.00 | **29.7%** | ~2,420 µs | 4.02 Hz |
| **0.01 (1%)** | **31** | Z-Score | 0.25 | 0.00 | 16.5% | ~330 µs | 4.08 Hz |
| | | Hampel | **0.58** | 0.00 | **22.0%** | ~3,630 µs | 4.04 Hz |
| **0.05 (5%)** | **5** | Z-Score | 0.00 | 0.00 | 0.0% | ~325 µs | 4.04 Hz |
| | | Hampel | **0.82** | 0.00 | **62.9%** | ~655 µs | 4.02 Hz |
| **0.05 (5%)** | **15** | Z-Score | 0.50 | 0.00 | 32.7% | ~328 µs | 4.01 Hz |
| | | Hampel | **0.75** | 0.00 | **54.9%** | ~2,360 µs | 4.01 Hz |
| **0.05 (5%)** | **31** | Z-Score | 0.40 | 0.00 | 28.3% | ~335 µs | 4.05 Hz |
| | | Hampel | **0.56** | 0.00 | **40.7%** | ~3,590 µs | 4.04 Hz |
| **0.10 (10%)** | **5** | Z-Score | 0.00 | 0.00 | 0.0% | ~328 µs | 4.10 Hz |
| | | Hampel | **0.70** | 0.00 | **63.1%** | ~665 µs | 4.03 Hz |
| **0.10 (10%)** | **15** | Z-Score | 0.17 | 0.00 | 13.0% | ~330 µs | 4.10 Hz |
| | | Hampel | **0.55** | 0.00 | **48.3%** | ~2,300 µs | 4.01 Hz |
| **0.10 (10%)** | **31** | Z-Score | 0.15 | 0.00 | 19.1% | ~650 µs* | 4.15 Hz |
| | | Hampel | **0.38** | 0.00 | **36.4%** | ~3,570 µs | 4.02 Hz |


### 5.2 Comparative Analysis
* **Z-Score:** At small windows ($W=5$), the Z-score consistently fails, yielding **0.0% MER** regardless of the contamination rate. The injected outliers ($+/- 15$) artificially inflate the local standard deviation, expanding the detection threshold so widely that the anomalies themselves are masked as normal.
* **Hampel:** The Median-based Hampel filter successfully ignores outlier magnitude. It peaks at a **63.1% MER** at 10% contamination with a small window ($W=5$), demonstrating strong robustness against high-magnitude transients where Z-Score fails. 
* **Window Size Trade-off:**
    * **Latency:** Execution time scales linearly with window size for the Hampel filter. Moving from $W=5$ (~660 µs) to $W=31$ (~3.6 ms) introduces significant processing overhead, eating into RTOS idle time. Z-Score remains highly efficient (~330 µs) across window sizes due to FPU optimization, but lacks accuracy.
    * **Accuracy vs. Density:** For Hampel, larger windows perform poorly at higher contamination rates. At 10% probability, increasing $W$ from 5 to 31 drops the TPR from 0.70 down to 0.38. A wider window is more likely to encompass multiple anomalies, skewing the median estimate itself.


---

# LLM Usage Report & Prompts



## Prompts Issued

1. **LoRaWAN:**
   "I'm using RadioLib v6.6.0 on a Heltec V3 ESP32-S3. TTN is giving me an error saying 'devnonce has already been used'. How do I correctly structure the `node.beginOTAA` and `activateOTAA` loops to handle this, and how should my keys be formatted (MSB or LSB)?"

2. **ESP32 FPU Math Bottleneck:**
   "My Z-Score filter is running slower than my Hampel filter on the ESP32, which doesn't make sense since sorting should be O(n log n). I am using the `pow(val, 2)` function for the variance. Is `pow()` heavily unoptimized on the ESP32? What's the alternative?"

3. **Signal Processing Theory:**
   "If I have a clean 4 Hz sine wave and I inject a single large-magnitude spike (e.g. value of 15) into an array of 128 samples, will the dominant frequency in the FFT change?"

4. **Choosing the Right Edge MQTT Broker:**
   "I need a public MQTT broker for testing an IoT university assignment. I am using an ESP32 with the PubSubClient library. What are the best public options. Why is HiveMQ a good choice for reliable edge communication?"

### 5.1 Opportunities and Limitations

Based on the specific prompts issued during development, the following observations were made regarding the LLM's capabilities as an engineering co-pilot:

* **Opportunities:**
  * **Algorithm & FPU Optimization:** It successfully identified non-obvious mathematical bottlenecks. For instance, it pointed out that using `pow(val, 2)` for the Z-Score variance invokes heavy math library computations. Suggesting simple multiplication instead allowed the system to leverage the ESP32's hardware FPU, drastically reducing the filter's execution time.
  * **Theoretical Clarifications:** It provided solid theoretical explanations for Digital Signal Processing, correctly explaining that a sparse anomaly spike acts as a Dirac delta in the time domain, raising the FFT broadband noise floor rather than shifting the dominant frequency peak.
  * **Architectural Decisions:** It efficiently compared Edge MQTT brokers, accurately highlighting HiveMQ's suitability for a frictionless, non-TLS embedded testing environment.

* **Limitations:**
  * **Inability to Resolve Non Coding Errors:** Despite multiple iterations, the LLM was entirely unable to solve the TTN `-1101 (DevNonce already used)` error. 
