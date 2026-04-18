# Point 8: Use of an LLM

## Prompts Issued to Achieve Outcome
To follow the course's academic integrity rules, I used the LLM as a study guide and a debugging helper. I did not ask it to write the final solutions for me; the final code and system design were done manually.

1. **(LoRaWAN Setup):** *"I'm trying to connect my Heltec V3 to The Things Network (TTN) using RadioLib. TTN gives me AppEUI, DevEUI, and AppKey. But RadioLib's `beginOTAA` asks for four variables: joinEUI, devEUI, nwkKey, and appKey. How do I match my 3 TTN keys to these 4 variables?"*
2. **(RTOS Queues):** *"In FreeRTOS, if I have a fast task reading a sensor at 100Hz and a slow task calculating an average every 5 seconds, how do I safely send data between them without freezing the fast task?"*
3. **(Bonus - Anomaly Filtering):** *"I need to filter out random, huge spikes from a noisy sine wave. I'm looking at Z-score and Hampel filters. Which one is better for catching really big spikes, and how different is the CPU cost between calculating a mean versus a median?"*
4. **(DSP Bug):** *"I'm using the `arduinoFFT` library. The first time it runs, it correctly finds 8Hz. Ten seconds later, it outputs 124Hz even though my wave is exactly the same. Does the FFT function overwrite my original data array?"*
5. **(Hardware Bug):** *"My ESP32 serial monitor freezes for exactly 5 seconds right when my LoRa module sends a message to TTN. The code doesn't crash, it just stops. Could this be a buffer issue, or is the radio antenna messing with the USB connection?"*

## Commentary on LLM Quality, Opportunities, and Limitations

**Quality of the Assistance:**
The LLM (Gemini) was a great learning partner. Instead of just handing me code, it explained the computer science concepts behind my problems—like how FreeRTOS queues manage memory, or why finding a median (for the Hampel filter) uses so much more CPU time than just finding an average (for the Z-score filter).

**Opportunities:**
The best part about using the LLM was saving time with outdated libraries. For example, `RadioLib` recently updated to version 6.6.0, changing how it handles keys from arrays to 64-bit integers. Most tutorials online are broken now, but the LLM quickly helped me fix the old syntax. It was also great for bouncing ideas around, like figuring out that `arduinoFFT` overwrites its own input arrays. That hint helped me add the right RTOS flags to protect my data buffers.

**Limitations:**
1. **Hardware Blindness:** The LLM sometimes assumed things about my board that weren't true. At first, it suggested using a built-in DAC to make the analog signal. That works on older ESP32s, but not on my ESP32-S3. I had to realize this on my own and write a math-based signal generator instead.
2. **It Can't See the Real World:** The LLM is good at software, but it can't see a physical breadboard. When my USB connection kept dropping during LoRa transmissions, the LLM suggested software fixes like increasing the Serial buffer. It took real-world human troubleshooting to realize the issue was actually Electromagnetic Interference (EMI) from my antenna being too close to the USB chip.
3. **Textbook Theory vs. Reality:** When I asked about adaptive sampling, the LLM suggested sampling at exactly 2.0x the max frequency (the strict Nyquist limit). In the real world, this caused aliasing because my samples perfectly aligned with the wave's zero-crossings (the wave "disappeared"). I had to manually change the code to a 2.5x safety margin to make the system actually work. This showed me that LLMs often stick to textbook math and miss practical engineering tricks.