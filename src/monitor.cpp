#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

Adafruit_INA219 ina219;
bool inaOk = false;

bool   adaptive  = false;
float  ovsPwr = 0, adpPwr = 0;
float  ovsEnergy = 0, adpEnergy = 0;
int    ovsN = 0, adpN = 0;
unsigned long lastMs = 0;

void setup() {
    Serial.begin(115200);
    Serial1.begin(115200, SERIAL_8N1, 4, 5); // Riceve dal Target (Assicurati che il TX del Target sia sul Pin 4 del Monitor)
    delay(1500);
    Wire.begin(41, 42);
    inaOk = ina219.begin();
    Serial.println(inaOk ? "[INA219] Sensore Connesso" : "[INA219] ERRORE: Sensore NON TROVATO");
    lastMs = millis();
}

void loop() {
    // Intercetta l'output del Target
    while (Serial1.available()) {
        static char line[64]; static int pos = 0;
        char c = Serial1.read();
        
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                line[pos] = '\0'; pos = 0;
                
                // Cerca la stringa esatta stampata dal Target all'inizio del blocco finestra
                if (strncmp(line, "PHASE:ADAPTIVE", 14) == 0) {
                    if (!adaptive) { adaptive = true; Serial.println("\n[MONITOR] Cambio Fase Rilevato -> ADAPTIVE"); }
                } 
                else if (strncmp(line, "PHASE:OVERSAMPLE", 16) == 0) {
                    if (adaptive) { adaptive = false; Serial.println("\n[MONITOR] Cambio Fase Rilevato -> OVERSAMPLE"); }
                } 
                else {
                    // Stampa i log normali del target
                    Serial.println(line);
                }
            }
        } else if (pos < 63) {
            line[pos++] = c;
        }
    }

    if (!inaOk) { delay(1); return; }

    unsigned long now = millis();
    if (now - lastMs < 1000) { delay(1); return; }
    
    float dt = (now - lastMs) / 1000.0f;
    lastMs = now;

    float mA = ina219.getCurrent_mA();
    float mW = ina219.getPower_mW();
    float mJ = mW * dt;

    Serial.printf("[INA219] Fase=%-10s | %6.1f mA | %6.1f mW | %7.3f mJ\n",
                  adaptive ? "ADAPTIVE" : "OVERSAMPLE", mA, mW, mJ);

    if (adaptive) {
        adpN++; adpEnergy += mJ; adpPwr += (mW - adpPwr) / adpN;
    } else {
        ovsN++; ovsEnergy += mJ; ovsPwr += (mW - ovsPwr) / ovsN;
    }

    // Stampa il riassunto ogni 30 secondi
    if ((ovsN + adpN) % 30 == 0 && (ovsN + adpN) > 0) {
        Serial.println("\n================ ENERGY SUMMARY ================");
        Serial.printf("  OVERSAMPLE: %.2f mW medi | %.2f mJ totali | %d campioni\n", ovsPwr, ovsEnergy, ovsN);
        Serial.printf("  ADAPTIVE  : %.2f mW medi | %.2f mJ totali | %d campioni\n", adpPwr, adpEnergy, adpN);
        if (ovsN > 0 && adpN > 0) {
            Serial.printf("  Risparmio Energetico: %.1f%%\n", 100.0f * (1.0f - adpPwr/ovsPwr));
        }
        Serial.println("================================================\n");
    }
}