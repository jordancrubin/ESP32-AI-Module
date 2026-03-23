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
#include "SettingsManager.h"

extern SettingsManager settings;
extern SpeechManager speech;
using namespace libhelix;

static File audioFile;
static float s_volume = 1.0f;
static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_std_config_t s_i2s_std_cfg; // Store the I2S standard config
static bool isWav = false;
static bool s_i2s_enabled = false;
static int s_fade_samples = 0;
const int FADE_LEN = 2000; // ~80ms fade-in at 24kHz (Faster attack for chimes)
static volatile bool s_is_interrupted = false;
static bool s_mp3_started = false;

// Helix Decoder Callback
void dataCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void*) {
    if (s_is_interrupted) return; // Drop audio instantly to bypass I2S blocking during interrupt

    // Handle Stereo -> Mono conversion if needed
    if (info.nChans == 2) {
        for (size_t i = 0; i < len / 2; i++) {
            int32_t l = pcm_buffer[i * 2];
            int32_t r = pcm_buffer[i * 2 + 1];
            pcm_buffer[i] = (int16_t)((l + r) / 2);
        }
        len /= 2; // Update length to reflect mono samples
    }

    // 1. Apply Volume & Fade-in
    for (size_t i = 0; i < len; i++) {
        float fade = 1.0f;
        if (s_fade_samples < FADE_LEN) {
            fade = (float)s_fade_samples / FADE_LEN;
            s_fade_samples++;
        }
        pcm_buffer[i] = (int16_t)(pcm_buffer[i] * s_volume * fade);
    }

    // 4. Feed AEC Reference (BEFORE I2S Write to prevent starvation)
    // Handle 24kHz -> 16kHz resampling for AEC
    if (info.samprate == 16000) {
        speech.feedReference(pcm_buffer, len);
    } else if (info.samprate == 24000) {
        // Simple 24kHz -> 16kHz downsampling (3 input -> 2 output)
        // We use a static buffer to avoid stack allocation issues
        static int16_t resample_buff[4096]; 
        size_t new_len = 0;
        
        for (size_t i = 0; i < len; i += 3) {
            if (new_len >= 4096 - 2) break;
            
            // Sample 1: Copy directly (0 -> 0)
            resample_buff[new_len++] = pcm_buffer[i];
            
            // Sample 2: Interpolate (1.5 -> 1)
            // We take average of index 1 and 2
            if (i + 2 < len) {
                int32_t val = ((int32_t)pcm_buffer[i+1] + (int32_t)pcm_buffer[i+2]) / 2;
                resample_buff[new_len++] = (int16_t)val;
            }
        }
        speech.feedReference(resample_buff, new_len);
    }

    // 2. Write to I2S (Blocking if buffer full)
    // Dynamically adjust I2S sample rate if needed
    if (s_tx_handle && s_i2s_std_cfg.clk_cfg.sample_rate_hz != info.samprate) {
        if (settings.debugMode) Serial.printf("SpeakerManager: Changing I2S sample rate from %d to %d\n", s_i2s_std_cfg.clk_cfg.sample_rate_hz, info.samprate);
        if (s_i2s_enabled) i2s_channel_disable(s_tx_handle);
        s_i2s_std_cfg.clk_cfg.sample_rate_hz = info.samprate;
        i2s_channel_reconfig_std_clock(s_tx_handle, &s_i2s_std_cfg.clk_cfg);
        i2s_channel_enable(s_tx_handle);
        s_i2s_enabled = true;
    }
    // 3. Write to I2S (Blocking if buffer full)
    if (s_tx_handle && s_i2s_enabled && !s_is_interrupted) {
        size_t bytes_written;
        i2s_channel_write(s_tx_handle, pcm_buffer, len * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(100));
    }
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
    s_mp3_started = true;

    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        8192,           // Stack size
        this,
        20,             // Priority 20 (High)
        NULL,
        1               // Core 1
    );
    if (settings.debugMode) Serial.println("SpeakerManager: Helix Decoder Initialized (AEC Enabled)");
}

void SpeakerManager::setVolume(int volume) {
    // Map 0-21 (old range) to 0.0 - 1.0
    s_volume = (float)volume / 21.0f;
}

void SpeakerManager::playSpeechFromFile(const char* filename) {
    s_is_interrupted = false;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    
    if (settings.debugMode) Serial.printf("Playing TTS from file: %s\n", filename);
    
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
            if (settings.debugMode) Serial.printf("WAV Sample Rate: %d\n", sampleRate);
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
        
        // Prime with a tiny burst of silence (50ms) to wake up amp and clear artifacts
        size_t bytes_written;
        const uint8_t silence_chunk[1024] = {0};
        int silence_chunks = (sampleRate * 2 * 0.05) / sizeof(silence_chunk); // 50ms
        if (silence_chunks < 1) silence_chunks = 1;
        
        for (int i = 0; i < silence_chunks; i++) {
            i2s_channel_write(tx_handle, silence_chunk, sizeof(silence_chunk), &bytes_written, 10);
        }
    }
    
    if (!isWav) {
        if (s_mp3_started) {
            mp3.end(); // Safely flush the internal decoder buffers
            s_mp3_started = false;
        }
        mp3.begin();
        s_mp3_started = true;
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
    s_is_interrupted = true; // Signal callback to dump audio instantly
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (s_mp3_started) {
        mp3.end(); // Clear internal decoder buffers (flushes remainder) immediately on stop
        s_mp3_started = false;
    }
    if (tx_handle && s_i2s_enabled) {
        i2s_channel_disable(tx_handle); // Disable I2S output
        s_i2s_enabled = false;
    }
    if (audioFile) audioFile.close();
    _isPlaying = false;
    s_is_interrupted = false; // Reset flag
    xSemaphoreGiveRecursive(_mutex);
}

void SpeakerManager::loop() {
    if (xSemaphoreTakeRecursive(_mutex, 0) == pdTRUE) {
        if (_isPlaying && !s_is_interrupted) {
            if (audioFile && audioFile.available()) {
                static uint8_t buff[4096]; // 4096 is required for fast LittleFS reads and Helix decoding
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
                        
                        // Feed AEC (BEFORE I2S Write)
                        // Handle 24kHz -> 16kHz resampling
                        if (s_i2s_std_cfg.clk_cfg.sample_rate_hz == 16000) {
                            speech.feedReference(pcm, samples);
                        } else if (s_i2s_std_cfg.clk_cfg.sample_rate_hz == 24000) {
                            static int16_t resample_buff[4096]; 
                            size_t new_len = 0;
                            
                            for (size_t i = 0; i < samples; i += 3) {
                                if (new_len >= 4096 - 2) break;
                                
                                // Sample 1: Copy directly
                                resample_buff[new_len++] = pcm[i];
                                
                                // Sample 2: Interpolate
                                if (i + 2 < samples) {
                                    int32_t val = ((int32_t)pcm[i+1] + (int32_t)pcm[i+2]) / 2;
                                    resample_buff[new_len++] = (int16_t)val;
                                }
                            }
                            speech.feedReference(resample_buff, new_len);
                        }

                        // Write to I2S
                        if (tx_handle && !s_is_interrupted) {
                            size_t bytes_written;
                            i2s_channel_write(tx_handle, pcm, bytesRead, &bytes_written, pdMS_TO_TICKS(100));
                        }
                    } else {
                        mp3.write(buff, bytesRead);
                    }
                }
            } else {
                // End of file
                _isPlaying = false;
                
                if (s_mp3_started) {
                    mp3.end(); // Clean up buffer on natural finish (flushes remaining audio)
                    s_mp3_started = false;
                }

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
        if (manager->isRunning() && !s_is_interrupted) {
            manager->loop();
            // No delay here to keep I2S buffer full (prevents stuttering)
        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); // Sleep when idle
        }
    }
}