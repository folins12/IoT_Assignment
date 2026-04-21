#include <Arduino.h>
#include <arduinoFFT.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <RadioLib.h>
#include <algorithm>

// CONFIGURATION
#define SIGNAL_MODE 2       // 1,2,3 to test the three different signals   
#define RUN_BONUS_8_2 1     // 1 to do the bonus part, else 0

#define INITIAL_SAMPLING_HZ 100
#define SAMPLES             128
#define WINDOW_MS           5000

// CREDENTIALS
const char* ssid = "Andrea's Galaxy S24";
const char* password = "cdjb0132";
const char* mqtt_server = "broker.hivemq.com";

uint64_t joinEUI = 0x0000000000000000;
uint64_t devEUI  = 0x70B3D57ED0076985; 
uint8_t appKey[] = { 0xE0, 0x9A, 0x93, 0xC8, 0xAE, 0x12, 0x13, 0x47, 0x75, 0x8F, 0x38, 0xCA, 0x40, 0xA0, 0xDA, 0xFD };

// HELTEC V3 LORA PINS
#define LORA_SCK 9
#define LORA_MISO 11
#define LORA_MOSI 10
#define LORA_CS 8
#define LORA_DIO1 14
#define LORA_RST 12
#define LORA_BUSY 13

// STRUCTURES & QUEUES
struct SensorSample {
    float rawValue;
    unsigned long timestamp;
};

struct AggregatedData {
    float average;
    unsigned long timestamp;
};

QueueHandle_t sampleQueue;
QueueHandle_t aggregateQueue;
volatile int currentDelayMs = 1000 / INITIAL_SAMPLING_HZ;

// RADIO & NETWORK OBJECTS
SPIClass radioSPI(FSPI);
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, radioSPI);
LoRaWANNode node(&radio, &EU868);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// FFT OBJECTS
double vReal[SAMPLES], vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, INITIAL_SAMPLING_HZ);


// UTILS for bonus
float getGaussianNoise(float mu, float sigma) {
    float u1 = max(0.0001f, (float)random(10000) / 10000.0f);
    float u2 = max(0.0001f, (float)random(10000) / 10000.0f);
    return (sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2)) * sigma + mu;
}

float calculateMER(float* filtered, float* raw, float* clean, int length) {
    float raw_err = 0, filt_err = 0;
    for (int i=0; i<length; i++) {
        raw_err += abs(raw[i] - clean[i]);
        filt_err += abs(filtered[i] - clean[i]);
    }
    return (raw_err == 0) ? 0 : ((raw_err - filt_err) / raw_err) * 100.0;
}

double getDominantFreq(float* input_signal, int freq) {
    double tempReal[SAMPLES], tempImag[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        tempReal[i] = input_signal[i];
        tempImag[i] = 0.0;
    }
    ArduinoFFT<double> tempFFT = ArduinoFFT<double>(tempReal, tempImag, SAMPLES, freq);
    tempFFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    tempFFT.compute(FFT_FORWARD);
    tempFFT.complexToMagnitude();
    return tempFFT.majorPeak();
}


// TASK: MAX FREQUENCY BENCHMARK
void TaskMaxFreqBenchmark(void *pvParameters) {
    Serial.println("\n[MAX_FREQ] Starting ADC Test (100,000 samples)...");
    unsigned long start = micros();
    for(long i=0; i<100000; i++) { volatile int v = analogRead(4); }
    unsigned long end = micros();
    float freq = 100000.0 / ((end - start) / 1000000.0);
    Serial.printf("[MAX_FREQ] Maximum Hardware Frequency: %.2f Hz\n", freq);
    vTaskDelete(NULL);
}


// TASK BONUS
#if RUN_BONUS_8_2 == 1
void TaskBonusSweep(void *pvParameters) {
    float probs[] = {0.01, 0.05, 0.10};
    int windows[] = {5, 15, 31};
    
    float clean_sig[SAMPLES], raw_sig[SAMPLES], filt_sig[SAMPLES];
    bool is_anom[SAMPLES];

    vTaskDelay(pdMS_TO_TICKS(15000)); // Wait for WiFi/LoRa stabilization

    for(;;) {
        for (float P : probs) {
            for (int W : windows) {
                for(int iter=0; iter<5; iter++) {
                    
                    int total_anomalies = 0;
                    
                    // 1. Generate Signal with Anomalies
                    for (int i = 0; i < SAMPLES; i++) {
                        float t = i / 100.0; 
                        clean_sig[i] = 2.0 * sin(2 * PI * 3 * t) + 4.0 * sin(2 * PI * 5 * t);
                        float noise = getGaussianNoise(0.0, 0.2);
                        is_anom[i] = (random(10000) < (P * 10000));
                        float spike = is_anom[i] ? (random(500, 1500) / 100.0 * (random(2) ? 1 : -1)) : 0;
                        raw_sig[i] = clean_sig[i] + noise + spike;
                        if(is_anom[i]) total_anomalies++;
                    }

                    Serial.println("\n========================================");
                    Serial.printf("Testing at P=%.2f, WINDOW=%d\n", P, W);

                    double peak_unfiltered = getDominantFreq(raw_sig, 100);
                    Serial.printf("[UNFILTERED] FFT Peak: %.2f Hz\n", peak_unfiltered);

                    // 2. Z-SCORE Filter
                    int z_tp = 0, z_fp = 0;
                    unsigned long z_start = micros();
                    for (int i = 0; i < SAMPLES; i++) {
                        int start_idx = max(0, i - W / 2);
                        int end_idx = min(SAMPLES - 1, i + W / 2);
                        int w_size = end_idx - start_idx + 1;
                        
                        float sum = 0, var_sum = 0;
                        for (int j = start_idx; j <= end_idx; j++) sum += raw_sig[j];
                        float mean = sum / w_size;
                        for (int j = start_idx; j <= end_idx; j++) var_sum += pow(raw_sig[j] - mean, 2);
                        float std_dev = sqrt(var_sum / w_size);

                        if (abs(raw_sig[i] - mean) > (3.0 * std_dev)) {
                            filt_sig[i] = mean;
                            is_anom[i] ? z_tp++ : z_fp++;
                        } else filt_sig[i] = raw_sig[i];
                    }
                    unsigned long z_time = micros() - z_start;
                    float z_mer = calculateMER(filt_sig, raw_sig, clean_sig, SAMPLES);
                    double peak_z = getDominantFreq(filt_sig, 100);
                    float z_tpr = total_anomalies > 0 ? (float)z_tp / total_anomalies : 0;
                    float z_fpr = (SAMPLES - total_anomalies) > 0 ? (float)z_fp / (SAMPLES - total_anomalies) : 0;
                    
                    Serial.printf("[Z-SCORE] Exec: %lu us | TPR: %.2f | FPR: %.2f | MER: %5.1f%% | FFT Peak: %.2f Hz\n", 
                                  z_time, z_tpr, z_fpr, z_mer, peak_z);

                    // 3. HAMPEL Filter
                    int h_tp = 0, h_fp = 0;
                    unsigned long h_start = micros();
                    for (int i = 0; i < SAMPLES; i++) {
                        int start_idx = max(0, i - W / 2);
                        int end_idx = min(SAMPLES - 1, i + W / 2);
                        int w_size = end_idx - start_idx + 1;
                        
                        float w_arr[31];
                        for (int j = 0; j < w_size; j++) w_arr[j] = raw_sig[start_idx + j];
                        std::sort(w_arr, w_arr + w_size);
                        float median = w_arr[w_size / 2];

                        float mad_arr[31];
                        for (int j = 0; j < w_size; j++) mad_arr[j] = abs(raw_sig[start_idx + j] - median);
                        std::sort(mad_arr, mad_arr + w_size);
                        float mad = mad_arr[w_size / 2];

                        if (abs(raw_sig[i] - median) > (3.0 * 1.4826 * mad)) {
                            filt_sig[i] = median;
                            is_anom[i] ? h_tp++ : h_fp++;
                        } else filt_sig[i] = raw_sig[i];
                    }
                    unsigned long h_time = micros() - h_start;
                    float h_mer = calculateMER(filt_sig, raw_sig, clean_sig, SAMPLES);
                    double peak_h = getDominantFreq(filt_sig, 100);
                    float h_tpr = total_anomalies > 0 ? (float)h_tp / total_anomalies : 0;
                    float h_fpr = (SAMPLES - total_anomalies) > 0 ? (float)h_fp / (SAMPLES - total_anomalies) : 0;

                    Serial.printf("[HAMPEL]  Exec: %lu us | TPR: %.2f | FPR: %.2f | MER: %5.1f%% | FFT Peak: %.2f Hz\n", 
                                  h_time, h_tpr, h_fpr, h_mer, peak_h);

                    vTaskDelay(pdMS_TO_TICKS(1500));
                }
            }
        }
    }
}
#endif


// TASK SAMPLING
void TaskSample(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    for (;;) {
        float t = millis() / 1000.0;
        SensorSample s;
        s.timestamp = millis();
        
        #if SIGNAL_MODE == 1
            s.rawValue = 5.0 * sin(2 * PI * 1 * t);
        #elif SIGNAL_MODE == 2
            s.rawValue = 3.0 * sin(2 * PI * 4 * t) + 1.5 * sin(2 * PI * 8 * t);
        #elif SIGNAL_MODE == 3
            s.rawValue = 2.0 * sin(2 * PI * 35 * t);
        #endif

        xQueueSend(sampleQueue, &s, 0);
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(currentDelayMs));
    }
}


// TASK FFT & ANALYSIS
void TaskAnalyze(void *pvParameters) {
    float windowSum = 0; int windowCount = 0;
    unsigned long windowStart = millis();
    int fftCount = 0;

    for (;;) {
        SensorSample s;
        if (xQueueReceive(sampleQueue, &s, portMAX_DELAY) == pdPASS) {
            windowSum += s.rawValue;
            windowCount++;

            if (fftCount < SAMPLES) {
                vReal[fftCount] = s.rawValue; vImag[fftCount] = 0; fftCount++;
            } else {
                unsigned long fft_start = micros();
                
                FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
                FFT.compute(FFT_FORWARD);
                FFT.complexToMagnitude();
                
                unsigned long fft_time = micros() - fft_start;
                double peak = FFT.majorPeak();
                
                int newFreq = (int)(peak * 2.2) + 2; 
                if(newFreq < 10) newFreq = 10;
                
                if (1000 / newFreq != currentDelayMs) {
                    currentDelayMs = 1000 / newFreq;
                    Serial.printf("\n[FFT MAIN] Adapted Freq to %d Hz (Exec time: %lu us)\n", newFreq, fft_time);
                }
                fftCount = 0;
            }

            if (millis() - windowStart >= WINDOW_MS) {
                AggregatedData agg = { windowSum / windowCount, s.timestamp };
                xQueueSend(aggregateQueue, &agg, 0);
                windowSum = 0; windowCount = 0; windowStart = millis();
            }
        }
    }
}


// TASK CLOUD/EDGE TRANSMISSION
void TaskTransmit(void *pvParameters) {
    mqtt.setServer(mqtt_server, 1883);

    Serial.println("\n[LORA] Booting Radio in background...");
    radioSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    if (radio.begin() == RADIOLIB_ERR_NONE) {
        radio.setDio2AsRfSwitch(true); 
        radio.setTCXO(1.8);
        node.beginOTAA(joinEUI, devEUI, appKey, appKey);
        
        while(!node.isActivated()) { 
            int join_state = node.activateOTAA();
            if (node.isActivated()) {
                Serial.println("\n[LORA] JOIN SUCCESSFUL! Connected to TTN.");
            } else {
                Serial.printf("[LORA] JOIN FAILED (Code: %d). Retrying in 10s...\n", join_state);
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        }
    }

    for (;;) {
        AggregatedData agg;
        if (xQueueReceive(aggregateQueue, &agg, portMAX_DELAY) == pdPASS) {
            
            Serial.println("\n--------------------------------------------------");
            
            if (WiFi.status() == WL_CONNECTED) {
                if (!mqtt.connected()) mqtt.connect("HeltecTarget");
                mqtt.loop(); 

                char msg[16]; sprintf(msg, "%.2f", agg.average);
                mqtt.publish("iot_assignment/folins12/average", msg);
                Serial.printf("[TX-MQTT] Average: %s | E2E Latency: %lu ms\n", msg, millis() - agg.timestamp);
                
                int theoretical = (INITIAL_SAMPLING_HZ * 5) * sizeof(float);
                Serial.printf("[METRICS] Payload: 4 bytes (vs %d bytes over-sampled)\n", theoretical);
            }
            
            static int count = 0;
            count++;
            if (node.isActivated()) {
                if (count % 6 == 0) {
                    int16_t tx_state = node.sendReceive((uint8_t*)&agg.average, 4);
                    if(tx_state == RADIOLIB_ERR_NONE || tx_state == -1116 || tx_state == RADIOLIB_ERR_RX_TIMEOUT) {
                        Serial.println("[TX-LORA] Uplink SUCCESS: Data sent to TTN.");
                    } else {
                        Serial.printf("[TX-LORA] Uplink FAILED (Error: %d)\n", tx_state);
                    }
                } else {
                    Serial.printf("[TX-LORA] Duty cycle paused (%d/6)\n", count % 6);
                }
            }
            Serial.println("--------------------------------------------------\n");
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}


// SETUP & LOOP
void setup() {
    Serial.begin(115200);
    pinMode(36, OUTPUT); digitalWrite(36, LOW); 
    
    Serial.println("\n\n==================================================");
    Serial.println("=== IoT System Booting ===");
    Serial.println("==================================================");

    Serial.print("[WIFI] Connecting...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\n[WIFI] Connected.");

    sampleQueue = xQueueCreate(256, sizeof(SensorSample));
    aggregateQueue = xQueueCreate(10, sizeof(AggregatedData));

    xTaskCreatePinnedToCore(TaskSample, "Samp", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(TaskAnalyze, "Anal", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskTransmit, "Tx", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(TaskMaxFreqBenchmark, "MaxFreq", 4096, NULL, 1, NULL, 1);

    #if RUN_BONUS_8_2 == 1
    Serial.println("[INIT] Bonus Task (8.2) ENABLED in background.");
    xTaskCreatePinnedToCore(TaskBonusSweep, "Bonus", 8192, NULL, 1, NULL, 1);
    #endif
}

void loop() { 
    vTaskDelete(NULL); 
}