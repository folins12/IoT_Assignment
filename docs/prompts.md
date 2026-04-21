# Point 8: Use of an LLM

## Prompts Issued

1. **(RadioLib OTAA Setup):** *"I am using the RadioLib library version 6.6.0 on an ESP32. I need to connect to The Things Network using OTAA. TTN provides me with a joinEUI, devEUI, and AppKey. Can you show me the exact syntax for initializing the `LoRaWANNode` and calling `beginOTAA` with these keys?"*
2. **(FreeRTOS Queues):** *"I have a FreeRTOS architecture on an ESP32. Task 1 samples a sensor at 100Hz. Task 2 computes an FFT and an average every 5 seconds. What is the most efficient way to pass the high-frequency float data from Task 1 to Task 2 without causing Queue overflows or blocking the 100Hz sampling loop?"*
3. **(arduinoFFT Adaptation):** *"I am using the `arduinoFFT` library (version 2.0+). I have an array of 128 samples. How do I apply a Hamming window and compute the forward FFT to find the major peak frequency?"*
4. **(C++ DSP Optimization):** *"I am implementing a Hampel filter on an ESP32. I need to find the median of a sliding window of size 5, 15, and 31. What is the fastest way in C++ to find the median of a small local float array without dynamically allocating memory on the heap?"*

## Commentary on LLM Quality, Opportunities, and Limitations

**Quality of Assistance:**
The LLM was exceptionally helpful in bridging the gap between outdated online tutorials and modern library versions (especially for `RadioLib` and `arduinoFFT`). It provided rapid, context-aware boilerplate code for FreeRTOS queues, which drastically reduced the time spent wrestling with syntax errors.

**Opportunities:**
The greatest advantage was mathematical and algorithmic brainstorming. When implementing the Hampel filter, the LLM suggested using `std::sort` on small, statically allocated arrays instead of complex heap-based median algorithms. This insight made the Hampel filter surprisingly fast and safe for embedded environments.

**Limitations:**
1. **Contextual Hardware Blindness:** When asked to optimize the Z-Score filter, the LLM suggested standard mathematical formulas using `pow()`. It failed to warn me that calling `pow()` repeatedly on an ESP32 without an FPU optimized for that specific instruction is computationally expensive. I had to discover through empirical logging that my Hampel filter (sorting) was actually executing faster than the Z-Score filter (math).
2. **Library Version Hallucinations:** The LLM frequently hallucinated older syntaxes for the `RadioLib` LoRaWAN implementation. TTN changed how keys are handled, and RadioLib updated its class structures. The LLM provided confidently incorrect code that wouldn't compile, forcing me to rely heavily on the official library source code and examples to fix the OTAA join process.