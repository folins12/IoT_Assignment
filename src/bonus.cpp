#include <Arduino.h>
#include <arduinoFFT.h>
#include <vector>
#include <algorithm>

// PARAMETERS
#define ANOMALY_PROB 0.01          // Test: 0.01 (1%), 0.05 (5%), 0.10 (10%)
#define FILTER_WINDOW 15           // Test: 5, 15, 31
#define THRESHOLD_MULTIPLIER 3.0   // 3-sigma rule

#define SAMPLES 128
#define SAMPLING_FREQ 100.0

double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);

float raw_signal[SAMPLES];
float clean_signal[SAMPLES];   
bool is_anomaly[SAMPLES];      
float filtered_signal[SAMPLES];

// Box-Muller transform for Gaussian Noise (sigma = 0.2)
float getGaussianNoise(float mu, float sigma) {
    float u1 = max(0.0001f, (float)random(10000) / 10000.0f);
    float u2 = max(0.0001f, (float)random(10000) / 10000.0f);
    float z0 = sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2);
    return z0 * sigma + mu;
}

void generateSignalBuffer() {
    for (int i = 0; i < SAMPLES; i++) {
        float t = i / SAMPLING_FREQ;
        // Base Signal: Max freq = 5Hz
        float base = 2.0 * sin(2 * PI * 3 * t) + 4.0 * sin(2 * PI * 5 * t);
        float noise = getGaussianNoise(0.0, 0.2);
        
        clean_signal[i] = base + noise;
        is_anomaly[i] = false;
        raw_signal[i] = clean_signal[i];

        // Anomaly Injection
        if ((random(10000) / 10000.0) < ANOMALY_PROB) {
            float spike = random(500, 1500) / 100.0; // U(5, 15)
            if (random(2) == 0) spike = -spike;
            raw_signal[i] += spike;
            is_anomaly[i] = true;
        }
    }
}

double getDominantFrequency(float* input_buffer) {
    for (int i = 0; i < SAMPLES; i++) {
        vReal[i] = input_buffer[i];
        vImag[i] = 0.0;
    }
    FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(FFT_FORWARD);
    FFT.complexToMagnitude();
    return FFT.majorPeak();
}

struct FilterMetrics {
    unsigned long exec_time_us;
    int true_positives;
    int false_positives;
    int total_anomalies;
};

FilterMetrics applyZScoreFilter() {
    FilterMetrics m = {0, 0, 0, 0};
    unsigned long start_time = micros();
    int half_win = FILTER_WINDOW / 2;

    for (int i = 0; i < SAMPLES; i++) {
        if (is_anomaly[i]) m.total_anomalies++;

        float sum = 0;
        int count = 0;
        for (int j = max(0, i - half_win); j <= min(SAMPLES - 1, i + half_win); j++) {
            sum += raw_signal[j];
            count++;
        }
        float mean = sum / count;

        float sq_sum = 0;
        for (int j = max(0, i - half_win); j <= min(SAMPLES - 1, i + half_win); j++) {
            sq_sum += pow(raw_signal[j] - mean, 2);
        }
        float stddev = sqrt(sq_sum / count);

        if (abs(raw_signal[i] - mean) > THRESHOLD_MULTIPLIER * stddev) {
            filtered_signal[i] = mean; 
            if (is_anomaly[i]) m.true_positives++;
            else m.false_positives++;
        } else {
            filtered_signal[i] = raw_signal[i];
        }
    }
    m.exec_time_us = micros() - start_time;
    return m;
}

FilterMetrics applyHampelFilter() {
    FilterMetrics m = {0, 0, 0, 0};
    unsigned long start_time = micros();
    int half_win = FILTER_WINDOW / 2;

    for (int i = 0; i < SAMPLES; i++) {
        if (is_anomaly[i]) m.total_anomalies++;

        std::vector<float> window;
        for (int j = max(0, i - half_win); j <= min(SAMPLES - 1, i + half_win); j++) {
            window.push_back(raw_signal[j]);
        }
        
        std::vector<float> sorted_window = window;
        std::sort(sorted_window.begin(), sorted_window.end());
        float median = sorted_window[sorted_window.size() / 2];

        std::vector<float> deviations;
        for (float val : window) {
            deviations.push_back(abs(val - median));
        }
        std::sort(deviations.begin(), deviations.end());
        float mad = deviations[deviations.size() / 2];
        
        // MAD * 1.4826 approximates standard deviation
        if (abs(raw_signal[i] - median) > THRESHOLD_MULTIPLIER * (mad * 1.4826)) {
            filtered_signal[i] = median;
            if (is_anomaly[i]) m.true_positives++;
            else m.false_positives++;
        } else {
            filtered_signal[i] = raw_signal[i];
        }
    }
    m.exec_time_us = micros() - start_time;
    return m;
}

void setup() {
    Serial.begin(115200);
    delay(2000);
}

void loop() {
    Serial.println("\n========================================");
    Serial.printf("PROFILING AT P=%.2f, WINDOW=%d\n", ANOMALY_PROB, FILTER_WINDOW);
    
    generateSignalBuffer();

    double peak_unfiltered = getDominantFrequency(raw_signal);
    Serial.printf("[UNFILTERED] FFT Peak: %.2f Hz\n", peak_unfiltered);

    FilterMetrics z = applyZScoreFilter();
    double peak_z = getDominantFrequency(filtered_signal);
    float z_tpr = z.total_anomalies > 0 ? (float)z.true_positives / z.total_anomalies : 0;
    
    Serial.printf("[Z-SCORE] Exec: %lu us | TPR: %.2f | FPR: %d | FFT Peak: %.2f Hz\n", 
                  z.exec_time_us, z_tpr, z.false_positives, peak_z);

    // Run Hampel and sync anomaly count
    FilterMetrics h = applyHampelFilter();
    h.total_anomalies = z.total_anomalies; 
    double peak_h = getDominantFrequency(filtered_signal);
    float h_tpr = h.total_anomalies > 0 ? (float)h.true_positives / h.total_anomalies : 0;

    Serial.printf("[HAMPEL]  Exec: %lu us | TPR: %.2f | FPR: %d | FFT Peak: %.2f Hz\n", 
                  h.exec_time_us, h_tpr, h.false_positives, peak_h);

    delay(5000);
}
