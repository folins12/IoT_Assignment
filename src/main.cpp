#include <Arduino.h>
#include <arduinoFFT.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <RadioLib.h>
#include <algorithm>
#include "credentials.h"

// CONFIGURATION
//   1 -> Single tone:  5.0*sin(2*pi*1*t)
//   2 -> Two tones:    3.0*sin(2*pi*4*t) + 1.5*sin(2*pi*8*t)  [max freq = 8 Hz]
//   3 -> High freq:    2.0*sin(2*pi*35*t)
#define SIGNAL_MODE       2

// Set to 1 to run the bonus anomaly-filter benchmark (Z-score vs Hampel)
#define RUN_BONUS         1

// Sampling parameters
#define INITIAL_SAMPLING_HZ  100     // Starting (over-sampled) frequency
#define SAMPLES              128     // FFT window size (power of 2)
#define WINDOW_MS            5000    // Aggregation window duration [ms]

#define NYQUIST_MARGIN       2.5f
#define MIN_ADAPTIVE_HZ      10      // Floor to avoid unrealistically slow sampling

// LoRa duty-cycle: transmit one uplink every LORA_TX_EVERY windows
#define LORA_TX_EVERY        6

// INA219 sync: this string is sent over Serial1 to the Monitor node
// so it can annotate its power log with the current sampling phase.
#define SYNC_TAG_OVERSAMPLE  "PHASE:OVERSAMPLE"
#define SYNC_TAG_ADAPTIVE    "PHASE:ADAPTIVE"

// HELTEC V3 LORA PINS
#define LORA_SCK   9
#define LORA_MISO  11
#define LORA_MOSI  10
#define LORA_CS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13

// DATA STRUCTURES
struct SensorSample {
    float         value;
    unsigned long generatedAt_ms;  // set in TaskSample for true E2E latency
};

struct AggregatedData {
    float         average;
    unsigned long windowStart_ms;  // first sample timestamp in this window
    unsigned long windowEnd_ms;    // last sample timestamp
    unsigned long windowExec_ms;   // total window wall-clock duration
    int           sampleCount;     // actual samples in this window
    int           samplingHz;      // adaptive frequency used for this window
};

// GLOBAL STATE
QueueHandle_t sampleQueue;
QueueHandle_t aggregateQueue;

volatile int   g_currentDelayMs   = 1000 / INITIAL_SAMPLING_HZ;
volatile int   g_currentHz        = INITIAL_SAMPLING_HZ;
volatile bool  g_adaptiveLocked   = false;  // true once FFT has adapted at least once

// FFT buffers (used exclusively inside TaskAnalyze, no mutex needed)
double vReal[SAMPLES], vImag[SAMPLES];
ArduinoFFT<double> FFT(vReal, vImag, SAMPLES, INITIAL_SAMPLING_HZ);

// NETWORK OBJECTS
SPIClass    radioSPI(FSPI);
SX1262      radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, radioSPI);
LoRaWANNode node(&radio, &EU868);
WiFiClient  espClient;
PubSubClient mqtt(espClient);

// UTILITY FUNCTIONS

// Box-Muller transform: generate one Gaussian sample N(mu, sigma)
static float gaussianNoise(float mu, float sigma) {
    float u1 = max(0.0001f, (float)random(10000) / 10000.0f);
    float u2 = max(0.0001f, (float)random(10000) / 10000.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * PI * u2) * sigma + mu;
}

// Mean Error Reduction [%]: how much the filter reduces absolute error vs clean
static float meanErrorReduction(const float* filtered, const float* raw,
                                 const float* clean, int n) {
    float rawErr = 0.0f, filtErr = 0.0f;
    for (int i = 0; i < n; i++) {
        rawErr  += fabsf(raw[i]      - clean[i]);
        filtErr += fabsf(filtered[i] - clean[i]);
    }
    if (rawErr < 1e-9f) return 0.0f;
    return (rawErr - filtErr) / rawErr * 100.0f;
}

// Run FFT on an arbitrary float buffer and return the dominant frequency [Hz].
static double dominantFreq(const float* sig, int n, int samplingHz) {
    double tReal[SAMPLES], tImag[SAMPLES];
    for (int i = 0; i < n; i++) { tReal[i] = sig[i]; tImag[i] = 0.0; }
    ArduinoFFT<double> tmp(tReal, tImag, n, (double)samplingHz);
    tmp.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    tmp.compute(FFT_FORWARD);
    tmp.complexToMagnitude();
    return tmp.majorPeak();
}

// TASK 1: MAX FREQUENCY BENCHMARK
void TaskMaxFreqBenchmark(void* pv) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    Serial.println("\n[BENCH] Starting hardware ADC benchmark (100 000 samples)...");
    const long N = 100000L;
    unsigned long t0 = micros();
    for (long i = 0; i < N; i++) {
        volatile int v = analogRead(4);
        (void)v;
    }
    unsigned long elapsed_us = micros() - t0;
    float maxHz = (float)N / (elapsed_us / 1e6f);

    Serial.printf("[BENCH] ADC elapsed: %lu us  ->  Max hardware freq: %.2f Hz\n",
                  elapsed_us, maxHz);
    Serial.printf("[BENCH] Configured initial sampling freq: %d Hz  "
                  "(well below hardware limit)\n", INITIAL_SAMPLING_HZ);

    vTaskDelete(NULL);
}

// TASK 2: SIGNAL SAMPLING
void TaskSample(void* pv) {
    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        unsigned long now_ms = millis();
        float t = now_ms / 1000.0f;

        float value = 0.0f;
        #if SIGNAL_MODE == 1
            // Single tone, max freq = 1 Hz -> optimal sampling ~2-3 Hz
            value = 5.0f * sinf(2.0f * PI * 1.0f * t);
        #elif SIGNAL_MODE == 2
            // Two tones, max freq = 8 Hz -> optimal sampling ~20 Hz
            value = 3.0f * sinf(2.0f * PI * 4.0f * t)
                  + 1.5f * sinf(2.0f * PI * 8.0f * t);
        #elif SIGNAL_MODE == 3
            // High-frequency tone, max freq = 35 Hz -> optimal sampling ~88 Hz
            value = 2.0f * sinf(2.0f * PI * 35.0f * t);
        #endif

        SensorSample s = { value, now_ms };
        xQueueSend(sampleQueue, &s, 0);

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(g_currentDelayMs));
    }
}

// TASK 3: FFT ANALYSIS & WINDOWED AGGREGATION
void TaskAnalyze(void* pv) {
    float         windowSum      = 0.0f;
    int           windowCount    = 0;
    unsigned long windowStart_ms = millis();
    unsigned long windowFirstTs  = millis();   // timestamp of first sample in window
    unsigned long windowLastTs   = millis();
    unsigned long windowWallStart = millis();  // wall-clock start for exec time

    int  fftCount = 0;
    bool phaseTagSent = false;   // have we sent the oversample phase tag yet?

    for (;;) {
        SensorSample s;
        if (xQueueReceive(sampleQueue, &s, portMAX_DELAY) != pdPASS) continue;

        // --- Accumulate for window average ---
        if (windowCount == 0) {
            windowFirstTs  = s.generatedAt_ms;
            windowWallStart = millis();
        }
        windowSum       += s.value;
        windowLastTs     = s.generatedAt_ms;
        windowCount++;

        // Send initial phase tag to Monitor node once at startup
        if (!phaseTagSent) {
            Serial1.println(SYNC_TAG_OVERSAMPLE);
            phaseTagSent = true;
        }

        // --- Fill FFT buffer ---
        if (fftCount < SAMPLES) {
            vReal[fftCount] = s.value;
            vImag[fftCount] = 0.0;
            fftCount++;
        }

        // --- Run FFT when buffer is full ---
        if (fftCount == SAMPLES) {
            unsigned long fftStart = micros();

            FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
            FFT.compute(FFT_FORWARD);
            FFT.complexToMagnitude();

            unsigned long fftExec_us = micros() - fftStart;
            double peakHz = FFT.majorPeak();

            // Nyquist: new_freq >= 2 * peak.
            int newHz = max((int)(peakHz * NYQUIST_MARGIN) + 1, MIN_ADAPTIVE_HZ);

            if (newHz != g_currentHz) {
                int oldHz = g_currentHz;
                g_currentHz      = newHz;
                g_currentDelayMs = 1000 / newHz;
                g_adaptiveLocked = true;

                Serial.printf("\n[FFT] Peak: %.2f Hz  |  Adapting: %d Hz -> %d Hz"
                              "  (Nyquist margin: %.1fx)"
                              "  |  FFT exec: %lu us\n",
                              peakHz, oldHz, newHz, NYQUIST_MARGIN, fftExec_us);

                // Notify Monitor node that the phase has changed
                Serial1.println(SYNC_TAG_ADAPTIVE);
            } else {
                Serial.printf("[FFT] Peak: %.2f Hz  |  Freq unchanged: %d Hz"
                              "  |  FFT exec: %lu us\n",
                              peakHz, g_currentHz, fftExec_us);
            }

            fftCount = 0;
        }

        // --- Flush window every WINDOW_MS ---
        if (millis() - windowStart_ms >= WINDOW_MS) {
            unsigned long windowExec_ms = millis() - windowWallStart;

            AggregatedData agg;
            agg.average        = (windowCount > 0) ? windowSum / windowCount : 0.0f;
            agg.windowStart_ms = windowFirstTs;
            agg.windowEnd_ms   = windowLastTs;
            agg.windowExec_ms  = windowExec_ms;
            agg.sampleCount    = windowCount;
            agg.samplingHz     = g_currentHz;

            xQueueSend(aggregateQueue, &agg, 0);

            // Reset window state
            windowSum       = 0.0f;
            windowCount     = 0;
            windowStart_ms  = millis();
        }
    }
}

// TASK 4: TRANSMIT (MQTT + LoRaWAN)
void TaskTransmit(void* pv) {
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);

    Serial.println("\n[LORA] Initialising radio...");
    radioSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

    bool loraReady = false;
    if (radio.begin() == RADIOLIB_ERR_NONE) {
        radio.setDio2AsRfSwitch(true);
        radio.setTCXO(1.8);

        uint64_t joinEUI = LORAWAN_JOIN_EUI;
        uint64_t devEUI  = LORAWAN_DEV_EUI;
        uint8_t  appKey[] = LORAWAN_APP_KEY;

        node.beginOTAA(joinEUI, devEUI, appKey, appKey);

        Serial.println("[LORA] Attempting OTAA join...");
        while (!node.isActivated()) {
            int state = node.activateOTAA();
            if (node.isActivated()) {
                Serial.println("[LORA] OTAA join SUCCESS – connected to TTN.");
                loraReady = true;
            } else {
                Serial.printf("[LORA] Join failed (code %d), retrying in 10 s...\n", state);
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        }
    } else {
        Serial.println("[LORA] Radio init failed – LoRaWAN disabled.");
    }

    //   INITIAL_SAMPLING_HZ samples/sec * 5 sec window * 4 bytes/float
    const int overSampledBytes = INITIAL_SAMPLING_HZ * (WINDOW_MS / 1000) * sizeof(float);

    int windowIndex = 0;

    for (;;) {
        AggregatedData agg;
        if (xQueueReceive(aggregateQueue, &agg, portMAX_DELAY) != pdPASS) continue;

        windowIndex++;
        unsigned long receiveTime_ms = millis();

        // Performance metrics
        // E2E latency: from moment the first sample in this window was generated (in TaskSample) to now (received by edge/cloud layer).
        unsigned long e2eLatency_ms = receiveTime_ms - agg.windowStart_ms;

        // Adaptive samples per window vs over-sampled baseline
        int adaptiveSamples     = agg.sampleCount;
        int overSampledSamples  = INITIAL_SAMPLING_HZ * (WINDOW_MS / 1000);
        int adaptiveStreamBytes    = adaptiveSamples    * sizeof(float);
        int overSampledStreamBytes = overSampledSamples * sizeof(float);
        float dataSavingPct = 100.0f * (1.0f - (float)adaptiveStreamBytes
                                                / (float)overSampledStreamBytes);

        Serial.println("\n==================================================");
        Serial.printf("[WINDOW #%d]\n", windowIndex);
        Serial.printf("  Average value      : %.4f\n", agg.average);
        Serial.printf("  Sampling freq      : %d Hz  (initial: %d Hz)\n",
                      agg.samplingHz, INITIAL_SAMPLING_HZ);
        Serial.printf("  Samples in window  : %d  (over-sampled baseline: %d)\n",
                      adaptiveSamples, overSampledSamples);
        Serial.printf("  Window exec time   : %lu ms\n", agg.windowExec_ms);
        Serial.printf("  E2E latency        : %lu ms  "
                      "(from sample generation to TX layer)\n", e2eLatency_ms);
        Serial.printf("  Stream bytes saved : %d vs %d bytes  (%.1f%% reduction)\n",
                      adaptiveStreamBytes, overSampledStreamBytes, dataSavingPct);

        // MQTT – edge server
        if (WiFi.status() == WL_CONNECTED) {
            if (!mqtt.connected()) {
                mqtt.connect(MQTT_CLIENT_ID);
            }
            mqtt.loop();

            char msg[32];
            snprintf(msg, sizeof(msg), "%.4f", agg.average);
            bool published = mqtt.publish(MQTT_TOPIC, msg);

            Serial.printf("  [MQTT] Published '%s' to '%s'  -> %s\n",
                          msg, MQTT_TOPIC, published ? "OK" : "FAILED");
        } else {
            Serial.println("  [MQTT] WiFi not connected – skipped.");
        }

        // LoRaWAN – cloud (TTN), duty-cycle limited
        if (loraReady && node.isActivated()) {
            if (windowIndex % LORA_TX_EVERY == 0) {
                int16_t txState = node.sendReceive(
                    (uint8_t*)&agg.average, sizeof(agg.average));

                bool ok = (txState == RADIOLIB_ERR_NONE      ||
                           txState == RADIOLIB_ERR_RX_TIMEOUT ||
                           txState == -1116);
                Serial.printf("  [LORA] Uplink %s (code %d)\n",
                              ok ? "SUCCESS" : "FAILED", txState);
            } else {
                Serial.printf("  [LORA] Duty-cycle pause (%d/%d)\n",
                              windowIndex % LORA_TX_EVERY, LORA_TX_EVERY);
            }
        } else {
            Serial.println("  [LORA] Not activated – skipped.");
        }

        Serial.println("==================================================\n");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// TASK 5 (BONUS): ANOMALY FILTER BENCHMARK
#if RUN_BONUS == 1

#define MAX_FILTER_WIN 31

void TaskBonusSweep(void* pv) {
    vTaskDelay(pdMS_TO_TICKS(20000));

    const float probabilities[] = { 0.01f, 0.05f, 0.10f };
    const int   windows[]       = { 5, 15, MAX_FILTER_WIN };
    const int   nProbs    = sizeof(probabilities) / sizeof(probabilities[0]);
    const int   nWindows  = sizeof(windows)       / sizeof(windows[0]);
    const int   ITERS     = 5;       // repetitions per (P, W) pair

    // Static buffers to stay off the stack
    static float cleanSig[SAMPLES], rawSig[SAMPLES], filtSig[SAMPLES];
    static bool  isAnom[SAMPLES];
    static float sortBuf[MAX_FILTER_WIN];

    for (;;) {
        for (int pi = 0; pi < nProbs; pi++) {
            float P = probabilities[pi];

            for (int wi = 0; wi < nWindows; wi++) {
                int W = windows[wi];

                float z_tprAcc = 0, z_fprAcc = 0, z_merAcc = 0;
                float h_tprAcc = 0, h_fprAcc = 0, h_merAcc = 0;
                unsigned long z_timeAcc = 0, h_timeAcc = 0;

                // Memory usage: two sorting arrays of size W (float) plus the three signal buffers (float[SAMPLES]) and bool[SAMPLES].
                size_t filterMem = 2 * W * sizeof(float)
                                 + 3 * SAMPLES * sizeof(float)
                                 + SAMPLES * sizeof(bool);

                for (int iter = 0; iter < ITERS; iter++) {
                    int totalAnom = 0;

                    // ---- 1. Generate signal + anomalies ----
                    for (int i = 0; i < SAMPLES; i++) {
                        float t = (float)i / 100.0f;  // sampled at 100 Hz
                        cleanSig[i] = 2.0f * sinf(2.0f * PI * 3.0f * t)
                                    + 4.0f * sinf(2.0f * PI * 5.0f * t);

                        float noise = gaussianNoise(0.0f, 0.2f);
                        isAnom[i]   = ((float)random(10000) / 10000.0f) < P;
                        float spike = 0.0f;
                        if (isAnom[i]) {
                            // Spike magnitude U(5,15), random sign
                            spike = (5.0f + (float)random(1000) / 100.0f)
                                    * (random(2) ? 1.0f : -1.0f);
                            totalAnom++;
                        }
                        rawSig[i] = cleanSig[i] + noise + spike;
                    }

                    double peakUnfiltered = dominantFreq(rawSig, SAMPLES, 100);

                    // ---- 2. Z-score filter ----
                    int z_tp = 0, z_fp = 0;
                    unsigned long z_t0 = micros();

                    for (int i = 0; i < SAMPLES; i++) {
                        int lo  = max(0, i - W / 2);
                        int hi  = min(SAMPLES - 1, i + W / 2);
                        int len = hi - lo + 1;

                        float sum = 0.0f;
                        for (int j = lo; j <= hi; j++) sum += rawSig[j];
                        float mean = sum / len;

                        float varSum = 0.0f;
                        for (int j = lo; j <= hi; j++)
                            varSum += (rawSig[j] - mean) * (rawSig[j] - mean);
                        float std = sqrtf(varSum / len);

                        if (std > 1e-9f && fabsf(rawSig[i] - mean) > 3.0f * std) {
                            filtSig[i] = mean;
                            isAnom[i] ? z_tp++ : z_fp++;
                        } else {
                            filtSig[i] = rawSig[i];
                        }
                    }
                    unsigned long z_us   = micros() - z_t0;
                    float         z_mer  = meanErrorReduction(filtSig, rawSig, cleanSig, SAMPLES);
                    double        peak_z = dominantFreq(filtSig, SAMPLES, 100);
                    float z_tpr = totalAnom > 0
                                  ? (float)z_tp / totalAnom : 0.0f;
                    float z_fpr = (SAMPLES - totalAnom) > 0
                                  ? (float)z_fp / (SAMPLES - totalAnom) : 0.0f;

                    // ---- 3. Hampel filter ----
                    int h_tp = 0, h_fp = 0;
                    unsigned long h_t0 = micros();

                    for (int i = 0; i < SAMPLES; i++) {
                        int lo  = max(0, i - W / 2);
                        int hi  = min(SAMPLES - 1, i + W / 2);
                        int len = hi - lo + 1;

                        // Copy window into sort buffer
                        for (int j = 0; j < len; j++) sortBuf[j] = rawSig[lo + j];
                        std::sort(sortBuf, sortBuf + len);
                        float median = sortBuf[len / 2];

                        // MAD (Median Absolute Deviation)
                        for (int j = 0; j < len; j++)
                            sortBuf[j] = fabsf(rawSig[lo + j] - median);
                        std::sort(sortBuf, sortBuf + len);
                        float mad = sortBuf[len / 2];

                        // Hampel threshold: 3 * 1.4826 * MAD
                        if (fabsf(rawSig[i] - median) > 3.0f * 1.4826f * mad) {
                            filtSig[i] = median;
                            isAnom[i] ? h_tp++ : h_fp++;
                        } else {
                            filtSig[i] = rawSig[i];
                        }
                    }
                    unsigned long h_us   = micros() - h_t0;
                    float         h_mer  = meanErrorReduction(filtSig, rawSig, cleanSig, SAMPLES);
                    double        peak_h = dominantFreq(filtSig, SAMPLES, 100);
                    float h_tpr = totalAnom > 0
                                  ? (float)h_tp / totalAnom : 0.0f;
                    float h_fpr = (SAMPLES - totalAnom) > 0
                                  ? (float)h_fp / (SAMPLES - totalAnom) : 0.0f;

                    z_tprAcc += z_tpr; z_fprAcc += z_fpr;
                    z_merAcc += z_mer; z_timeAcc += z_us;
                    h_tprAcc += h_tpr; h_fprAcc += h_fpr;
                    h_merAcc += h_mer; h_timeAcc += h_us;

                    // Per-iteration FFT frequency comparison
                    int unfiltHz  = max((int)(peakUnfiltered * NYQUIST_MARGIN) + 1, MIN_ADAPTIVE_HZ);
                    int zFilterHz = max((int)(peak_z         * NYQUIST_MARGIN) + 1, MIN_ADAPTIVE_HZ);
                    int hFilterHz = max((int)(peak_h         * NYQUIST_MARGIN) + 1, MIN_ADAPTIVE_HZ);

                    Serial.printf("\n[BONUS] P=%.2f | W=%d | iter=%d\n"
                                  "  Anomalies injected : %d / %d\n"
                                  "  Unfiltered FFT peak: %.2f Hz -> adaptive %d Hz\n"
                                  "  Z-score  | exec %6lu us | TPR %.2f | FPR %.2f | MER %5.1f%% | peak %.2f Hz -> %d Hz\n"
                                  "  Hampel   | exec %6lu us | TPR %.2f | FPR %.2f | MER %5.1f%% | peak %.2f Hz -> %d Hz\n",
                                  P, W, iter + 1,
                                  totalAnom, SAMPLES,
                                  peakUnfiltered, unfiltHz,
                                  z_us, z_tpr, z_fpr, z_mer, peak_z, zFilterHz,
                                  h_us, h_tpr, h_fpr, h_mer, peak_h, hFilterHz);
                }

                // ---- Average over ITERS ----
                Serial.printf("\n[BONUS][AVG over %d iters] P=%.2f | W=%d\n"
                              "  Memory for filter buffers : %u bytes\n"
                              "  Z-score  | avg exec %6lu us | avg TPR %.2f | avg FPR %.2f | avg MER %5.1f%%\n"
                              "  Hampel   | avg exec %6lu us | avg TPR %.2f | avg FPR %.2f | avg MER %5.1f%%\n",
                              ITERS, P, W,
                              (unsigned)filterMem,
                              z_timeAcc / ITERS,
                              z_tprAcc / ITERS, z_fprAcc / ITERS, z_merAcc / ITERS,
                              h_timeAcc / ITERS,
                              h_tprAcc / ITERS, h_fprAcc / ITERS, h_merAcc / ITERS);

                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }

        // After one full sweep, pause and let other tasks breathe
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
#endif // RUN_BONUS

// SETUP
void setup() {
    Serial.begin(115200);

    pinMode(36, OUTPUT);
    digitalWrite(36, LOW);

    // Serial1 is used to send phase-sync tags to the Monitor node
    Serial1.begin(115200, SERIAL_8N1, 5, 4);  // RX=5, TX=4

    Serial.println("\n\n==================================================");
    Serial.println("=== IoT Target Node – Booting ===");
    Serial.printf ("=== Signal mode: %d | Bonus: %s ===\n",
                   SIGNAL_MODE, RUN_BONUS ? "ON" : "OFF");
    Serial.println("==================================================\n");

    // --- WiFi ---
    Serial.printf("[WIFI] Connecting to '%s'...", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int wifiTries = 0;
    while (WiFi.status() != WL_CONNECTED && wifiTries < 40) {
        delay(500); Serial.print("."); wifiTries++;
    }
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("\n[WIFI] Connected. IP: %s\n",
                      WiFi.localIP().toString().c_str());
    else
        Serial.println("\n[WIFI] Connection failed – MQTT will be skipped.");

    // --- Queues ---
    sampleQueue    = xQueueCreate(256, sizeof(SensorSample));
    aggregateQueue = xQueueCreate(10,  sizeof(AggregatedData));

    // --- FreeRTOS Tasks ---
    //
    // Core assignment:
    //   Core 0: TaskMaxFreqBenchmark (runs once, short-lived)
    //           TaskTransmit         (I/O-bound, long waits)
    //   Core 1: TaskSample           (time-critical, periodic)
    //           TaskAnalyze          (CPU-intensive FFT)
    //   Core 0: TaskBonusSweep (if enabled, low priority, compute-heavy)
    //
    // Priority order (higher number = higher priority in FreeRTOS):
    //   TaskSample (3) > TaskAnalyze (2) > TaskTransmit (1) = Benchmark (1)

    xTaskCreatePinnedToCore(TaskSample,           "Sample",   4096,  NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(TaskAnalyze,          "Analyze",  8192,  NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskTransmit,         "Transmit", 6144,  NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(TaskMaxFreqBenchmark, "Bench",    4096,  NULL, 1, NULL, 0);

    #if RUN_BONUS == 1
    Serial.println("[INIT] Bonus anomaly-filter benchmark ENABLED.");
    // Stack is larger to accommodate static float arrays and sort buffers
    xTaskCreatePinnedToCore(TaskBonusSweep, "Bonus", 12288, NULL, 1, NULL, 0);
    #endif
}

// loop() is not used with FreeRTOS; delete the idle task to free resources.
void loop() {
    vTaskDelete(NULL);
}