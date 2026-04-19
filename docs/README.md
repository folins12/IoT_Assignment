# IoT Individual Assignment

## 1. Input Signal Formulation
To guarantee mathematical integrity without requiring external DAC hardware, the input signal was generated mathematically within the primary FreeRTOS sensing task.
The input signal is: 
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

## 7. Bonus: Advanced DSP & Anomaly Filtering Analysis
To evaluate the system under adverse conditions, the signal $s(t) = 2\sin(2\pi \cdot 3 \cdot t) + 4\sin(2\pi \cdot 5 \cdot t)$ was injected with Gaussian baseline noise ($\sigma=0.2$) and a sparse anomaly spike process ($U(5, 15)$) simulating EMI interference. Z-Score and Hampel filters were tested across different injection probabilities ($p=1$%$, 5$%$, 10$%) and window sizes ($W=5, 15, 31$).

**The Impact of Anomalies on FFT Estimation**
Unfiltered anomalies destroy the FFT's ability to identify the true dominant frequency, cause false peaks alter the adaptive sampling logic.

**Z-Score Filter**
Despite executing rapidly, the Z-Score filter consistently failed to detect severe anomalies, yielding a True Positive Rate (TPR) near zero and 0.0% Mean Error Reduction (MER). This demonstrates the "masking effect": a massive outlier alters the arithmetic mean and standard deviation, causing the anomaly to raise its own detection threshold and hide itself.

**Hampel Filter**
The Hampel filter demonstrated superior detection. By relying on the median and the Median Absolute Deviation (MAD), the filter's central tendency remains tightly anchored to the underlying clean sine wave.

**Algorithmic Complexity on Hardware**
The linear $O(N)$ Z-Score executed in just **~600 µs** at $W=31$. In contrast, the sorting-dependent $O(N \log N)$ Hampel filter required **~3,400 µs** for the same window. 

**Filter Window Optimization & The Over-Filtering Penalty**
A narrow window ($W=5$) produced the highest MER (clearing over **70%** of corruption at $p=0.10$) while executing in just **~500 µs**. 

Expanding the window to $W=31$ in low-contamination scenarios ($p=0.01$) resulted in **severely negative MER values** (e.g., -232%). A median window that is too large attenuates and distorts the peaks of the high-frequency sine wave. This causes false positives and mathematical errors that are greater than those of raw noise.