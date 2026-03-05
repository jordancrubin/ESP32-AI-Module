#pragma once
#include <Arduino.h>
#include <driver/i2s_std.h>
#include "Config.h"
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

class SpeechManager {
public:
    SpeechManager();
    void begin();
    bool detectWakeWord(float threshold);
    uint8_t* record(int durationMs, size_t* outSize, int silenceThreshold);
    void feedReference(const int16_t *data, size_t samples);

private:
    void setupI2S();
    i2s_chan_handle_t rx_handle = NULL; // Handle for the new I2S driver

    // Edge Impulse Inference
    float *inference_buffer = nullptr;
    size_t inference_buf_ptr = 0;
};

struct WavHeader {
    char riff[4];           // "RIFF"
    uint32_t overallSize;   // filesize - 8
    char wave[4];           // "WAVE"
    char fmtChunkMarker[4]; // "fmt "
    uint32_t lengthOfFmt;   // 16
    uint16_t formatType;    // 1 (PCM)
    uint16_t channels;      // 1
    uint32_t sampleRate;    // 16000
    uint32_t byteRate;      // 32000
    uint16_t blockAlign;    // 2
    uint16_t bitsPerSample; // 16
    char dataChunkHeader[4];// "data"
    uint32_t dataSize;      // data size
};