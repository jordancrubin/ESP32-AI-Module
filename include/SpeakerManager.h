#pragma once
#include <Arduino.h>
#include "Config.h"
#include <driver/i2s_std.h>

class SpeakerManager {
public:
    SpeakerManager();
    void begin();
    void playSpeechFromFile(const char* filename);
    void setVolume(int volume);
    void stop();
    void loop();
    bool isRunning();

private:
    static void audioTask(void* parameter);
    SemaphoreHandle_t _mutex;
    volatile bool _isPlaying = false;
    i2s_chan_handle_t tx_handle = NULL; // I2S Transmit Handle
};