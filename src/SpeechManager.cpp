/*
  SpeechManager.cpp - ESP32 AI Module Audio Input
  Handles I2S microphone recording (INMP441) and buffer management.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/
#include "SpeechManager.h"

SpeechManager::SpeechManager() {}

void SpeechManager::begin() {
    setupI2S();
    Serial.println("SpeechManager: ICS-43434 I2S Initialized");
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

bool SpeechManager::detectWakeWord() {
    // Placeholder for actual ESP-SR processing
    // In a full implementation, this would read from I2S, feed the AFE, 
    // and check the Multinet output.
    
    // For now, we verify we can read data
    size_t bytes_read = 0;
    int32_t buffer[256];
    i2s_channel_read(rx_handle, buffer, sizeof(buffer), &bytes_read, 0); // 0 = Non-blocking
    
    if (bytes_read > 0) {
        // Audio data is flowing
        return false; 
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
    const int SILENCE_THRESHOLD = 800; // Amplitude threshold for "quiet" (after 32x gain)
    const unsigned long SILENCE_DURATION = 3000; // Stop after 3 seconds of silence

    while (samplesRead < numSamples) {
        i2s_channel_read(rx_handle, sampleBuffer, sizeof(sampleBuffer), &bytesRead, portMAX_DELAY);
        int samplesInBatch = bytesRead / 4; // Number of 32-bit samples
        
        bool voiceDetectedInBatch = false;
        // We are reading STEREO (L/R interleaved), but we only want LEFT channel (index 0, 2, 4...)
        // This fixes the "Slow Playback" issue caused by capturing both slots as mono data.
        for (int i = 0; i < samplesInBatch && samplesRead < numSamples; i += 2) {
            int32_t raw = sampleBuffer[i] >> 16; // Shift to get 16-bit
            
            // Software Gain (32x) to fix "Low Sensitivity"
            raw *= 32;
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