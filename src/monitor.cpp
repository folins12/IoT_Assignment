#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

Adafruit_INA219 ina219;
bool ina219_connected = false;

unsigned long last_print_time = 0;
const unsigned long print_interval = 5000;

void setup() {
  Serial.begin(115200);

  delay(2000);
  Serial.println("\n========================================");
  Serial.println("--- Monitor Booting ---");
  Serial.println("========================================");

  Wire.begin(41, 42); 
  if (ina219.begin()) {
    ina219_connected = true;
    Serial.println("[INIT] INA219 Power Sensor Connected.");
  } else {
    Serial.println("[ERROR] INA219 not found. Check wiring!");
  }
}

void loop() {
  if (ina219_connected) {
    unsigned long current_millis = millis();

    if (current_millis - last_print_time >= print_interval) {
      float current_mA = ina219.getCurrent_mA();
      float power_mW = ina219.getPower_mW();

      Serial.println("\n[INA219] --- Energy Consumption Report ---");
      Serial.printf("> Current: %.2f mA\n", current_mA);
      Serial.printf("> Power  : %.2f mW\n", power_mW);
      Serial.println("------------------------------------------");
      
      last_print_time = current_millis;
    }
  }  
  delay(1); 
}