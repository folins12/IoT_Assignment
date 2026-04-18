### 1. Input Signal Formulation
To guarantee pristine mathematical integrity without requiring external DAC hardware, the input signal was generated mathematically within the primary FreeRTOS sensing task. The signal takes the form of $s(t) = 3\sin(2\pi \cdot 4 \cdot t) + 1.5\sin(2\pi \cdot 8 \cdot t)$. The maximum inherent frequency of this signal is **8 Hz**.

### 2. Maximum Sampling Frequency
A raw benchmark was performed on the ESP32-S3 Analog-to-Digital Converter (ADC) using a blocking `analogRead()` loop to demonstrate over-sampling capabilities.
* **Result:** 100,000 samples were captured in 6.111358 seconds.
* **Max Frequency:** 16,362.97 Hz.
* **Conclusion:** This proves the hardware is vastly capable of over-sampling. We initialize our RTOS sampling task at **100 Hz**, representing a highly over-sampled state.

### 3. Identify Optimal Sampling Frequency
A dedicated FreeRTOS task executes the `arduinoFFT` library every 10 seconds.
* The FFT correctly identified the peak frequency at **8.00 Hz**.
* Applying the Nyquist-Shannon sampling theorem ($f_s \ge 2 \cdot f_{max}$) with an engineering safety margin of 2.5x, the logic automatically adapts the sampling frequency to **20 Hz**.
* **Edge Case Discovered (Aliasing):** When strictly attempting to sample at exactly 2.0x (16 Hz), the system suffered perfect phase-alignment signal loss. The 8 Hz component fell exactly on the zero-crossings of the 16 Hz sample rate, causing the FFT to falsely lock onto the remaining 4 Hz wave. This real-world finding validated the necessity of the 2.5x oversampling margin used in the final code.

### 4. Aggregation Over a Window
Instead of sending raw high-frequency data, the primary task computes the arithmetic mean of the wave over a continuous **5-second window**. Because the signal is a symmetrical sine wave, this edge-computed aggregation correctly and predictably flattens the output to `~0.00`.

### 5. Edge Server Communication (MQTT)
The 5-second average is pushed to a FreeRTOS Queue, consumed by `TaskMQTT`, and transmitted via `PubSubClient` over WiFi to a public HiveMQ broker (`broker.hivemq.com`).

### 6. Cloud Communication (LoRaWAN)
A secondary communication task (`TaskLoRa`) reads the same average and transmits it via LoRaWAN OTAA using `RadioLib` to The Things Network (TTN). 

### 7. Performance Measurement
* **Energy Savings:** By autonomously adapting the sampling rate from 100 Hz to 20 Hz, the system reduced its active CPU polling cycles by **80%**, representing a nearly proportional decrease in active sensing energy consumption.
* **Per-Window Execution Time:** The `arduinoFFT` library executed the 128-sample transformation in just **2.3 milliseconds** (`2,354 µs`).
* **Volume of Data Transmitted:** Sending raw data at 100 Hz would require transmitting 500 floats (2,000 bytes) every 5 seconds. Aggregation reduces this to exactly 1 float (4 bytes). This yields a **99.8% reduction** in network payload size.
* **End-to-End Latency:** MQTT network latency to the Edge server averaged **45ms**. 
* **Hardware Observation (EMI):** During LoRaWAN TTN transmissions, the radio frequency bursts temporarily caused Electromagnetic Interference (EMI) that knocked out the unshielded USB-to-Serial connection. FreeRTOS correctly buffered this downtime and resumed flawless aggregation once the radio burst concluded.

---

## 8 Bonus: Advanced DSP & Anomaly Filtering Analysis
To evaluate the system under noisy conditions, the `env:bonus` environment benchmarks an alternative signal: $s(t) = 2\sin(2\pi \cdot 3 \cdot t) + 4\sin(2\pi \cdot 5 \cdot t)$ injected with Gaussian baseline noise ($\sigma=0.2$) and a sparse anomaly spike process ($U(5, 15)$). Two anomaly-aware filters (Z-score and Hampel) were evaluated across varying injection probabilities ($p=0.01, 0.05, 0.10$) and window sizes.

**The Impact of Anomalies on Frequency Estimation:**
Transient hardware faults (spikes) act as impulses, introducing massive high-frequency noise across the spectrum. At a 10% injection rate ($p=0.10$) without filtering, the unfiltered FFT completely lost the true 5 Hz peak, erroneously estimating the dominant frequency at 3.07 Hz. This miscalculation would cause an adaptive sampler to arbitrarily alter the polling rate, destroying data integrity.

**The Z-Score Filter Flaw (Mean-Based Masking):**
The Z-Score filter (3-sigma threshold) failed to detect anomalies efficiently, consistently producing a True Positive Rate (TPR) near `0.00`. A massive outlier heavily skews the mean and violently inflates the standard deviation of that specific window. Consequently, the anomaly masks itself by raising its own detection threshold.

**The Hampel Filter Superiority (Median Robustness):**
The Hampel filter demonstrated superior anomaly detection, frequently achieving a TPR of `1.00` at a 1% injection rate. Because it relies on the median rather than the mean, the central tendency remains anchored to the clean sine wave data regardless of extreme outliers. 

**The Computational Energy & Latency Trade-off:**
The superior accuracy of the Hampel filter incurs a massive computational penalty. Calculating a sliding mean for the Z-score took approximately `2,420 µs` (Window=15), whereas finding the median for the Hampel filter ($O(N \log N)$) took `14,000 µs`. Furthermore, utilizing large statistical windows (e.g., Window=31) drastically increases system latency, as the system must buffer 31 samples before the center point can be processed.