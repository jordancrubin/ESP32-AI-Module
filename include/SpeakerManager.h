#pragma once
#include <Arduino.h>
#include "Audio.h"
#include "Config.h"

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
    Audio *audio;
    static void audioTask(void* parameter);
    SemaphoreHandle_t _mutex;
    volatile bool _isPlaying = false;
};