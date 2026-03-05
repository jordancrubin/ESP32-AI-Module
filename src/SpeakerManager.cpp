/*
  SpeakerManager.cpp - ESP32 AI Module Audio Output
  Manages I2S audio playback via MAX98357A or similar DACs.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/
#include "SpeakerManager.h"
#include <math.h>
#include <FS.h>
#include <LittleFS.h>
#include "MP3DecoderHelix.h"
#include "SpeechManager.h" // To feed reference signal

extern SpeechManager speech;
using namespace libhelix;

static File audioFile;
static float s_volume = 1.0f;
static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_std_config_t s_i2s_std_cfg; // Store the I2S standard config
static bool isWav = false;
static bool s_i2s_enabled = false;
static int s_fade_samples = 0;
const int FADE_LEN = 4000; // ~160ms fade-in at 24kHz

// Helix Decoder Callback
void dataCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void*) {
    // 1. Apply Volume & Fade-in
    for (size_t i = 0; i < len; i++) {
        float fade = 1.0f;
        if (s_fade_samples < FADE_LEN) {
            fade = (float)s_fade_samples / FADE_LEN;
            s_fade_samples++;
        }
        pcm_buffer[i] = (int16_t)(pcm_buffer[i] * s_volume * fade);
    }

    // 2. Write to I2S (Blocking if buffer full)
    // Dynamically adjust I2S sample rate if needed
    if (s_tx_handle && s_i2s_std_cfg.clk_cfg.sample_rate_hz != info.samprate) {
        Serial.printf("SpeakerManager: Changing I2S sample rate from %d to %d\n", s_i2s_std_cfg.clk_cfg.sample_rate_hz, info.samprate);
        if (s_i2s_enabled) i2s_channel_disable(s_tx_handle);
        s_i2s_std_cfg.clk_cfg.sample_rate_hz = info.samprate;
        i2s_channel_reconfig_std_clock(s_tx_handle, &s_i2s_std_cfg.clk_cfg);
        i2s_channel_enable(s_tx_handle);
        s_i2s_enabled = true;
    }
    // 3. Write to I2S (Blocking if buffer full)
    if (s_tx_handle) {
        size_t bytes_written;
        i2s_channel_write(s_tx_handle, pcm_buffer, len * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    }
    // 4. Feed AEC Reference
    speech.feedReference(pcm_buffer, len);
}

static MP3DecoderHelix mp3(dataCallback);

SpeakerManager::SpeakerManager() {
    _mutex = xSemaphoreCreateRecursiveMutex();
}

void SpeakerManager::begin() {
    // Initialize I2S for Speaker (TX) using ESP-IDF 5.x driver
    // We use I2S_NUM_0 to avoid conflict with Mic on I2S_NUM_1
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    s_i2s_std_cfg = { // Initialize the static config struct
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000), // Default to 24kHz (common for TTS)
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)SPEAKER_SCK,
            .ws = (gpio_num_t)SPEAKER_WS,
            .dout = (gpio_num_t)SPEAKER_SD,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    i2s_channel_init_std_mode(tx_handle, &s_i2s_std_cfg);
    // Do NOT enable here. Enable only when playing.
    
    s_tx_handle = tx_handle; // Save to static for callback
    
    mp3.begin();

    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        8192,           // Stack size
        this,
        20,             // Priority 20 (High)
        NULL,
        1               // Core 1
    );
    Serial.println("SpeakerManager: Helix Decoder Initialized (AEC Enabled)");
}

void SpeakerManager::setVolume(int volume) {
    // Map 0-21 (old range) to 0.0 - 1.0
    s_volume = (float)volume / 21.0f;
}

void SpeakerManager::playSpeechFromFile(const char* filename) {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    
    Serial.printf("Playing TTS from file: %s\n", filename);
    
    // Flush I2S buffer to remove stale data
    String fn = String(filename);
    isWav = fn.endsWith(".wav");
    s_fade_samples = 0; // Reset fade-in counter

    // Open file first to check parameters if WAV
    if (audioFile) audioFile.close();
    audioFile = LittleFS.open(filename, "r");
    
    uint32_t sampleRate = 24000; // Default

    if (isWav && audioFile) {
        // Read WAV header to get sample rate
        if (audioFile.size() >= 44) {
            uint8_t header[44];
            audioFile.read(header, 44);
            // Sample rate is at offset 24 (4 bytes)
            memcpy(&sampleRate, &header[24], 4);
            Serial.printf("WAV Sample Rate: %d\n", sampleRate);
        }
        // Seek back to data start (assuming standard 44 byte header)
        audioFile.seek(44);
    }

    if (tx_handle) {
        if (s_i2s_enabled) {
            i2s_channel_disable(tx_handle); // Always disable before re-enabling
            s_i2s_enabled = false;
        }
        
        // Update clock if needed (for WAV, or reset for MP3)
        if (s_i2s_std_cfg.clk_cfg.sample_rate_hz != sampleRate) {
             s_i2s_std_cfg.clk_cfg.sample_rate_hz = sampleRate;
             i2s_channel_reconfig_std_clock(tx_handle, &s_i2s_std_cfg.clk_cfg);
        }

        i2s_channel_enable(tx_handle);
        s_i2s_enabled = true;
        
        // Prime with 500ms of silence to wake up amp and prevent start cutoff
        // 24000Hz * 2 bytes/sample * 0.5s = 24000 bytes
        // We use a 1024 byte buffer, so ~24 chunks
        size_t bytes_written;
        const uint8_t silence_chunk[1024] = {0};
        int silence_chunks = (sampleRate * 2 * 0.5) / sizeof(silence_chunk); // 500ms
        
        for (int i = 0; i < silence_chunks; i++) {
            i2s_channel_write(tx_handle, silence_chunk, sizeof(silence_chunk), &bytes_written, 100);
        }
    }
    
    if (!isWav) {
        mp3.begin();
    }
    
    // Reset stream
    if (audioFile) audioFile.close();
    audioFile = LittleFS.open(filename, "r");
    
    if (isWav && audioFile) {
        // Skip WAV header (44 bytes)
        audioFile.seek(44);
    }
    
    _isPlaying = true;
    xSemaphoreGiveRecursive(_mutex);
}

void SpeakerManager::stop() {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (tx_handle && s_i2s_enabled) {
        i2s_channel_disable(tx_handle); // Disable I2S output
        s_i2s_enabled = false;
    }
    if (audioFile) audioFile.close();
    _isPlaying = false;
    xSemaphoreGiveRecursive(_mutex);
}

void SpeakerManager::loop() {
    if (xSemaphoreTakeRecursive(_mutex, 0) == pdTRUE) {
        if (_isPlaying) {
            if (audioFile && audioFile.available()) {
                uint8_t buff[1024];
                int bytesRead = audioFile.read(buff, sizeof(buff));
                if (bytesRead > 0) {
                    if (isWav) {
                        // Handle WAV (16-bit PCM)
                        int16_t* pcm = (int16_t*)buff;
                        size_t samples = bytesRead / 2;
                        
                        // Apply Volume & Fade-in
                        for (size_t i = 0; i < samples; i++) {
                            float fade = 1.0f;
                            if (s_fade_samples < FADE_LEN) {
                                fade = (float)s_fade_samples / FADE_LEN;
                                s_fade_samples++;
                            }
                            pcm[i] = (int16_t)(pcm[i] * s_volume * fade);
                        }
                        
                        // Write to I2S
                        if (tx_handle) {
                            size_t bytes_written;
                            i2s_channel_write(tx_handle, pcm, bytesRead, &bytes_written, portMAX_DELAY);
                        }
                        
                        // Feed AEC
                        speech.feedReference(pcm, samples);
                    } else {
                        mp3.write(buff, bytesRead);
                    }
                }
            } else {
                // End of file
                _isPlaying = false;
                
                // Flush tail with silence before disabling to prevent artifacts
                if (tx_handle && s_i2s_enabled) {
                    size_t bytes_written;
                    const uint8_t tail_silence[2048] = {0};
                    i2s_channel_write(tx_handle, tail_silence, sizeof(tail_silence), &bytes_written, 100);
                }

                if (tx_handle && s_i2s_enabled) {
                    i2s_channel_disable(tx_handle); // Disable I2S output
                    s_i2s_enabled = false;
                }
                if (audioFile) audioFile.close();
                LittleFS.remove("/speech.mp3");
            }
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

bool SpeakerManager::isRunning() {
    return _isPlaying;
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