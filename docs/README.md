# IoT System Architecture & Performance Report

## 1. Input Signal Formulation
To ensure a mathematically precise evaluation without requiring external hardware signal generators, the signal is synthesized directly within the primary FreeRTOS sensing task. 
The baseline input signal (Mode 2) is:
$$s(t) = 3\sin(2\pi \cdot 4 \cdot t) + 1.5\sin(2\pi \cdot 8 \cdot t)$$

## 2. Maximum Hardware Sampling Frequency
A raw blocking benchmark (`analogRead()`) was executed on the Heltec ESP32-S3 ADC to determine the absolute hardware limit.
* **Empirical Result**: 100,000 samples executed in ~6.3 seconds.
* **Max Hardware Frequency**: **~15,772 Hz** (up to ~15,840 Hz depending on background tasks).
* **Conclusion**: The board is highly capable. We initialized our RTOS adaptive sampling task at a safe, over-sampled baseline of **100 Hz**.

## 3. Identify Optimal Sampling Frequency (Adaptive FFT)
A dedicated RTOS task (`TaskAnalyze`) collects 128 samples and runs the `arduinoFFT` library to evaluate the spectral composition.

* **Analysis**: The FFT successfully isolates the highest frequency component at **8.00 Hz**.
* **Adaptive Logic**: According to the Nyquist theorem, $f_s > 16$ Hz. To provide a safety margin for the ESP32 ADC and FreeRTOS tick granularity, the algorithm applies a multiplier: `newFreq = (int)(peak * 2.2) + 2`.
* **Result**: The system dynamically adapts the sampling frequency to **~19-20 Hz**.

## 4. Aggregation Over a Window
Instead of flooding the network with raw telemetry, the IoT node computes the arithmetic average of the sampled wave over a continuous **5,000 ms (5-second) sliding window**. Because the baseline input is a symmetrical composite sine wave, the resulting aggregated payload mathematically flattens to `~0.00`, proving that no data points are skipped by the RTOS scheduler.

## 5. Network Communication
* **Edge / WiFi (MQTT)**: The aggregated 5-second average is pushed to `broker.hivemq.com` using the `PubSubClient`.
* **Cloud (LoRaWAN)**: Simultaneously, the system transmits the data via LoRaWAN OTAA to The Things Network (TTN) using `RadioLib`. A duty-cycle logic restricts LoRa transmissions to 1 out of every 6 windows to comply with fair use policies.

## 6. Performance & System Evaluation
* **Energy Savings**: INA219 power monitoring logs show the current consumption drops from **~106 mA** during active computation/transmission to **~45 mA** during RTOS idle periods. By adapting from 100 Hz to 20 Hz, the system spends significantly more time in the 45 mA low-power state.
* **Per-Window Execution Time**: The core FFT computation (`TaskAnalyze`) takes an impressively low **~2,950 µs** (2.95 ms) to process 128 samples.
* **Network Payload Reduction**: Transmitting raw data at 100 Hz requires 500 floats (2,000 bytes) every 5 seconds. Edge aggregation reduces this to exactly 1 float (**4 bytes**).
* **End-to-End Latency**: The recorded MQTT E2E latency (from window close to network transmission) generally hovers between **1 ms and 3 ms**, though it occasionally spikes to ~1,500 ms depending on network congestion.

---

## 7. Bonus 1: Multi-Signal Adaptive Analysis
The adaptive logic was tested against three diverse signals:
1. **Low Freq ($1$ Hz)**: System adapts to ~10 Hz. Massive energy savings; CPU is almost entirely idle.
2. **Mixed Freq ($8$ Hz)**: System adapts to ~20 Hz. Balanced performance.
3. **High Freq ($35$ Hz)**: System adapts to ~79-95 Hz. Minimal CPU savings compared to the 100 Hz baseline.

**Discussion:** Static over-sampling wastes energy if the physical phenomenon is slow. Adaptive FFT dynamically scales the duty cycle. Even for the 35 Hz signal where CPU savings are minimal, the *network* savings remain identical (4 bytes transmitted every 5s), proving edge-aggregation is universally beneficial.

---

## 8. Bonus 2: Advanced DSP & Anomaly Filtering
The signal was injected with Gaussian noise ($\sigma=0.2$) and a sparse uniform anomaly process ($U(5, 15)$) to simulate EMI spikes. We evaluated **Z-Score** and **Hampel** filters across different injection probabilities ($P$) and window sizes ($W$).

### The Impact of Anomalies on FFT & Energy
Unfiltered anomalies register as broadband high-frequency energy. In the logs, unfiltered spikes caused the FFT to wrongly detect dominant peaks at random frequencies. Without filtering, this forces the adaptive sampler to stay awake at maximum frequency, destroying the battery.

### Empirical Filter Performance (Based on Logs)
* **Z-Score Failure (Masking Effect):** The logs prove the Z-Score filter consistently fails (TPR near $0.00$, MER ~ $0.0\%$). Massive outliers distort the standard deviation, causing the anomaly to "hide" itself inside the artificially inflated threshold.
* **Hampel Success:** The Hampel (median-based) filter accurately identified anomalies. At $P=0.01$ and $W=5$, it achieved a True Positive Rate (TPR) of **1.00** and a Mean Error Reduction (MER) of **~49.4%**.

### The Computational Paradox (Execution Time)
Counter-intuitively, the logs show the Hampel filter executing *faster* than the Z-Score filter on the ESP32:
* **$W=5$**: Z-Score $\approx 995$ µs | Hampel $\approx 635$ µs
* **$W=15$**: Z-Score $\approx 2,440$ µs | Hampel $\approx 2,160$ µs
* **$W=31$**: Z-Score $\approx 4,620$ µs | Hampel $\approx 3,650$ µs

**Why?** The Z-score implementation utilizes the `pow()` function to calculate variance, which is highly unoptimized on the ESP32 CPU. Conversely, the Hampel filter relies on `std::sort` for small static arrays. The ESP32 sorts 31 elements in memory much faster than it can compute 31 floating-point powers. 

### Conclusion on Window Size
Larger windows ($W=31$) do not improve performance. They increase execution latency (from 635 µs to 3,650 µs) and decrease the MER (dropping from ~50% to ~20%). An empirical window size of **$W=5$** paired with a **Hampel Filter** provides the ultimate balance of anomaly rejection, low execution time, and minimal RTOS latency.