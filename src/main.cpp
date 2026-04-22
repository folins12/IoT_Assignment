#include <Arduino.h>
#include <arduinoFFT.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <RadioLib.h>
#include "bonus.h"
#include "credentials.h"

// CONFIGURATION
#define SIGNAL_MODE 2       // 1 (Low), 2 (Complex), 3 (High)
#define RUN_BONUS_8_2 0     // 1 to run bonus part
#define INITIAL_SAMPLING_HZ 100
#define SAMPLES             128
#define WINDOW_MS           5000

// HELTEC V3 LORA PINS
#define LORA_SCK 9
#define LORA_MISO 11
#define LORA_MOSI 10
#define LORA_CS 8
#define LORA_DIO1 14
#define LORA_RST 12
#define LORA_BUSY 13

// STRUCTURES
struct SensorSample {
    float rawValue;
    unsigned long timestamp;
};

struct AggregatedData {
    float average;
    unsigned long firstTimestamp; 
    double lastFFTPeak;
};

QueueHandle_t sampleQueue;
QueueHandle_t aggregateQueue;

volatile int currentDelayMs = 1000 / INITIAL_SAMPLING_HZ;
volatile uint32_t hardwareMaxSamplingFreq = 0;

// RADIO & NETWORK OBJECTS
SPIClass radioSPI(FSPI);
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, radioSPI);
LoRaWANNode node(&radio, &EU868);
WiFiClient espClient;
PubSubClient mqtt(espClient);

double vReal[SAMPLES];
double vImag[SAMPLES];

// ----------------------- TASKS -----------------------------

// TASK: SAMPLING
void TaskSample(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    for (;;) {
        float jitter = random(-5, 5) / 10000.0; 
        float t = (micros() / 1000000.0) + jitter;
        
        SensorSample s;
        s.timestamp = millis();
        
        #if SIGNAL_MODE == 1
            s.rawValue = 5.0 * sin(2 * PI * 1 * t);
        #elif SIGNAL_MODE == 2
            s.rawValue = 3.0 * sin(2 * PI * 4 * t) + 1.5 * sin(2 * PI * 3 * t);
        #elif SIGNAL_MODE == 3
            s.rawValue = 2.0 * sin(2 * PI * 35 * t);
        #endif

        xQueueSend(sampleQueue, &s, portMAX_DELAY);
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(currentDelayMs));
    }
}

// TASK: FFT & ANALYSIS
void TaskAnalyze(void *pvParameters) {
    float windowSum = 0; 
    int windowCount = 0;
    unsigned long windowStart = millis();
    unsigned long firstSampleTime = 0;
    
    int fftCount = 0;
    double currentPeak = 0.0;

    for (;;) {
        SensorSample s;
        if (xQueueReceive(sampleQueue, &s, portMAX_DELAY) == pdPASS) {
            
            if (windowCount == 0) firstSampleTime = s.timestamp;
            
            windowSum += s.rawValue;
            windowCount++;

            if (fftCount < SAMPLES) {
                vReal[fftCount] = s.rawValue; 
                vImag[fftCount] = 0; 
                fftCount++;
            } 
            
            if (fftCount >= SAMPLES) {
                double currentFs = 1000.0 / currentDelayMs;
                ArduinoFFT<double> localFFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, currentFs);
                
                localFFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
                localFFT.compute(FFT_FORWARD);
                localFFT.complexToMagnitude();
                
                currentPeak = localFFT.majorPeak();
                
                int newFreq = (int)(currentPeak * 2.5) + 1; 
                if (newFreq < 10) newFreq = 10; 
                
                currentDelayMs = 1000 / newFreq;
                fftCount = 0; 
            }

            if (millis() - windowStart >= WINDOW_MS) {
                AggregatedData agg;
                agg.average = windowCount > 0 ? (windowSum / windowCount) : 0.0;
                agg.firstTimestamp = firstSampleTime;
                agg.lastFFTPeak = currentPeak;
                
                xQueueSend(aggregateQueue, &agg, 0);
                
                windowSum = 0; 
                windowCount = 0; 
                windowStart = millis();
            }
        }
    }
}

// TASK: MQTT & LoRa TRANSMISSION
void TaskTransmit(void *pvParameters) {
    mqtt.setServer(mqtt_server, 1883);

    bool loraActivated = false;
    radioSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    
    if (radio.begin() == RADIOLIB_ERR_NONE) {
        radio.setDio2AsRfSwitch(true); 
        radio.setTCXO(1.8);
        node.beginOTAA(joinEUI, devEUI, appKey, appKey);
        
        Serial.println("[LoRaWAN] Attempting background TTN Join...");
        if (node.activateOTAA() == RADIOLIB_ERR_NONE) {
            loraActivated = true;
        }
    }

    static int loraDutyCycleCount = 0;

    for (;;) {
        AggregatedData agg;
        if (xQueueReceive(aggregateQueue, &agg, portMAX_DELAY) == pdPASS) {
            
            unsigned long e2e_latency = millis() - agg.firstTimestamp;
            int adaptiveBytes = (1000 / currentDelayMs) * (WINDOW_MS / 1000) * sizeof(float);
            int rawBytes = INITIAL_SAMPLING_HZ * (WINDOW_MS / 1000) * sizeof(float);

            loraDutyCycleCount++;

            if (!loraActivated && (loraDutyCycleCount % 6 == 0)) {
                if (node.activateOTAA() == RADIOLIB_ERR_NONE) {
                    loraActivated = true;
                }
            }

            Serial.println("\n==================================================");
            Serial.println("[EDGE NODE] 5-Second Window Aggregation");
            Serial.println("--------------------------------------------------");
            
            if (WiFi.status() == WL_CONNECTED) {
                if (!mqtt.connected()) mqtt.connect("HeltecTarget");
                mqtt.loop(); 

                char msg[16]; 
                sprintf(msg, "%.2f", agg.average);
                mqtt.publish("iot_assignment/folins12/average", msg);
                
                Serial.printf(">> Avg Value    : %s\n", msg);
                Serial.printf(">> E2E Latency  : %lu ms\n", e2e_latency);
            } else {
                Serial.println(">> MQTT Status  : OFFLINE");
            }
            
            Serial.printf(">> FFT Peak     : %.2f Hz\n", agg.lastFFTPeak);
            Serial.printf(">> Sampling At  : %d Hz\n", (int)(1000 / currentDelayMs));
            Serial.printf(">> Data Volume  : %d bytes (Adaptive) vs %d bytes (Raw)\n", adaptiveBytes, rawBytes);
            
            if (loraActivated) {
                if (loraDutyCycleCount % 6 == 0) {
                    int16_t tx_state = node.sendReceive((uint8_t*)&agg.average, 4);
                    if (tx_state == RADIOLIB_ERR_NONE || tx_state == -1116 || tx_state == RADIOLIB_ERR_RX_TIMEOUT) {
                        Serial.println(">> Cloud Link   : TTN Uplink SUCCESS");
                    } else {
                        Serial.printf(">> Cloud Link   : TTN Uplink FAILED (%d)\n", tx_state);
                    }
                } else {
                    Serial.printf(">> Cloud Link   : TTN Duty Cycle Paused (%d/6)\n", loraDutyCycleCount % 6);
                }
            } else {
                Serial.printf(">> Cloud Link   : TTN Disconnected (Retry in %d windows)\n", 6 - (loraDutyCycleCount % 6));
            }
            Serial.println("==================================================");
        }
    }
}

// SETUP
void setup() {
    Serial.begin(115200);
    pinMode(36, OUTPUT); digitalWrite(36, LOW); 
    delay(2000);
    
    Serial.println("\n\n==================================================");
    Serial.println("=== IoT System Booting ===");
    Serial.println("==================================================");

    Serial.println("[SYSTEM] Hardware ADC Benchmark Started...");
    uint32_t sampleCount = 0;
    uint32_t startTime = micros();
    while (micros() - startTime < 1000000) {
        volatile int dummyValue = analogRead(4);
        sampleCount++;
    }
    hardwareMaxSamplingFreq = sampleCount;
    Serial.printf("[SYSTEM] Max Hardware Rate : %u Hz\n", hardwareMaxSamplingFreq);
    Serial.printf("[SYSTEM] Init Sampling Rate: %d Hz\n", INITIAL_SAMPLING_HZ);

    Serial.print("\n[WIFI] Connecting...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\n[WIFI] Connected.");

    sampleQueue = xQueueCreate(512, sizeof(SensorSample));
    aggregateQueue = xQueueCreate(10, sizeof(AggregatedData));

    xTaskCreatePinnedToCore(TaskSample, "Samp", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(TaskAnalyze, "Anal", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskTransmit, "Tx", 8192, NULL, 1, NULL, 1);

    #if RUN_BONUS_8_2 == 1
    Serial.println("[INIT] Bonus Task Enabled in background.");
    xTaskCreatePinnedToCore(TaskBonusSweep, "Bonus", 8192, NULL, 1, NULL, 1);
    #endif
}

void loop() { 
    vTaskDelete(NULL); 
}