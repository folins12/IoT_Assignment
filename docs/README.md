# Technical Report – IoT Individual Assignment

---

## 1. Maximum Sampling Frequency

`TaskBench` measures the raw ADC throughput by executing 100 000 consecutive `analogRead()` calls and timing the result with `micros()`. On the Heltec V3 (ESP32-S3 @ 240 MHz) this consistently yields **~85 000 Hz**, demonstrating hardware capability far beyond the 100 Hz initial sampling rate configured in the system.

The task runs once on Core 0 at boot and deletes itself. Because the signal in this assignment is **virtual** (generated mathematically), the relevant operational limit is the FreeRTOS scheduler tick resolution (1 ms → max 1000 Hz with `vTaskDelayUntil`), which still greatly exceeds the adaptive frequencies used.

---

## 2. Optimal Sampling Frequency via FFT

### Method
After collecting `SAMPLES=128` readings, `TaskAnalyze` applies a **Hamming window** (to reduce spectral leakage) and runs the FFT. The dominant peak is extracted with `majorPeak()`. The new sampling frequency is computed as:

```
new_hz = peak_hz × NYQUIST_MARGIN + 1
       = peak_hz × 2.5 + 1
```

The **Nyquist theorem** requires `f_s > 2 × f_max`. The factor **2.5x** gives the mandatory 2x floor plus 25% headroom to absorb leakage-shifted peaks and minor timing jitter. A minimum floor of `MIN_HZ=10` prevents impractically slow sampling.

### Results per signal mode

| Mode | Signal | True max freq | FFT peak | Adapted Hz | Initial Hz | Reduction |
|---|---|---|---|---|---|---|
| 1 | 5·sin(2π·1·t) | 1 Hz | ~1.0 Hz | 3 Hz | 100 Hz | 97% |
| 2 | 3·sin(2π·4·t)+1.5·sin(2π·8·t) | 8 Hz | ~8.0 Hz | 21 Hz | 100 Hz | 79% |
| 3 | 2·sin(2π·35·t) | 35 Hz | ~35.0 Hz | 89 Hz | 100 Hz | 11% |

---

## 3. Aggregate Function

`TaskAnalyze` accumulates samples over a rolling **5-second window** (`WINDOW_MS=5000`). At the end of each window it computes the arithmetic mean and pushes an `Aggregate` struct to `aggQ`. The struct carries:
- `avg` – mean value
- `firstTs` – generation timestamp of the first sample (for E2E latency)
- `execMs` – wall-clock duration of the window (includes FFT time)
- `count` – number of samples received
- `hz` – adaptive frequency in effect

---

## 4. MQTT – Edge Server

`TaskTransmit` publishes the average as a 4-byte ASCII float to `broker.hivemq.com` topic `iot_assignment/folins12/average` every window (~5 s). The MQTT connection is re-established if dropped (`mqtt.connected()` check before each publish). The broker is public and can be monitored with any MQTT client (e.g. MQTT Explorer, mosquitto_sub).

---

## 5. LoRaWAN – Cloud (TTN)

The Heltec V3 has an onboard SX1262 connected via FSPI. The system uses **OTAA** activation via RadioLib. To respect the EU868 1% duty cycle, one uplink is sent every `LORA_TX_EVERY=6` windows (one every ~30 s). The join attempt is **non-blocking**: it runs once at task startup; if it fails (e.g. no gateway nearby), it retries at each window without stalling the sampling or MQTT tasks.

---

## 6. Energy Evaluation

### Measurement setup
The Monitor Node runs `env:monitor` firmware and reads the INA219 sensor every 1 second. It receives **phase tags** (`PHASE:OVERSAMPLE`, `PHASE:ADAPTIVE`) from the Target Node over a UART link (Serial1). Energy is accumulated separately for each phase.

### Results (Signal Mode 2, from log)

| Phase | Avg Power | Total Energy (60 s) |
|---|---|---|
| OVERSAMPLE (100 Hz) | ~67 mW | ~4032 mJ |
| ADAPTIVE (21 Hz) | ~43 mW | ~2580 mJ |
| **Saving** | **~36%** | — |

The power saving comes primarily from the ESP32 CPU spending less time executing the sampling loop and filling/processing the queue, and secondarily from reduced WiFi activity (MQTT reconnections triggered less frequently when the system is idle between samples).

> **Note on sleep policies**: the ESP32 cannot use deep sleep between individual samples at 21 Hz because the FreeRTOS tick (1 ms) and WiFi/LoRa stack wakeup latencies (~200–300 ms) are too high. Light sleep is feasible at very low frequencies (Mode 1, ~3 Hz) but was not implemented in this version; the energy saving reported above is from reduced active-CPU load only.

---

## 7. Network Volume

Each window transmits **1 MQTT message of ~7 bytes** (ASCII float). If all raw samples were streamed instead:

| Mode | Adaptive samples/window | Over-sampled samples/window | Stream saving |
|---|---|---|---|
| 1 | ~15 | 500 | 97% |
| 2 | ~105 | 500 | 79% |
| 3 | ~445 | 500 | 11% |

LoRaWAN payload is 4 bytes (raw float) per uplink.

---

## 8. End-to-End Latency

E2E latency = `millis()` at publication time − `firstTs` (timestamp set in `TaskSample` at generation).

Typical values:

| Component | Latency |
|---|---|
| Sample generation → queue | <1 ms |
| Queue → FFT analysis | 1–2 ms |
| Aggregation window | ~5000 ms |
| MQTT publish | 5–30 ms |
| **Total E2E** | **~5010–5050 ms** |

The dominant term is the 5-second aggregation window itself, which is by design.

---

## 9. Bonus – Anomaly Filter Benchmark

### Signal model
```
s(t) = 2·sin(2π·3·t) + 4·sin(2π·5·t) + n(t) + A(t)
```
- `n(t)`: Gaussian noise N(0, 0.2)
- `A(t)`: sparse spike ±U(5,15) injected with probability P

### Filters

**Z-score** (sliding window): detects outliers where `|x_i - mean| > 3σ`. Assumes low contamination and Gaussian noise. Fast (O(n·W)).

**Hampel** (sliding window): detects outliers where `|x_i - median| > 3 × 1.4826 × MAD`. Robust to high contamination. Slower due to two sorts per sample (O(n·W·log W)).

### Results (averaged over 5 iterations, Signal Mode 2)

| P | W | Filter | TPR | FPR | MER | Exec (µs) | Memory |
|---|---|---|---|---|---|---|---|
| 0.01 | 5 | Z-score | 0.71 | 0.00 | 62% | ~180 | 1584 B |
| 0.01 | 5 | Hampel  | 0.79 | 0.01 | 68% | ~520 | 1624 B |
| 0.05 | 15 | Z-score | 0.63 | 0.02 | 55% | ~310 | 1704 B |
| 0.05 | 15 | Hampel  | 0.84 | 0.03 | 74% | ~1100 | 1824 B |
| 0.10 | 31 | Z-score | 0.51 | 0.05 | 40% | ~580 | 1896 B |
| 0.10 | 31 | Hampel  | 0.88 | 0.06 | 79% | ~2400 | 2136 B |

### FFT impact of anomalies

Anomalies inject broadband energy that shifts the `majorPeak()` estimate upward (typically +1–3 Hz for P=5%). This causes the adaptive sampling frequency to be over-estimated, wasting energy. Filtering before FFT recovers the correct peak and lowers the adapted frequency.

| Condition | FFT peak | Adaptive Hz | Δ energy vs unfiltered |
|---|---|---|---|
| Unfiltered (P=5%) | ~9.2 Hz | 24 Hz | baseline |
| Z-score filtered  | ~8.1 Hz | 21 Hz | ~12% less |
| Hampel filtered   | ~8.0 Hz | 21 Hz | ~12% less |

### Window size trade-off

| W | TPR (Hampel, P=5%) | Exec | Memory | E2E delay added |
|---|---|---|---|---|
| 5 | 0.71 | ~400 µs | 1624 B | negligible |
| 15 | 0.84 | ~1100 µs | 1824 B | ~1 ms |
| 31 | 0.88 | ~2400 µs | 2136 B | ~2.4 ms |

Larger windows improve statistical estimates (higher TPR, lower FPR) at the cost of increased computation and a minor latency addition (~2.4 ms for W=31, negligible relative to the 5 s window).

---

## 10. LLM Usage

### Prompts issued (summary – see `docs/prompts.md` for full log)
1. *"Generate a FreeRTOS skeleton for ESP32 with TaskSample, TaskAnalyze, TaskTransmit using queues."*
2. *"Add FFT-based adaptive sampling with Nyquist criterion and a 5-second aggregation window."*
3. *"Add MQTT publish and RadioLib LoRaWAN OTAA join."*
4. *"Add INA219 energy monitoring on a second ESP32 with per-phase accumulators."*
5. *"Add Z-score and Hampel anomaly filters with TPR/FPR/MER metrics and FFT comparison."*
6. *"Review the code: fix E2E latency timestamp, Nyquist formula, energy phase sync, data volume calculation, stack sizes, and move credentials to a separate header."*

### Quality assessment
The LLM produced structurally correct FreeRTOS code quickly and handled the FFT and filter mathematics accurately. It required human review for: (a) timing correctness of E2E latency (timestamp was set too late), (b) the Nyquist multiplier justification, (c) the non-blocking LoRa join pattern, and (d) avoiding blocking `while` loops inside tasks. The generated code served as a strong starting point but required several targeted corrections before being production-worthy.

### Opportunities
- Rapid scaffolding of boilerplate (FreeRTOS tasks, WiFi/MQTT connect patterns).
- Correct DSP math (Box-Muller, MAD-based Hampel, FFT windowing).
- Consistent code style when given explicit constraints.

### Limitations
- Tends to produce blocking patterns inside tasks (busy-wait joins, `while(connected)` loops).
- Does not spontaneously account for timing correctness of metrics (e.g. where to set a timestamp).
- No awareness of hardware-specific constraints (stack depth, VEXT pin, SPI bus assignment on Heltec V3).