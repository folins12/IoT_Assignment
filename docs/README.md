# IoT Individual Assignment

## 1. Input Signal Formulation
To guarantee mathematical integrity without requiring external DAC hardware, the input signal was generated mathematically within the primary FreeRTOS sensing task.
The base signal takes the form: 
$s(t) = 3\sin(2\pi \cdot 4 \cdot t) + 1.5\sin(2\pi \cdot 8 \cdot t)$

The maximum inherent frequency present in this signal is **8 Hz**.

## 2. Maximum Hardware Sampling Frequency
A raw benchmark was performed on the ESP32-S3 Analog-to-Digital Converter (ADC) using a blocking `analogRead()` loop in a dedicated test script to demonstrate the hardware's native over-sampling capabilities.
* **Result**: 100,000 samples were captured in approximately 6.1 seconds.
* **Max Hardware Frequency**: ~16,378 Hz.
* **Conclusion**: This proves the hardware is vastly capable of extreme over-sampling. We initialize our RTOS sampling task at **100 Hz**, representing a highly over-sampled baseline for our 8 Hz signal.

## 3. Identify Optimal Sampling Frequency (Adaptive FFT)
A dedicated FreeRTOS task executes the `arduinoFFT` library periodically to evaluate the signal's spectral composition and adapt the sampling rate to save energy.

* **Analysis**: The FFT correctly identifies the maximum frequency component at **8.00 Hz**.
* **Nyquist Application**: According to the Nyquist-Shannon sampling theorem ($f_s > 2 \cdot f_{max}$), the absolute theoretical minimum sampling rate is >16 Hz.
* **Embedded RTOS Optimization (The 20Hz Choice)**: Instead of sampling at exactly 17 Hz (which mathematically satisfies Nyquist), the adaptive logic dynamically searches for the nearest frequency that perfectly divides 1000ms (in this case, **20 Hz**, yielding exactly 50ms per tick). This deliberate engineering choice prevents FreeRTOS `pdMS_TO_TICKS` truncation errors, completely eliminating "beat frequency" artifacts and phase-sliding on the teleplot visualization.

## 4. Aggregation Over a Window
Instead of transmitting raw high-frequency data (which drains energy and bandwidth), the primary task computes the arithmetic mean (average) of the sampled wave over a continuous **5-second window**.

*Note on Data*: Because the input is a symmetrical sine wave, the mathematical average over a 5-second sliding window predictably and consistently flattens to `~0.00`. This result acts as a validation mechanism, proving that the ESP32 is aggregating the waveform symmetrically without dropping samples.

## 5. Network Communication
* **Edge Communication (MQTT)**: The 5-second average is pushed to a FreeRTOS Queue, consumed by `TaskMQTT`, and transmitted via `PubSubClient` over WiFi to a public HiveMQ broker (`broker.hivemq.com`).
* **Cloud Communication (LoRaWAN)**: A secondary RTOS task (`TaskLoRa`) reads the identical average and transmits it via LoRaWAN OTAA using the `RadioLib` library to The Things Network (TTN).

## 6. Performance & System Evaluation
* **Energy Savings**: By autonomously adapting the sampling rate from 100 Hz to 20 Hz, the system reduced its active CPU polling cycles by **80%**, representing a massive decrease in active sensing energy consumption.
* **Per-Window Execution Time**: The `arduinoFFT` library executes the 128-sample transformation efficiently in just **2.3 milliseconds** (`2,354 µs`).
* **Network Payload Reduction**: Sending raw data at 100 Hz would require transmitting 500 floats (2,000 bytes) every 5 seconds. Local aggregation reduces this to exactly 1 float (4 bytes), yielding a **99.8% reduction** in network payload.
* **End-to-End Latency**: MQTT network latency from the moment the 5-second window closes to Edge server reception averaged **45ms**.

---

## 7. Bonus: Advanced DSP & Anomaly Filtering Analysis
To evaluate the system under harsh real-world conditions, an alternative signal was tested including Gaussian baseline noise ($\sigma=0.2$) and a sparse anomaly spike process ($U(5, 15)$) simulating EMI interference. Two anomaly-aware filters (Z-score and Hampel) were evaluated across varying injection probabilities ($p=0.01, 0.05, 0.10$).

**The Impact of Anomalies on FFT Estimation**
Transient hardware faults act as impulses, introducing massive high-frequency noise across the spectrum. Without filtering, the FFT completely lost the true peak, erroneously estimating the dominant frequency.

**The Z-Score Filter Flaw (Mean-Based Masking)**
The Z-Score filter (3-sigma threshold) failed to detect severe anomalies efficiently. A massive outlier heavily skews the mean and violently inflates the standard deviation of its window, causing the anomaly to "mask" itself.

**The Hampel Filter Superiority (Median Robustness)**
The Hampel filter demonstrated superior anomaly detection, achieving a high True Positive Rate (TPR). Because it relies on the median absolute deviation (MAD) rather than standard deviation, the central tendency remains anchored to the clean sine wave data regardless of extreme outliers.

**The Computational Energy & Latency Trade-off**
The superior accuracy of the Hampel filter incurs a massive computational penalty. Calculating a sliding mean for the Z-score was significantly faster than finding the median for the Hampel filter, which requires sorting. Furthermore, utilizing large statistical windows increases system latency, as the edge node must buffer more samples before processing.