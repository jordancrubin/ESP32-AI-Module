#include "SpeakerManager.h"
#include <math.h>
#include "Audio.h"
#include <LittleFS.h>

SpeakerManager::SpeakerManager() {
    audio = nullptr;
    _mutex = xSemaphoreCreateRecursiveMutex();
}

void SpeakerManager::begin() {
    if (audio) return; // Guard: already initialized
    
    audio = new Audio();
    audio->setPinout(SPEAKER_SCK, SPEAKER_WS, SPEAKER_SD);
    audio->setVolume(21); // Native 0-21 range
    audio->setConnectionTimeout(5000, 5000); // Increase timeout for TTS generation/connection

    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        16384,          // Stack size
        this,
        20,             // Priority 20 (High)
        NULL,
        1               // Core 1
    );
    Serial.println("SpeakerManager: Audio Initialized (Direct Host Mode)");
}

void SpeakerManager::setVolume(int volume) {
    if (audio) audio->setVolume(volume);
}

void SpeakerManager::playSpeechFromFile(const char* filename) {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (audio) {
        audio->stopSong();
        Serial.printf("Playing TTS from file: %s\n", filename);
        Serial.flush();
        audio->connecttoFS(LittleFS, filename);
        _isPlaying = true;
    } else {
        Serial.println("SpeakerManager Error: Audio object is NULL!");
    }
    xSemaphoreGiveRecursive(_mutex);
}

void SpeakerManager::stop() {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (audio) audio->stopSong();
    _isPlaying = false;
    xSemaphoreGiveRecursive(_mutex);
}

void SpeakerManager::loop() {
    if (xSemaphoreTakeRecursive(_mutex, 0) == pdTRUE) {
        if (audio) {
            audio->loop();
            if (_isPlaying && !audio->isRunning()) {
                _isPlaying = false;
                LittleFS.remove("/speech.mp3"); // Clean up temp file to save space
            }
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

bool SpeakerManager::isRunning() {
    return _isPlaying || (audio && audio->isRunning());
}

void SpeakerManager::audioTask(void* parameter) {
    SpeakerManager* manager = static_cast<SpeakerManager*>(parameter);
    while (true) {
        if (manager->isRunning()) {
            manager->loop();
            vTaskDelay(1); // Yield to allow other tasks (like Serial/WiFi) to run
        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); // Sleep when idle
        }
    }
}