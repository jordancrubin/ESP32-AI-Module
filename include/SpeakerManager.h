#pragma once
#include <Arduino.h>
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "AudioFileSourcePROGMEM.h"
#include "Config.h"

class SpeakerManager {
public:
    SpeakerManager();
    void begin();
    void playAudioFromRAM(uint8_t* data, size_t size);
    void playTone(int freq, int duration_ms);
    void setVolume(int volume);
    void stop();
    void loop();
    bool isRunning();

private:
    AudioGenerator *generator;
    AudioFileSource *file;
    AudioOutputI2S *out;
    uint8_t *mp3Buffer;
    static void audioTask(void* parameter);
    SemaphoreHandle_t _mutex;
    volatile bool _isPlaying = false;
};