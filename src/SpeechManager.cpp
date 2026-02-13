#include "SpeechManager.h"

SpeechManager::SpeechManager() {}

void SpeechManager::begin() {
    setupI2S();
    Serial.println("SpeechManager: I2S Initialized");
}

void SpeechManager::setupI2S() {
    // New I2S Driver (ESP-IDF 5.x / Arduino 3.x) configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &rx_handle);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
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

    // Record Audio
    Serial.println("Recording...");
    size_t bytesRead;
    int32_t sampleBuffer[64]; // Temporary buffer for 32-bit I2S data
    int16_t* pcmBuffer = (int16_t*)(wavBuffer + sizeof(WavHeader));
    size_t samplesRead = 0;

    while (samplesRead < numSamples) {
        i2s_channel_read(rx_handle, sampleBuffer, sizeof(sampleBuffer), &bytesRead, portMAX_DELAY);
        int samplesInBatch = bytesRead / 4; // 4 bytes per 32-bit sample
        for (int i = 0; i < samplesInBatch && samplesRead < numSamples; i++) {
            // INMP441 sends 24-bit data MSB aligned in 32-bit word. Shift right 16 to get top 16 bits.
            pcmBuffer[samplesRead++] = (int16_t)(sampleBuffer[i] >> 16);
        }
    }
    
    Serial.println("Recording Complete.");
    *outSize = fileSize;
    return wavBuffer;
}