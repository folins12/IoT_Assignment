#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// CONFIGURATION
#define INA219_SDA_PIN      41
#define INA219_SCL_PIN      42
#define SERIAL1_RX_PIN       4
#define SERIAL1_TX_PIN       5
#define SERIAL1_BAUD    115200
#define POWER_SAMPLE_INTERVAL_MS  1000

// Phase tags expected from the Target Node (must match definitions in main.cpp)
#define TAG_OVERSAMPLE  "PHASE:OVERSAMPLE"
#define TAG_ADAPTIVE    "PHASE:ADAPTIVE"

Adafruit_INA219 ina219;
bool            ina219Ok   = false;
static char  g_phase[32]         = "OVERSAMPLE";  // initial assumption
static bool  g_phaseChanged      = false;

// Energy accumulators per phase (mJ = mW * s)
static float g_energyOversampled_mJ = 0.0f;
static float g_energyAdaptive_mJ    = 0.0f;
static int   g_samplesOversample    = 0;
static int   g_samplesAdaptive      = 0;
static float g_avgPowerOversample   = 0.0f;  // running mean
static float g_avgPowerAdaptive     = 0.0f;

// For timing the power sampling interval precisely
static unsigned long g_lastPowerMs = 0;

// Line buffer for Serial1 input
static char  g_lineBuf[64];
static int   g_linePos = 0;

// HELPERS

// Update running mean: new_mean = old_mean + (x - old_mean) / n
static float runningMean(float oldMean, float x, int n) {
    return oldMean + (x - oldMean) / (float)n;
}

// Parse a line received from the Target Node and update phase if it is a tag.
// Returns true if the line was a phase tag (and should not be forwarded).
static bool handlePhaseLine(const char* line) {
    if (strcmp(line, TAG_OVERSAMPLE) == 0) {
        if (strcmp(g_phase, "OVERSAMPLE") != 0) {
            strncpy(g_phase, "OVERSAMPLE", sizeof(g_phase) - 1);
            g_phaseChanged = true;
            Serial.println("\n[MONITOR] Phase -> OVERSAMPLED");
        }
        return true;
    }
    if (strcmp(line, TAG_ADAPTIVE) == 0) {
        if (strcmp(g_phase, "ADAPTIVE") != 0) {
            strncpy(g_phase, "ADAPTIVE", sizeof(g_phase) - 1);
            g_phaseChanged = true;
            Serial.println("\n[MONITOR] Phase -> ADAPTIVE (FFT-optimised)");
        }
        return true;
    }
    return false;
}

// SETUP
void setup() {
    Serial.begin(115200);
    Serial1.begin(SERIAL1_BAUD, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);

    delay(2000);
    Serial.println("\n==============================================");
    Serial.println("=== IoT Monitor Node – Booting ===");
    Serial.println("==============================================\n");

    Wire.begin(INA219_SDA_PIN, INA219_SCL_PIN);
    if (ina219.begin()) {
        ina219Ok = true;
        Serial.println("[INIT] INA219 found – power monitoring active.");
    } else {
        Serial.println("[ERROR] INA219 not found – check wiring (SDA=41, SCL=42).");
    }

    g_lastPowerMs = millis();
    Serial.println("[INIT] Forwarding Target Node serial output below.\n");
    Serial.println("----------------------------------------------");
}

// LOOP
void loop() {
    while (Serial1.available()) {
        char c = (char)Serial1.read();

        if (c == '\n' || c == '\r') {
            if (g_linePos > 0) {
                g_lineBuf[g_linePos] = '\0';
                g_linePos = 0;

                if (!handlePhaseLine(g_lineBuf)) {
                    Serial.println(g_lineBuf);
                }
            }
        } else {
            if (g_linePos < (int)sizeof(g_lineBuf) - 2) {
                g_lineBuf[g_linePos++] = c;
            }
        }
    }

    // 2. Sample INA219 at POWER_SAMPLE_INTERVAL_MS intervals.
    if (!ina219Ok) { delay(1); return; }

    unsigned long now_ms = millis();
    if (now_ms - g_lastPowerMs < POWER_SAMPLE_INTERVAL_MS) {
        delay(1);
        return;
    }
    float dtSec = (float)(now_ms - g_lastPowerMs) / 1000.0f;
    g_lastPowerMs = now_ms;

    float current_mA = ina219.getCurrent_mA();
    float power_mW   = ina219.getPower_mW();

    // Energy contributed in this interval [mJ]
    float energy_mJ = power_mW * dtSec;

    // Accumulate into the correct phase bucket
    bool isAdaptive = (strcmp(g_phase, "ADAPTIVE") == 0);
    if (isAdaptive) {
        g_energyAdaptive_mJ += energy_mJ;
        g_samplesAdaptive++;
        g_avgPowerAdaptive = runningMean(g_avgPowerAdaptive, power_mW,
                                         g_samplesAdaptive);
    } else {
        g_energyOversampled_mJ += energy_mJ;
        g_samplesOversample++;
        g_avgPowerOversample = runningMean(g_avgPowerOversample, power_mW,
                                            g_samplesOversample);
    }

    // 3. Print the power sample, annotated with the current phase.
    Serial.printf("[INA219] Phase=%-11s | Current: %7.2f mA | Power: %7.2f mW"
                  " | Energy (this tick): %.3f mJ\n",
                  g_phase, current_mA, power_mW, energy_mJ);

    // Print phase-transition summary when phase just changed
    if (g_phaseChanged) {
        g_phaseChanged = false;
        Serial.println("[MONITOR] --- Phase transition detected ---");
        if (g_samplesOversample > 0)
            Serial.printf("[MONITOR] OVERSAMPLE phase: avg %.2f mW | "
                          "total energy %.3f mJ (%d samples)\n",
                          g_avgPowerOversample, g_energyOversampled_mJ,
                          g_samplesOversample);
        if (g_samplesAdaptive > 0)
            Serial.printf("[MONITOR] ADAPTIVE    phase: avg %.2f mW | "
                          "total energy %.3f mJ (%d samples)\n",
                          g_avgPowerAdaptive, g_energyAdaptive_mJ,
                          g_samplesAdaptive);
    }

    // Periodically print the full energy comparison summary (every 30 samples)
    int totalSamples = g_samplesOversample + g_samplesAdaptive;
    if (totalSamples > 0 && totalSamples % 30 == 0) {
        Serial.println("\n[MONITOR] ====== Energy Summary ======");
        Serial.printf("  OVERSAMPLE: avg %7.2f mW | total %9.3f mJ | %d samples\n",
                      g_avgPowerOversample, g_energyOversampled_mJ,
                      g_samplesOversample);
        Serial.printf("  ADAPTIVE  : avg %7.2f mW | total %9.3f mJ | %d samples\n",
                      g_avgPowerAdaptive, g_energyAdaptive_mJ,
                      g_samplesAdaptive);

        if (g_samplesOversample > 0 && g_samplesAdaptive > 0) {
            float saving_pct = 100.0f * (1.0f - g_avgPowerAdaptive
                                                / g_avgPowerOversample);
            Serial.printf("  Power saving (adaptive vs oversample): %.1f%%\n",
                          saving_pct);
        }
        Serial.println("[MONITOR] ==============================\n");
    }
}