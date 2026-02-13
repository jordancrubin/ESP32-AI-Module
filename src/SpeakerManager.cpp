#include "SpeakerManager.h"
#include <math.h>

struct __attribute__((packed)) WavHeader {
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

static int _currentVolume = 21;

SpeakerManager::SpeakerManager() {
    generator = nullptr;
    file = nullptr;
    out = nullptr;
    mp3Buffer = nullptr;
    _mutex = xSemaphoreCreateRecursiveMutex();
}

void SpeakerManager::begin() {
    if (out) return; // Guard: already initialized
    // Initialize I2S Output on Port 0 (Default)
    out = new AudioOutputI2S(); 
    out->SetPinout(SPEAKER_SCK, SPEAKER_WS, SPEAKER_SD);
    out->SetGain(1.0); // Volume 0.0 to 4.0 (1.0 is standard)
    // out->SetOutputModeMono(true); // Uncomment if you need to force mono mixing

    // Create a task on Core 0 specifically for Audio to prevent crackling
    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        16384,          // Stack size
        this,
        20,             // Priority 20 (High)
        NULL,
        1               // Core 1
    );
    Serial.println("SpeakerManager: Audio Initialized (ESP8266Audio)");
}

void SpeakerManager::setVolume(int volume) {
    _currentVolume = volume;
    // Map 0-21 to 0.0-1.0. Standard software gain range to prevent distortion.
    float gain = (float)volume / 21.0f; 
    Serial.printf("SpeakerManager: Setting Gain to %.2f\n", gain);
    if (out) out->SetGain(gain);
}

void SpeakerManager::playAudioFromRAM(uint8_t* data, size_t size) {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    stop(); // Stop current playback and free previous buffer

    Serial.printf("Playing audio from RAM. Size: %u\n", size);
    mp3Buffer = data; // Store pointer to free later
    
    file = new AudioFileSourcePROGMEM(mp3Buffer, size);
    generator = new AudioGeneratorMP3();
    if (out) out->SetGain((float)_currentVolume / 21.0f);
    generator->begin(file, out);
    _isPlaying = true;
    xSemaphoreGiveRecursive(_mutex);
}

void SpeakerManager::playTone(int freq, int duration_ms) {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    stop();

    Serial.printf("Tone: %d Hz (%d ms) [44.1kHz Stereo]\n", freq, duration_ms);

    // Generate WAV in RAM
    uint32_t sampleRate = 44100; // Use standard rate to avoid I2S clock glitches
    uint32_t numSamples = (sampleRate * duration_ms) / 1000;
    uint16_t channels = 2;       // Use Stereo for better DAC compatibility
    uint32_t dataSize = numSamples * channels * 2; // 2 bytes per sample * 2 channels
    uint32_t fileSize = sizeof(WavHeader) + dataSize;

    // Use internal RAM for tones to avoid PSRAM bus noise.
    // This ensures the volume "beep" stays clean even during heavy memory usage.
    mp3Buffer = (uint8_t*)malloc(fileSize);
    if (!mp3Buffer) mp3Buffer = (uint8_t*)ps_malloc(fileSize);

    if (!mp3Buffer) {
        Serial.println("Tone Error: PSRAM Allocation Failed");
        xSemaphoreGiveRecursive(_mutex);
        return;
    }

    WavHeader header;
    memcpy(header.riff, "RIFF", 4);
    header.overallSize = fileSize - 8;
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.fmtChunkMarker, "fmt ", 4);
    header.lengthOfFmt = 16;
    header.formatType = 1; // PCM
    header.channels = channels;
    header.sampleRate = sampleRate;
    header.byteRate = sampleRate * channels * 2;
    header.blockAlign = channels * 2;
    header.bitsPerSample = 16;
    memcpy(header.dataChunkHeader, "data", 4);
    header.dataSize = dataSize;
    
    memcpy(mp3Buffer, &header, sizeof(WavHeader));

    int16_t *pcm = (int16_t*)(mp3Buffer + sizeof(WavHeader));
    // Manually scale tone amplitude by volume as a fail-safe.
    // This ensures the tone volume works even if library gain is buggy.
    float volScale = (float)_currentVolume / 21.0f;
    for (uint32_t i = 0; i < numSamples; i++) {
        // Generate sine wave and duplicate for Left/Right channels
        int16_t val = (int16_t)(sin(2.0 * M_PI * freq * i / sampleRate) * 30000.0f * volScale);
        pcm[i * 2] = val;     // Left
        pcm[i * 2 + 1] = val; // Right
    }

    file = new AudioFileSourcePROGMEM(mp3Buffer, fileSize);
    generator = new AudioGeneratorWAV();
    if (out) out->SetGain((float)_currentVolume / 21.0f);
    if (generator->begin(file, out)) {
        _isPlaying = true;
    } else {
        Serial.println("Tone Error: Generator Begin Failed");
        stop();
    }
    xSemaphoreGiveRecursive(_mutex);
}

void SpeakerManager::stop() {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (generator) {
        if (generator->isRunning()) generator->stop();
        delete generator;
        generator = nullptr;
    }
    if (file) {
        file->close();
        delete file;
        file = nullptr;
    }
    if (mp3Buffer) {
        free(mp3Buffer);
        mp3Buffer = nullptr;
    }
    _isPlaying = false;
    xSemaphoreGiveRecursive(_mutex);
}

void SpeakerManager::loop() {
    if (generator && generator->isRunning()) {
        if (!generator->loop()) {
            generator->stop();
            _isPlaying = false;
        }
    }
}

bool SpeakerManager::isRunning() {
    return generator && generator->isRunning();
}

void SpeakerManager::audioTask(void* parameter) {
    SpeakerManager* manager = static_cast<SpeakerManager*>(parameter);
    while (true) {
        if (!manager->_isPlaying) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (xSemaphoreTakeRecursive(manager->_mutex, portMAX_DELAY) == pdTRUE) {
            if (manager->isRunning()) {
                manager->loop();
            } else {
                manager->_isPlaying = false; // Sync flag
            }
            xSemaphoreGiveRecursive(manager->_mutex);
        }
        // No delay here while playing; AudioOutputI2S blocks internally to match sample rate
    }
}