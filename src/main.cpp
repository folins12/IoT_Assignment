#include <Arduino.h>
#include <arduinoFFT.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <RadioLib.h>
#include <algorithm>

// CONFIGURATION
#define SIGNAL_MODE         2
#define RUN_BONUS_8_2       1

#define INITIAL_SAMPLING_HZ 100
#define SAMPLES             128
#define WINDOW_MS           5000
#define NYQUIST_MARGIN      2.5f
#define MIN_ADAPTIVE_HZ     10

// CREDENTIALS (Ripristinate dal tuo file originale)
const char* ssid = "Andrea's Galaxy S24";
const char* password = "cdjb0132";
const char* mqtt_server = "broker.hivemq.com";
uint64_t joinEUI = 0x0000000000000000;
uint64_t devEUI  = 0x70B3D57ED0076985; 
uint8_t appKey[] = { 0xE0, 0x9A, 0x93, 0xC8, 0xAE, 0x12, 0x13, 0x47, 0x75, 0x8F, 0x38, 0xCA, 0x40, 0xA0, 0xDA, 0xFD };

// HELTEC V3 LORA PINS
#define LORA_SCK  9
#define LORA_MISO 11
#define LORA_MOSI 10
#define LORA_CS   8
#define LORA_DIO1 14
#define LORA_RST  12
#define LORA_BUSY 13

// STRUCTURES & QUEUES
struct SensorSample {
    float         rawValue;
    unsigned long timestamp;
};

struct AggregatedData {
    float         average;
    unsigned long firstSampleTs;
    unsigned long execMs;
    int           sampleCount;
    int           samplingHz;
};

QueueHandle_t sampleQueue;
QueueHandle_t aggregateQueue;
volatile int  currentDelayMs = 1000 / INITIAL_SAMPLING_HZ;
volatile int  currentHz      = INITIAL_SAMPLING_HZ;

// RADIO, NETWORK & FFT
SPIClass     radioSPI(FSPI);
SX1262       radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, radioSPI);
LoRaWANNode  node(&radio, &EU868);
WiFiClient   espClient;
PubSubClient mqtt(espClient);
double vReal[SAMPLES], vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, INITIAL_SAMPLING_HZ);

// BONUS UTILS
float getGaussianNoise(float mu, float sigma) {
    float u1 = max(0.0001f, (float)random(10000) / 10000.0f);
    float u2 = max(0.0001f, (float)random(10000) / 10000.0f);
    return (sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2)) * sigma + mu;
}

float calculateMER(float* filtered, float* raw, float* clean, int length) {
    float raw_err = 0, filt_err = 0;
    for (int i = 0; i < length; i++) {
        raw_err  += abs(raw[i]      - clean[i]);
        filt_err += abs(filtered[i] - clean[i]);
    }
    return (raw_err == 0) ? 0 : ((raw_err - filt_err) / raw_err) * 100.0;
}

double getDominantFreq(float* sig, int freq) {
    double tR[SAMPLES], tI[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) { tR[i] = sig[i]; tI[i] = 0.0; }
    ArduinoFFT<double> tmp(tR, tI, SAMPLES, freq);
    tmp.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    tmp.compute(FFT_FORWARD);
    tmp.complexToMagnitude();
    return tmp.majorPeak();
}

// =============================================================================
// TASKS
// =============================================================================
void TaskMaxFreqBenchmark(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    unsigned long t0 = micros();
    for (long i = 0; i < 100000; i++) { volatile int v = analogRead(4); (void)v; }
    float maxHz = 100000.0f / ((micros() - t0) / 1e6f);
    Serial.printf("\n[MAX_FREQ] Max hardware freq: %.2f Hz | Configured: %d Hz\n", maxHz, INITIAL_SAMPLING_HZ);
    vTaskDelete(NULL);
}

#if RUN_BONUS_8_2 == 1
void TaskBonusSweep(void *pvParameters) {
    float probs[]   = {0.01, 0.05, 0.10};
    int   windows[] = {5, 15, 31};
    static float clean_sig[SAMPLES], raw_sig[SAMPLES], filt_sig[SAMPLES];
    static bool  is_anom[SAMPLES];

    vTaskDelay(pdMS_TO_TICKS(15000));

    for (;;) {
        for (float P : probs) {
            for (int W : windows) {
                float z_tprA=0,z_fprA=0,z_merA=0; unsigned long z_usA=0;
                float h_tprA=0,h_fprA=0,h_merA=0; unsigned long h_usA=0;

                for (int iter = 0; iter < 5; iter++) {
                    int total_anomalies = 0;
                    for (int i = 0; i < SAMPLES; i++) {
                        float t = i / 100.0;
                        clean_sig[i] = 2.0 * sin(2*PI*3*t) + 4.0 * sin(2*PI*5*t);
                        is_anom[i]   = (random(10000) < (P * 10000));
                        raw_sig[i]   = clean_sig[i] + getGaussianNoise(0.0, 0.2) + (is_anom[i] ? (random(500,1500)/100.0*(random(2)?1:-1)) : 0);
                        if (is_anom[i]) total_anomalies++;
                    }

                    // Z-SCORE
                    int z_tp = 0, z_fp = 0;
                    unsigned long z_t0 = micros();
                    for (int i = 0; i < SAMPLES; i++) {
                        int lo = max(0, i-W/2), hi = min(SAMPLES-1, i+W/2), len = hi-lo+1;
                        float sum = 0, var = 0;
                        for (int j = lo; j <= hi; j++) sum += raw_sig[j];
                        float mean = sum / len;
                        for (int j = lo; j <= hi; j++) var += pow(raw_sig[j]-mean, 2);
                        float sd = sqrt(var / len);
                        if (sd > 1e-9f && abs(raw_sig[i]-mean) > 3.0*sd) {
                            filt_sig[i] = mean; is_anom[i] ? z_tp++ : z_fp++;
                        } else filt_sig[i] = raw_sig[i];
                    }
                    z_usA += micros() - z_t0; z_merA += calculateMER(filt_sig, raw_sig, clean_sig, SAMPLES);
                    z_tprA += total_anomalies ? (float)z_tp/total_anomalies : 0; 
                    z_fprA += (SAMPLES-total_anomalies) ? (float)z_fp/(SAMPLES-total_anomalies) : 0;

                    // HAMPEL
                    int h_tp = 0, h_fp = 0;
                    unsigned long h_t0 = micros();
                    for (int i = 0; i < SAMPLES; i++) {
                        int lo = max(0, i-W/2), hi = min(SAMPLES-1, i+W/2), len = hi-lo+1;
                        float w_arr[31], mad_arr[31];
                        for (int j = 0; j < len; j++) w_arr[j] = raw_sig[lo+j];
                        std::sort(w_arr, w_arr+len); float median = w_arr[len/2];
                        for (int j = 0; j < len; j++) mad_arr[j] = abs(raw_sig[lo+j]-median);
                        std::sort(mad_arr, mad_arr+len); float mad = mad_arr[len/2];
                        
                        if (abs(raw_sig[i]-median) > 3.0*1.4826*mad) {
                            filt_sig[i] = median; is_anom[i] ? h_tp++ : h_fp++;
                        } else filt_sig[i] = raw_sig[i];
                    }
                    h_usA += micros() - h_t0; h_merA += calculateMER(filt_sig, raw_sig, clean_sig, SAMPLES);
                    h_tprA += total_anomalies ? (float)h_tp/total_anomalies : 0; 
                    h_fprA += (SAMPLES-total_anomalies) ? (float)h_fp/(SAMPLES-total_anomalies) : 0;
                }
                Serial.printf("\n[BONUS AVG] P=%.2f W=%d | [Z-Score] TPR:%.2f MER:%.1f%% %luus | [Hampel] TPR:%.2f MER:%.1f%% %luus\n",
                              P, W, z_tprA/5, z_merA/5, z_usA/5, h_tprA/5, h_merA/5, h_usA/5);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
#endif

void TaskSample(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    for (;;) {
        unsigned long now = millis();
        float t = now / 1000.0f;
        SensorSample s;
        s.timestamp = now;

        #if SIGNAL_MODE == 1
            s.rawValue = 5.0 * sin(2*PI*1*t);
        #elif SIGNAL_MODE == 2
            s.rawValue = 3.0 * sin(2*PI*4*t) + 1.5 * sin(2*PI*8*t);
        #elif SIGNAL_MODE == 3
            s.rawValue = 2.0 * sin(2*PI*35*t);
        #endif

        xQueueSend(sampleQueue, &s, 0);
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(currentDelayMs));
    }
}

void TaskAnalyze(void *pvParameters) {
    float windowSum = 0;
    int windowCount = 0, fftCount = 0;
    unsigned long windowFirstTs = 0, windowWallStart = 0, windowStart = millis();

    for (;;) {
        SensorSample s;
        if (xQueueReceive(sampleQueue, &s, portMAX_DELAY) == pdPASS) {
            if (windowCount == 0) { windowFirstTs = s.timestamp; windowWallStart = millis(); }
            windowSum += s.rawValue;
            windowCount++;

            if (fftCount < SAMPLES) {
                vReal[fftCount] = s.rawValue; vImag[fftCount] = 0; fftCount++;
            } else {
                unsigned long fft_start = micros();
                FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
                FFT.compute(FFT_FORWARD);
                FFT.complexToMagnitude();
                
                double peak = FFT.majorPeak();
                int newHz = max((int)(peak * NYQUIST_MARGIN) + 1, MIN_ADAPTIVE_HZ);

                if (newHz != currentHz) {
                    Serial.printf("\n[FFT] Peak=%.2fHz | Freq aggiornata: %dHz -> %dHz (Exec=%luus)\n", peak, currentHz, newHz, micros() - fft_start);
                    currentHz = newHz; 
                    currentDelayMs = 1000 / newHz;
                }
                fftCount = 0;
            }

            if (millis() - windowStart >= WINDOW_MS) {
                AggregatedData agg = { windowSum / windowCount, windowFirstTs, millis() - windowWallStart, windowCount, currentHz };
                xQueueSend(aggregateQueue, &agg, 0);
                windowSum = 0; windowCount = 0; windowStart = millis();
            }
        }
    }
}

void TaskTransmit(void *pvParameters) {
    mqtt.setServer(mqtt_server, 1883);
    radioSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

    if (radio.begin() == RADIOLIB_ERR_NONE) {
        radio.setDio2AsRfSwitch(true); radio.setTCXO(1.8);
        node.beginOTAA(joinEUI, devEUI, appKey, appKey);
        
        Serial.println("\n[LORA] Tentativo di join OTAA...");
        while(!node.isActivated()) { 
            int join_state = node.activateOTAA();
            if (node.isActivated()) Serial.println("[LORA] JOIN SUCCESSFUL! Connesso a TTN.");
            else { Serial.printf("[LORA] JOIN FAILED (Code: %d). Riprovo in 10s...\n", join_state); vTaskDelay(pdMS_TO_TICKS(10000)); }
        }
    }

    // Svuota la coda accumulata durante il join per evitare latenze di 50+ secondi
    xQueueReset(aggregateQueue);
    int count = 0;

    for (;;) {
        AggregatedData agg;
        if (xQueueReceive(aggregateQueue, &agg, portMAX_DELAY) == pdPASS) {
            count++;
            // STAMPA LA FASE QUI: Garantisce che il Monitor la legga ogni 5 secondi
            Serial.printf("\nPHASE:%s\n", agg.samplingHz == INITIAL_SAMPLING_HZ ? "OVERSAMPLE" : "ADAPTIVE");
            Serial.println("--------------------------------------------------");
            Serial.printf("[WIN#%d] Avg=%.4f | Freq=%dHz | Exec=%lums | E2E Latency=%lums\n", count, agg.average, agg.samplingHz, agg.execMs, millis() - agg.firstSampleTs);

            if (WiFi.status() == WL_CONNECTED) {
                if (!mqtt.connected()) mqtt.connect("HeltecTarget");
                mqtt.loop();
                char msg[16]; sprintf(msg, "%.4f", agg.average);
                Serial.printf("[TX-MQTT] %s -> %s\n", msg, mqtt.publish("iot_assignment/folins12/average", msg) ? "OK" : "FAILED");
            }

            if (node.isActivated()) {
                if (count % 6 == 0) {
                    int16_t tx = node.sendReceive((uint8_t*)&agg.average, 4);
                    Serial.printf("[TX-LORA] Uplink %s (code %d)\n", (tx == RADIOLIB_ERR_NONE || tx == -1116 || tx == RADIOLIB_ERR_RX_TIMEOUT) ? "OK" : "FAIL", tx);
                } else Serial.printf("[TX-LORA] Duty cycle in pausa (%d/6)\n", count % 6);
            }
            Serial.println("--------------------------------------------------\n");
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(36, OUTPUT); digitalWrite(36, LOW);

    Serial.printf("\n[BOOT] Signal=%d | Bonus=%s | InitHz=%d\n", SIGNAL_MODE, RUN_BONUS_8_2?"ON":"OFF", INITIAL_SAMPLING_HZ);

    Serial.print("[WIFI] Connessione in corso...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println(" OK");

    sampleQueue    = xQueueCreate(256, sizeof(SensorSample));
    aggregateQueue = xQueueCreate(10,  sizeof(AggregatedData));

    xTaskCreatePinnedToCore(TaskSample,           "Samp",   4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(TaskAnalyze,          "Anal",   8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskTransmit,         "Tx",     6144, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(TaskMaxFreqBenchmark, "MaxFreq",4096, NULL, 1, NULL, 0);

    #if RUN_BONUS_8_2 == 1
    xTaskCreatePinnedToCore(TaskBonusSweep, "Bonus", 10240, NULL, 1, NULL, 0);
    #endif
}

void loop() { vTaskDelete(NULL); }