/*
  SpeechManager.cpp - ESP32 AI Module Audio Input
  Handles I2S microphone recording (INMP441) and buffer management.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/

#include "SpeechManager.h"
#include <ESP-ai-wakeword_inferencing.h>

// Pointer to the buffer for the static callback
static float *s_inference_buffer = nullptr;

// Callback function for Edge Impulse to fetch data
static int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
    memcpy(out_ptr, s_inference_buffer + offset, length * sizeof(float));
    return 0;
}

SpeechManager::SpeechManager() {}

void SpeechManager::begin() {
    setupI2S();
    run_classifier_init();

    // Allocate inference buffer in PSRAM
    inference_buffer = (float*)ps_malloc(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));
    s_inference_buffer = inference_buffer;
    
    if (!inference_buffer) {
        Serial.println("SpeechManager: Failed to allocate inference buffer!");
    }

    Serial.println("SpeechManager: ICS-43434 I2S Initialized");
    Serial.printf("Edge Impulse: %d classification labels loaded.\n", EI_CLASSIFIER_LABEL_COUNT);
    Serial.printf("Edge Impulse: Expected Sample Rate: %d Hz\n", EI_CLASSIFIER_FREQUENCY);
}

void SpeechManager::setupI2S() {
    // New I2S Driver (ESP-IDF 5.x / Arduino 3.x) configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &rx_handle);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_SCK,
            .ws = (gpio_num_t)I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)I2S_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    i2s_channel_init_std_mode(rx_handle, &std_cfg);
    i2s_channel_enable(rx_handle);
}

bool SpeechManager::detectWakeWord(float threshold) {
    if (!inference_buffer) return false;

    // 1. Read available audio data from I2S (Non-blocking)
    size_t bytes_read = 0;
    int32_t i2s_buffer[256]; // Temporary buffer for raw I2S data
    // Use small timeout (10ms) to ensure we get a full chunk of aligned data
    i2s_channel_read(rx_handle, i2s_buffer, sizeof(i2s_buffer), &bytes_read, pdMS_TO_TICKS(10));
    
    if (bytes_read == 0) return false;

    // Debug: Print raw I2S data periodically to verify hardware input
    // static unsigned long last_raw_debug = 0;
    // if (millis() - last_raw_debug > 2000 && bytes_read >= 8) {
    //     Serial.printf("Raw I2S Hex: L=0x%08X R=0x%08X\n", i2s_buffer[0], i2s_buffer[1]);
    //     // Warn if we are reading partial frames (desync risk)
    //     if (bytes_read % 8 != 0) {
    //         Serial.printf("WARNING: I2S read misaligned! Bytes: %d\n", bytes_read);
    //     }
    //     last_raw_debug = millis();
    // }

    // 2. Process samples and fill inference buffer
    int samplesRead = bytes_read / 4; // 32-bit samples
    
    // Use static to persist peak level across multiple calls until inference runs
    static int32_t max_audio_level = 0;
    static float dc_offset = 0.0f;
    static int32_t debug_min_val = 32767;
    static int32_t debug_max_val = -32768;

    for (int i = 0; i < samplesRead; i += 2) { // Stereo -> Mono (Left Channel)
        // Convert 32-bit int to float
        // BIT SHIFT: Slide the 32-bit data right by 12 bits (was 15).
        // This provides 8x gain to boost quiet microphones (INMP441/ICS-43434).
        int32_t raw = i2s_buffer[i] >> 14; 

        // Remove DC Offset (High-pass filter) to prevent early clipping
        // Adjusted to 0.995f (approx 13Hz cutoff) to preserve voice body
        dc_offset = (dc_offset * 0.995f) + ((float)raw * 0.005f);
        raw -= (int32_t)dc_offset;

        // Clamp to 16-bit range
        if (raw > 32767) raw = 32767;
        else if (raw < -32768) raw = -32768;

        if (abs(raw) > max_audio_level) max_audio_level = abs(raw);
        if (raw < debug_min_val) debug_min_val = raw;
        if (raw > debug_max_val) debug_max_val = raw;

        if (inference_buf_ptr < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
            inference_buffer[inference_buf_ptr++] = (float)raw;
        }

        // 3. Run Inference if buffer is full
        if (inference_buf_ptr >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
            
            // Note: Software AGC removed for continuous mode as it requires full window context.
            // run_classifier_continuous handles the sliding window internally.
            // Ensure your model is trained with data that matches your mic levels.

            signal_t signal;
            signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
            signal.get_data = &raw_feature_get_data;

            ei_impulse_result_t result = { 0 };

            // COMPREHENSIVE DEBUG: Enable Edge Impulse internal debug periodically
            // This prints DSP timings and Feature generation details to Serial
            bool debug_mode = false;

            // Run classifier
            EI_IMPULSE_ERROR res = run_classifier(&signal, &result, debug_mode);
            if (res != EI_IMPULSE_OK) {
                Serial.printf("ERR: Failed to run classifier (%d)\n", res);
                return false;
            }

            // Print Debug Info
            Serial.printf("Raw: %d | Time: %dms | ", max_audio_level, result.timing.dsp + result.timing.classification);
            for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                Serial.printf("%s: %.2f ", result.classification[ix].label, result.classification[ix].value);
            }
            Serial.println();
            
            // Reset level tracker for next window
            max_audio_level = 0;
            debug_min_val = 32767;
            debug_max_val = -32768;

            bool wake_word_detected = false;
            
            for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                if (result.classification[ix].value > threshold) {
                    const char* label = result.classification[ix].label;
                    if (strcmp(label, "noise") != 0 && strcmp(label, "unknown") != 0) {
                        Serial.printf(">>> WAKE WORD DETECTED: %s (%.2f) <<<\n", label, result.classification[ix].value);
                        wake_word_detected = true;
                    }
                }
            }
            
            if (wake_word_detected) {
                // Reset buffer pointer for next slice
                inference_buf_ptr = 0;
                return true;
            }
            
            // Slide buffer
            int slide = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE / 2;
            memmove(inference_buffer, inference_buffer + slide, (EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - slide) * sizeof(float));
            inference_buf_ptr -= slide;
        }
    }
    return false;
}

uint8_t* SpeechManager::record(int durationMs, size_t* outSize) {
    size_t sampleRate = 16000;
    size_t numSamples = (sampleRate * durationMs) / 1000;
    size_t dataSize = numSamples * 2; // 16-bit = 2 bytes per sample
    size_t fileSize = sizeof(WavHeader) + dataSize;

    // Allocate memory in PSRAM
    uint8_t* wavBuffer = (uint8_t*)ps_malloc(fileSize);
    if (!wavBuffer) {
        Serial.println("SpeechManager: Failed to allocate PSRAM for recording");
        return nullptr;
    }

    // Create WAV Header
    WavHeader header;
    memcpy(header.riff, "RIFF", 4);
    header.overallSize = fileSize - 8;
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.fmtChunkMarker, "fmt ", 4);
    header.lengthOfFmt = 16;
    header.formatType = 1; // PCM
    header.channels = 1;   // Mono
    header.sampleRate = sampleRate;
    header.byteRate = sampleRate * 2;
    header.blockAlign = 2;
    header.bitsPerSample = 16;
    memcpy(header.dataChunkHeader, "data", 4);
    header.dataSize = dataSize;

    // Copy header to buffer
    memcpy(wavBuffer, &header, sizeof(WavHeader));

    // Flush DMA buffer to remove stale audio (silence/noise from before button press)
    size_t bytesFlushed = 0;
    int32_t flushBuffer[128];
    while (true) {
        i2s_channel_read(rx_handle, flushBuffer, sizeof(flushBuffer), &bytesFlushed, 0);
        if (bytesFlushed == 0) break;
    }

    // Record Audio
    Serial.println("Recording...");
    size_t bytesRead;
    int32_t sampleBuffer[128]; // Temporary buffer for 32-bit I2S data (Stereo)
    int16_t* pcmBuffer = (int16_t*)(wavBuffer + sizeof(WavHeader));
    size_t samplesRead = 0;
    
    unsigned long silenceStart = millis();
    const int SILENCE_THRESHOLD = 50; // Amplitude threshold for "quiet" (after 2x gain)
    const unsigned long SILENCE_DURATION = 3000; // Stop after 3 seconds of silence
    float rec_dc_offset = 0.0f;

    while (samplesRead < numSamples) {
        i2s_channel_read(rx_handle, sampleBuffer, sizeof(sampleBuffer), &bytesRead, portMAX_DELAY);
        int samplesInBatch = bytesRead / 4; // Number of 32-bit samples
        
        bool voiceDetectedInBatch = false;
        // We are reading STEREO (L/R interleaved), but we only want LEFT channel (index 0, 2, 4...)
        // This fixes the "Slow Playback" issue caused by capturing both slots as mono data.
        for (int i = 0; i < samplesInBatch && samplesRead < numSamples; i += 2) {
            // Shift by 15 for safe 2x gain
            int32_t raw = sampleBuffer[i] >> 15; 
            
            // Remove DC Offset
            rec_dc_offset = (rec_dc_offset * 0.995f) + ((float)raw * 0.005f);
            raw -= (int32_t)rec_dc_offset;

            if (raw > 32767) raw = 32767;
            else if (raw < -32768) raw = -32768;
            
            if (abs(raw) > SILENCE_THRESHOLD) {
                voiceDetectedInBatch = true;
            }
            
            pcmBuffer[samplesRead++] = (int16_t)raw;
        }
        
        if (voiceDetectedInBatch) {
            silenceStart = millis(); // Reset timer if we hear something
        } else {
            if (millis() - silenceStart > SILENCE_DURATION) {
                Serial.println("Silence detected, stopping recording.");
                break;
            }
        }
    }
    
    Serial.println("Recording Complete.");
    
    // Update WAV Header with actual size (since we might have stopped early)
    size_t actualDataSize = samplesRead * 2;
    size_t actualFileSize = sizeof(WavHeader) + actualDataSize;
    WavHeader* headerPtr = (WavHeader*)wavBuffer;
    headerPtr->dataSize = actualDataSize;
    headerPtr->overallSize = actualFileSize - 8;
    
    *outSize = actualFileSize;
    return wavBuffer;
}