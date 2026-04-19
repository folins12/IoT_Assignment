#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <RadioLib.h>
#include <arduinoFFT.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// ------------ 1. CONFIGURATION & CREDENTIALS ------------
const char* ssid = "";
const char* password = "";
const char* mqtt_server = "broker.hivemq.com"; 

// TTN LoRaWAN Credentials
uint64_t joinEUI = 0x0000000000000000; 
uint64_t devEUI  = 0x0000000000000000; 
uint8_t appKey[]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

#define NSS 8
#define DIO1 14
#define NRST 12
#define BUSY 13
SX1262 radio = new Module(NSS, DIO1, NRST, BUSY);
LoRaWANNode node(&radio, &EU868);

WiFiClient espClient;
PubSubClient mqtt(espClient);

Adafruit_INA219 ina219;
bool ina219_connected = false;


// ------------ 2. SIGNAL & FFT PARAMETERS ------------
const uint16_t samples = 128;
double vReal[samples];
double vImag[samples];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, samples, 100.0);

volatile int current_sampling_hz = 100; 
volatile float current_average = 0.0;
volatile float total_energy_mJ = 0.0;

// RTOS Synchronization flags
volatile bool collect_fft_data = false;
volatile int fft_index = 0;
QueueHandle_t avgQueue;


// ------------ 3. FREERTOS TASKS ------------
void TaskSensor(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  float window_sum = 0;
  int window_count = 0;
  unsigned long window_start_time = millis();

  for(;;) {
    float t = millis() / 1000.0;
    float signal = 3.0 * sin(2 * PI * 4 * t) + 1.5 * sin(2 * PI * 8 * t);
    
    Serial.printf(">Signal:%.2f\n", signal);
    Serial.printf(">SamplingFreq:%d\n", current_sampling_hz);

    // Provide fresh data only when the FFT task requests it
    if (collect_fft_data && fft_index < samples) {
      vReal[fft_index] = signal;
      vImag[fft_index] = 0;
      fft_index++;
    }

    window_sum += signal;
    window_count++;

    // Calculate energy silently
    if(ina219_connected) {
      float power_mW = ina219.getPower_mW();
      total_energy_mJ += power_mW * (1000.0 / current_sampling_hz);
    }

    // 5-Second Aggregation Window
    if (millis() - window_start_time >= 5000) {
      current_average = window_sum / window_count;
      Serial.printf(">Average:%.2f\n", current_average);
      
      xQueueSend(avgQueue, (void *)&current_average, 0);

      window_sum = 0;
      window_count = 0;
      window_start_time = millis();
    }

    const TickType_t xDelay = pdMS_TO_TICKS(1000 / current_sampling_hz);
    vTaskDelayUntil(&xLastWakeTime, xDelay);
  }
}

void TaskProcessFFT(void *pvParameters) {
  for(;;) {
    vTaskDelay(pdMS_TO_TICKS(20000)); 
    
    fft_index = 0;
    collect_fft_data = true;
    while(fft_index < samples) {
      vTaskDelay(pdMS_TO_TICKS(10)); 
    }
    collect_fft_data = false;

    FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(FFT_FORWARD);
    FFT.complexToMagnitude();

    // Maximum amplitude to establish a dynamic noise threshold
    double max_magnitude = 0.0;
    for (uint16_t i = 2; i < (samples / 2); i++) {
      if (vReal[i] > max_magnitude) {
        max_magnitude = vReal[i];
      }
    }
    double threshold = max_magnitude * 0.15; 

    // Maximum frequency that exceeds the threshold
    double max_freq = 0.0;
    for (uint16_t i = 2; i < (samples / 2); i++) {
      if (vReal[i] > threshold) {
        max_freq = (i * 1.0 * current_sampling_hz) / samples; 
      }
    }

    // Nyquist
    if (max_freq > 0 && max_freq < 50) {
      int base_freq = (int)(max_freq * 2.0) + 1;
      // Find the closest divisor of 1000 (greater than or equal to base_freq)
      int new_freq = base_freq;
      while (1000 % new_freq != 0) {
        new_freq++;
      }
      
      current_sampling_hz = new_freq;
      FFT = ArduinoFFT<double>(vReal, vImag, samples, (double)current_sampling_hz);
    }
  }
}

void TaskMQTT(void *pvParameters) {
  float received_avg;
  for(;;) {
    if (xQueueReceive(avgQueue, &received_avg, portMAX_DELAY) == pdPASS) {
      if (WiFi.status() == WL_CONNECTED) {
        if (!mqtt.connected()) mqtt.connect("Heltec_ESP32_IoT");
        char payload[10];
        dtostrf(received_avg, 1, 2, payload);
        mqtt.publish("student/heltec/avg", payload);
      }
    }
  }
}

void TaskLoRa(void *pvParameters) {
  int state = radio.begin();
  if (state == RADIOLIB_ERR_NONE) {
    node.beginOTAA(joinEUI, devEUI, appKey, appKey);
  }
  for(;;) {
    vTaskDelay(pdMS_TO_TICKS(30000)); 
    if(node.isActivated()) {
      uint8_t payload[4];
      memcpy(payload, (uint8_t*)&current_average, 4);
      node.sendReceive(payload, 4);
    }
  }
}

// MAIN SETUP
void setup() {
  Serial.begin(115200);
  delay(2000);

  Wire.begin(41, 42); // INA219 I2C
  if (ina219.begin()) ina219_connected = true;

  WiFi.begin(ssid, password);
  mqtt.setServer(mqtt_server, 1883);
  avgQueue = xQueueCreate(10, sizeof(float));

  xTaskCreatePinnedToCore(TaskSensor, "SensorTask", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskProcessFFT, "FFTTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskMQTT, "MQTTTask", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskLoRa, "LoRaTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelete(NULL); 
}
