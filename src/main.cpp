#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <RadioLib.h>
#include <arduinoFFT.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// 1. CREDENTIALS & CONFIGURATIONS
const char* ssid = "iPhone di Michele";
const char* password = "Michele4!";
const char* mqtt_server = "broker.hivemq.com"; 

uint64_t joinEUI = 0x0000000000000000; 
uint64_t devEUI  = 0x70B3D57ED0076985;
uint8_t appKey[] = { 0xE0, 0x9A, 0x93, 0xC8, 0xAE, 0x12, 0x13, 0x47, 0x75, 0x8F, 0x38, 0xCA, 0x40, 0xA0, 0xDA, 0xFD };

// Heltec V3 Internal LoRa Pins
#define LORA_SCK 9
#define LORA_MISO 11
#define LORA_MOSI 10
#define LORA_CS 8
#define LORA_DIO1 14
#define LORA_RST 12
#define LORA_BUSY 13

// 2. GLOBAL OBJECTS & VARIABLES
SPIClass radioSPI(FSPI);
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, radioSPI);
LoRaWANNode node(&radio, &EU868);

WiFiClient espClient;
PubSubClient mqtt(espClient);

Adafruit_INA219 ina219;
bool ina219_connected = false;

// DSP & FFT Parameters
#define SIGNAL_TEST_MODE 1
const uint16_t samples = 128;
double vReal[samples];
double vImag[samples];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, samples, 100.0);

volatile int current_sampling_hz = 100;
volatile float current_average = 0.0;
volatile double total_energy_mJ = 0.0;

volatile bool collect_fft_data = false;
volatile int fft_index = 0;

QueueHandle_t avgQueue;

// 3. FREERTOS TASKS
void TaskSensor(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  float window_sum = 0;
  int window_count = 0;
  unsigned long window_start_time = millis();

  for(;;) {
    float t = millis() / 1000.0;
    float signal = 0.0;
    
    #if SIGNAL_TEST_MODE == 1
        signal = 5.0 * sin(2 * PI * 1 * t);
    #elif SIGNAL_TEST_MODE == 2
        signal = 3.0 * sin(2 * PI * 4 * t) + 1.5 * sin(2 * PI * 8 * t);
    #elif SIGNAL_TEST_MODE == 3
        signal = 2.0 * sin(2 * PI * 35 * t);
    #endif
    
    if (collect_fft_data && fft_index < samples) {
      vReal[fft_index] = (double)signal;
      vImag[fft_index] = 0.0;
      fft_index++;
    }
    
    window_sum += signal;
    window_count++;

    if(ina219_connected) {
      float power_mW = ina219.getPower_mW();
      total_energy_mJ += power_mW * (1.0 / current_sampling_hz); 
    }

    // 5-Second Aggregation Window
    if (millis() - window_start_time >= 5000) {
      unsigned long execution_time = millis() - window_start_time;
      current_average = window_sum / window_count;
      
      Serial.printf("\n--- WINDOW COMPLETE ---\n");
      Serial.printf("> Average: %.2f\n", current_average);
      Serial.printf("> Sampling Freq: %d Hz\n", current_sampling_hz);
      Serial.printf("> Window Exec Time: %lu ms\n", execution_time);
      Serial.printf("> Accumulated Energy: %.2f mJ\n", total_energy_mJ);
      Serial.printf("-----------------------\n");

      xQueueSend(avgQueue, (void *)&current_average, 0);
      
      window_sum = 0;
      window_count = 0;
      window_start_time = millis();
    }
    
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000 / current_sampling_hz));
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

    // Dynamic thresholding to ignore noise
    double max_magnitude = 0.0;
    for (uint16_t i = 2; i < (samples / 2); i++) {
      if (vReal[i] > max_magnitude) max_magnitude = vReal[i];
    }
    double threshold = max_magnitude * 0.15; 
    
    // Identify maximum dominant frequency
    double max_freq = 0.0;
    for (uint16_t i = 2; i < (samples / 2); i++) {
      if (vReal[i] > threshold) max_freq = (i * 1.0 * current_sampling_hz) / samples; 
    }

    // Adapt sampling frequency according to Nyquist Theorem (Fs > 2 * Fmax)
    if (max_freq > 0 && max_freq < 50) {
      int base_freq = (int)(max_freq * 2.0) + 1;
      int new_freq = base_freq;
      
      // Ensure the frequency is a clean divisor of 1000ms for FreeRTOS ticks
      //while (1000 % new_freq != 0) new_freq++;
      
      if (current_sampling_hz != new_freq) {
        Serial.printf("[FFT] Dominant Freq: %.2f Hz. Adapting Sampling Freq to %d Hz\n", max_freq, new_freq);
        current_sampling_hz = new_freq;
        FFT = ArduinoFFT<double>(vReal, vImag, samples, (double)current_sampling_hz);
      }
    }
  }
}


void TaskMQTT(void *pvParameters) {
  float received_avg;
  for(;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqtt.connected()) {
        String clientId = "Heltec-" + String(random(0xffff), HEX);
        mqtt.connect(clientId.c_str());
      }
      mqtt.loop(); 
    }
    
    // Receive data from the Sensor Task
    if (xQueueReceive(avgQueue, &received_avg, pdMS_TO_TICKS(100)) == pdPASS) {
      if (mqtt.connected()) {
        char payload[10];
        dtostrf(received_avg, 1, 2, payload);
        mqtt.publish("iot_assignment/folins12/average", payload);
        Serial.printf("[MQTT] Edge Server Publish: %s\n", payload);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


void TaskLoRa(void *pvParameters) {
  Serial.println("\n[LORA] Initializing Radio Module...");
  radioSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  
  int state = radio.begin();
  if (state == RADIOLIB_ERR_NONE) {
    // Critical hardware configurations for Heltec V3
    radio.setDio2AsRfSwitch(true); 
    radio.setTCXO(1.8);            
    
    node.beginOTAA(joinEUI, devEUI, appKey, appKey);
    
    // Join Network Loop// Join Network Loop
    while(!node.isActivated()) {
      Serial.println("[LORA] Attempting TTN Join Request...");
      int join_state = node.activateOTAA();
      
      // Controlliamo direttamente se si è attivata, ignorando i finti errori
      if (node.isActivated()) {
        Serial.println("\n[LORA] JOIN SUCCESSFUL! Connected to TTN.");
      } else {
        Serial.printf("[LORA] JOIN FAILED (Code: %d). Retrying in 10s...\n", join_state);
        vTaskDelay(pdMS_TO_TICKS(10000));
      }
    }
  }

  for(;;) {
    vTaskDelay(pdMS_TO_TICKS(30000)); 
    
    if(node.isActivated()) {
      Serial.println("[LORA] Preparing Cloud Uplink...");
      uint8_t payload[4];
      float avg = current_average;
      
      memcpy(payload, &avg, sizeof(float));
      
      unsigned long tx_start = millis();
      int tx_state = node.sendReceive(payload, 4);
      unsigned long latency = millis() - tx_start;
      
      if(tx_state == RADIOLIB_ERR_NONE || tx_state == -1116 || tx_state == RADIOLIB_ERR_RX_TIMEOUT) {
         Serial.printf("[LORA] Packet DELIVERED to TTN! (Latency: %lu ms)\n", latency);
      } else {
         Serial.printf("[LORA] Transmission Error: %d\n", tx_state);
      }
    }
  }
}


// 4. MAIN SETUP
void setup() {
  // Power up the external LoRa radio hardware (Vext)
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW); 
  delay(100);

  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== IoT Individual Assignment Booting ===");

  // Initialize Power Sensor
  Wire.begin(41, 42); 
  if (ina219.begin()) {
    ina219_connected = true;
    Serial.println("[INIT] INA219 Power Sensor Connected.");
  } else {
    Serial.println("[INIT] INA219 not found. Power metrics disabled.");
  }

  // Connect to WiFi
  Serial.print("[WIFI] Connecting to network...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print("."); 
  }
  Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

  // Setup Queue and MQTT
  mqtt.setServer(mqtt_server, 1883);
  avgQueue = xQueueCreate(10, sizeof(float));

  // Dispatch FreeRTOS Tasks
  Serial.println("[INIT] Dispatching FreeRTOS Tasks...");
  
  xTaskCreatePinnedToCore(TaskSensor, "Sensor", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskProcessFFT, "FFT", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskMQTT, "MQTT", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskLoRa, "LoRa", 8192, NULL, 3, NULL, 0); // Core 0 for precise radio timing
}

void loop() {
  // Empty loop as execution is fully handled by FreeRTOS tasks
  vTaskDelete(NULL); 
}