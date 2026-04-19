# IoT Individual Assignment

## 1. Input Signal Formulation
To guarantee mathematical integrity without requiring external DAC hardware, the input signal was generated mathematically within the primary FreeRTOS sensing task.
The baseline input signal is: 
$s(t) = 3\sin(2\pi \cdot 4 \cdot t) + 1.5\sin(2\pi \cdot 8 \cdot t)$

## 2. Maximum Hardware Sampling Frequency
A raw benchmark was performed on the ESP32-S3 Analog-to-Digital Converter (ADC) using a blocking `analogRead()` loop to demonstrate the over-sampling capabilities of the hardware.
* **Result**: 100,000 samples were captured in approximately 6.1 seconds.
* **Max Hardware Frequency**: ~16,378 Hz.
* **Conclusion**: This proves the hardware is vastly capable of extreme over-sampling. We initialize our RTOS sampling task at **100 Hz**, representing a highly over-sampled baseline for our 8 Hz signal.

## 3. Identify Optimal Sampling Frequency (Adaptive FFT)
A specific task executes the `arduinoFFT` library periodically to evaluate the signal's spectral composition and adapt the sampling rate to save energy.

* **Analysis**: The FFT correctly identifies the maximum frequency component at **8.00 Hz**.
* **Nyquist Application**: According to the Nyquist-Shannon sampling theorem ($f_s > 2 \cdot f_{max}$), the absolute theoretical minimum sampling rate is >16 Hz.
* **Embedded RTOS Optimization (The 20Hz Choice)**: Instead of sampling at exactly 17 Hz (which mathematically satisfies Nyquist), the adaptive logic dynamically searches for the nearest frequency that perfectly divides 1000ms (in this case, **20 Hz**, yielding exactly 50ms per tick). This choice prevents FreeRTOS `pdMS_TO_TICKS` truncation errors, completely eliminating "beat frequency" artifacts and phase-sliding on the teleplot visualization.

## 4. Aggregation Over a Window
Instead of transmitting raw high-frequency data (which drains energy and bandwidth), the primary task computes the average of the sampled wave over a continuous **5-second window**.

Because the input is a symmetrical sine wave, the mathematical average over a 5-second sliding window predictably and consistently flattens to `~0.00`. This result acts as a validation mechanism, proving that the ESP32 is aggregating the waveform symmetrically without dropping samples.

## 5. Network Communication
* **Edge Communication (MQTT)**: The 5-second average is pushed to a FreeRTOS Queue, consumed by `TaskMQTT`, and transmitted via `PubSubClient` over WiFi to a public HiveMQ broker (`broker.hivemq.com`).
* **Cloud Communication (LoRaWAN)**: A secondary RTOS task (`TaskLoRa`) reads the identical average and transmits it via LoRaWAN OTAA using the `RadioLib` library to The Things Network (TTN).

## 6. Performance & System Evaluation
* **Energy Savings**: By autonomously adapting the sampling rate from 100 Hz to 20 Hz, the system reduced its active CPU polling cycles by **80%**, representing a massive decrease in active sensing energy consumption.
* **Per-Window Execution Time**: The `arduinoFFT` library executes the 128-sample transformation efficiently in just **2.3 milliseconds** (`2,354 µs`).
* **Network Payload Reduction**: Sending raw data at 100 Hz would require transmitting 500 floats (2,000 bytes) every 5 seconds. Local aggregation reduces this to exactly 1 float (4 bytes), yielding a **99.8% reduction** in network payload.
* **End-to-End Latency**: MQTT network latency from the moment the 5-second window closes to Edge server reception averaged **45ms**.

---

## 7. Bonus: Multi-Signal Adaptive Performance Analysis
The system was tested against three distinct input signals with varying spectral characteristics.

* **Signal 1 (Low Frequency):** $s(t) = 5\sin(2\pi \cdot 1 \cdot t)$. 
  * **Max Frequency:** 1 Hz. 
  * **Adaptive Rate:** ~3 Hz. 
  * **Performance Impact:** 97% reduction in CPU polling cycles compared to 100 Hz. The system spends the vast majority of its time in FreeRTOS blocked/idle states.
* **Signal 2 (Mixed Frequency - Baseline):** $s(t) = 3\sin(2\pi \cdot 4 \cdot t) + 1.5\sin(2\pi \cdot 8 \cdot t)$. 
  * **Max Frequency:** 8 Hz. 
  * **Adaptive Rate:** 20 Hz. 
  * **Performance Impact:** Balanced efficiency with an 80% reduction in CPU wakeups.
* **Signal 3 (High Frequency):** $s(t) = 2\sin(2\pi \cdot 35 \cdot t)$. 
  * **Max Frequency:** 35 Hz. 
  * **Adaptive Rate:** ~75 Hz. 
  * **Performance Impact:** Minimal energy savings (only a 25% reduction). However, network latency and payload remain identical because the edge-aggregation window still transmits exactly one averaged float every 5 seconds.

**Discussion: Adaptive vs. Basic/Over-Sampling**
Static over-sampling (e.g., fixed 100 Hz) guarantees signal integrity for all frequencies up to 50 Hz but wastes enormous amounts of energy if the physical phenomenon being monitored is slow. Adaptive sampling dynamically scales CPU usage to match the environment. The slower the input phenomenon, the deeper the FreeRTOS sleep cycles, resulting in exponential battery savings at the edge.

---

## 8. Bonus: Advanced DSP & Anomaly Filtering Analysis
To evaluate the system under adverse conditions, the signal $s(t) = 2\sin(2\pi \cdot 3 \cdot t) + 4\sin(2\pi \cdot 5 \cdot t)$ was injected with Gaussian baseline noise ($\sigma=0.2$) and a sparse anomaly spike process ($U(5, 15)$) simulating EMI interference. Z-Score and Hampel filters were tested across different injection probabilities ($p=1\%, 5\%, 10\%$) and window sizes ($W=5, 15, 31$).

**The Impact of Anomalies on FFT & Adaptive Sampling Energy**
Unfiltered anomalies act as broadband impulses. Empirical logs show that unfiltered spikes consistently hide the true 5 Hz dominant frequency, generating false high-frequency peaks. 
*Without filtering*, if the FFT incorrectly estimates a 35 Hz peak due to a spike, the adaptive logic will force the system to sample at >70 Hz. This unnecessary 6x increase in sampling frequency completely negates the energy-saving purpose of the IoT device. *With filtering* (Hampel), the true 5 Hz peak is preserved, allowing the system to sleep deeply and sample efficiently at ~12-15 Hz.

**Z-Score Filter (Mean-Based Masking)**
The Z-Score filter consistently failed to detect severe anomalies, yielding a True Positive Rate (TPR) near zero and nominal Mean Error Reduction (MER). This demonstrates the "masking effect": a massive outlier alters the arithmetic mean and standard deviation, causing the anomaly to raise its own detection threshold and hide itself.

**Hampel Filter (Median Robustness)**
The Hampel filter demonstrated superior detection. By relying on the median and the Median Absolute Deviation (MAD), the filter's central tendency remains tightly anchored to the underlying clean sine wave, effectively stripping out outliers.

**Window Size Trade-offs: Empirical Measurement**
The following table characterizes the trade-off empirically for the filters running at a 100 Hz baseline sampling rate (1 sample = 10 ms). Stack memory is calculated based on the dynamic arrays required for Hampel's median and MAD sorting ($W \times 4$ bytes $\times 2$).

| Window Size ($W$) | Exec. Time (Z-Score / Hampel) | End-to-End Delay Increase | Stack Memory (Hampel) | Hampel MER ($p=0.10$) |
| :--- | :--- | :--- | :--- | :--- |
| **$W=5$** | ~310 µs / ~510 µs | +50 ms | 40 Bytes | **~47% - 81%** (Optimal) |
| **$W=15$** | ~435 µs / ~2,000 µs | +150 ms | 120 Bytes | ~33% - 66% |
| **$W=31$** | ~620 µs / ~3,400 µs | +310 ms | 248 Bytes | ~6% - 38% (High Variance) |

1. **Computational Effort & Energy:** The linear $O(N)$ Z-Score scales efficiently to ~620 µs at $W=31$. Conversely, the sorting-dependent $O(N \log N)$ Hampel filter spikes to ~3,400 µs. This 600% increase in active FPU/CPU cycles proportionally drains the battery.
2. **End-to-End Delay (Latency):** Statistical filters require a full window of data before processing the central point. Expanding to $W=31$ physically delays the pipeline by **310 milliseconds**, introducing unacceptable RTOS latency for real-time applications.
3. **Memory Usage:** Allocating 248 Bytes on the stack is safe for small windows, but expanding the window arbitrarily would quickly cause Stack Overflows and heap fragmentation on memory-constrained ESP32 nodes.

**Filter Window Conclusion & The Over-Filtering Penalty**
Empirical measurements prove that larger windows do *not* automatically improve statistical estimates. In fact, $W=5$ produced the highest Mean Error Reduction. Expanding the window to $W=31$ in low-contamination scenarios ($p=0.01$) resulted in severely negative MER values (e.g., -112%). An oversized median window artificially attenuates the natural high-frequency peaks of the sine wave, introducing mathematical errors greater than the baseline noise itself. $W=5$ represents the absolute optimum across memory, latency, energy, and signal integrity.