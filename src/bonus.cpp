#include "bonus.h"
#include <arduinoFFT.h>
#include <algorithm>

#define SAMPLES 128

// UTILS
double getLocalDominantFreq(float* input_signal, int freq) {
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

float getGaussianNoise(float mu, float sigma) {
    float u1 = max(0.0001f, (float)random(10000) / 10000.0f);
    float u2 = max(0.0001f, (float)random(10000) / 10000.0f);
    return (sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2)) * sigma + mu;
}

float calculateMER(float* filtered, float* raw, float* clean, int length) {
    float raw_err = 0, filt_err = 0;
    for (int i = 0; i < length; i++) {
        raw_err += abs(raw[i] - clean[i]);
        filt_err += abs(filtered[i] - clean[i]);
    }
    return (raw_err == 0) ? 0 : ((raw_err - filt_err) / raw_err) * 100.0;
}

void TaskBonusSweep(void *pvParameters) {
    float probs[] = {0.01, 0.05, 0.10};
    int windows[] = {5, 15, 31};
    
    float clean_sig[SAMPLES], raw_sig[SAMPLES], filt_sig[SAMPLES];
    bool is_anom[SAMPLES]; 
    vTaskDelay(pdMS_TO_TICKS(15000)); 

    for(;;) {
        for (float P : probs) {
            for (int W : windows) {
                for(int iter=0; iter<5; iter++) {
                    int total_anomalies = 0;
                    
                    // Generate Signal
                    for (int i = 0; i < SAMPLES; i++) {
                        float t = i / 100.0; 
                        
                        // signal (Mode 2)
                        clean_sig[i] = 3.0 * sin(2 * PI * 4 * t) + 1.5 * sin(2 * PI * 3 * t);
                        float noise = getGaussianNoise(0.0, 0.2);
                        
                        // Anomaly injection
                        is_anom[i] = (random(10000) < (P * 10000));
                        
                        float spike;
                        if (is_anom[i]) {
                            float value = random(500, 1500) / 100.0;
                            if (random(2)) {
                                spike = value;
                            } else {
                                spike = -value;
                            }
                        } else {
                            spike = 0;
                        }
                        
                        raw_sig[i] = clean_sig[i] + noise + spike;
                        if(is_anom[i]) total_anomalies++;
                    }

                    // ------------------------------------ UNFILTERDE ------------------------------------
                    double peak_unfiltered = getLocalDominantFreq(raw_sig, 100);

                    Serial.println("\n[BONUS 8.2] -------------------------------------");
                    Serial.printf("Filter Benchmark | Prob: %.2f | Window: %d\n", P, W);
                    Serial.printf("> Unfiltered Peak : %5.2f Hz\n", peak_unfiltered);


                    // ------------------------------------ Z-SCORE FILTrE ------------------------------------
                    int z_tp = 0, z_fp = 0;
                    unsigned long z_start = micros();
                    for (int i = 0; i < SAMPLES; i++) {
                        int start_idx = max(0, i - W / 2);
                        int end_idx = min(SAMPLES - 1, i + W / 2);
                        int w_size = end_idx - start_idx + 1;
                        
                        float sum = 0, var_sum = 0;
                        for (int j = start_idx; j <= end_idx; j++) sum += raw_sig[j];
                        float mean = sum / w_size;
                        for (int j = start_idx; j <= end_idx; j++) {
                            float diff = raw_sig[j] - mean;
                            var_sum += (diff * diff); 
                        }
                        float std_dev = sqrt(var_sum / w_size);

                        if (abs(raw_sig[i] - mean) > (3.0 * std_dev)) {
                            filt_sig[i] = mean;
                            is_anom[i] ? z_tp++ : z_fp++;
                        } else filt_sig[i] = raw_sig[i];
                    }
                    unsigned long z_time = micros() - z_start;
                    
                    float z_mer = calculateMER(filt_sig, raw_sig, clean_sig, SAMPLES);
                    double peak_z = getLocalDominantFreq(filt_sig, 100);
                    float z_tpr = total_anomalies > 0 ? (float)z_tp / total_anomalies : 0;
                    float z_fpr = (SAMPLES - total_anomalies) > 0 ? (float)z_fp / (SAMPLES - total_anomalies) : 0;
                    
                    Serial.printf("> Z-Score Filter  : Exec %4lu us | TPR %.2f | FPR %.2f | MER %5.1f%% | Peak %5.2f Hz\n", z_time, z_tpr, z_fpr, z_mer, peak_z);


                    // ------------------------------------ HAMPEL FILTER ------------------------------------
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
                    double peak_h = getLocalDominantFreq(filt_sig, 100);
                    float h_tpr = total_anomalies > 0 ? (float)h_tp / total_anomalies : 0;
                    float h_fpr = (SAMPLES - total_anomalies) > 0 ? (float)h_fp / (SAMPLES - total_anomalies) : 0;
                    
                    Serial.printf("> Hampel Filter   : Exec %4lu us | TPR %.2f | FPR %.2f | MER %5.1f%% | Peak %5.2f Hz\n", h_time, h_tpr, h_fpr, h_mer, peak_h);

                    vTaskDelay(pdMS_TO_TICKS(1500));
                }
            }
        }
    }
}