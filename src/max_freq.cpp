#include <Arduino.h>

#define SENSOR_PIN 4

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  pinMode(SENSOR_PIN, INPUT);
  
  Serial.println("Starting ADC Max Frequency Test...");
  
  unsigned long start_time = micros();
  long samples = 100000;
  
  for(long i = 0; i < samples; i++) {
    volatile int val = analogRead(SENSOR_PIN);
  }
  
  unsigned long end_time = micros();
  float duration_seconds = (end_time - start_time) / 1000000.0;
  float max_freq = samples / duration_seconds;
  
  Serial.printf("Time taken for %ld samples: %f seconds\n", samples, duration_seconds);
  Serial.printf("MAXIMUM SAMPLING FREQUENCY: %.2f Hz\n", max_freq);
}

void loop() {
  delay(10000);
}