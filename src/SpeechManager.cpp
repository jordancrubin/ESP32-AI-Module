/*
  SpeechManager.cpp - ESP32 AI Module Audio Input
  Handles I2S microphone recording (ICS-43434) and buffer management.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/

#include "SpeechManager.h"
#include <ESP-ai-wakeword_inferencing.h>
#include "SettingsManager.h"
#include <freertos/ringbuf.h>

extern SettingsManager settings;

// Speex Handles
static SpeexEchoState *st = NULL;
static SpeexPreprocessState *den = NULL;

// AEC Reference Buffer (Circular Buffer)
#define REF_BUFFER_SIZE 16384 // ~1 second at 16kHz
static int16_t s_ref_buffer[REF_BUFFER_SIZE];
static volatile size_t s_ref_write_index = 0;
static volatile size_t s_ref_read_index = 0;
static SemaphoreHandle_t s_ref_mutex = NULL;

// AEC Debug & Tuning
static volatile bool s_debug_aec = false;
static volatile int s_aec_target_delay = 640; // Default ~40ms (Aligned with user suggestion)
static volatile int s_aec_gain = 2; // Default Gain 2x to match Mic levels
static volatile bool s_aec_invert = false; // Phase inversion flag
static volatile int s_input_balance = 0; // -100 to 100
static volatile int32_t s_gain_l = 256;
static volatile int32_t s_gain_r = 256;
static volatile int32_t s_peak_l = 0;
static volatile int32_t s_peak_r = 0;
static volatile int s_aec_attenuation = 75;
static volatile int s_aec_cutoff = 600;

#define MAX_AEC_DEBUG_LOGS 250
struct AecDebugRecord {
    uint16_t refBuf;
    uint16_t micL;
    uint16_t micR;
    uint16_t refPk;
    uint16_t outPk;
};
static AecDebugRecord s_aec_debug_log[MAX_AEC_DEBUG_LOGS];
static volatile size_t s_aec_debug_log_count = 0;

// Global functions for main.cpp to call
void setAecDebug(bool enable) { 
    s_debug_aec = enable; 
    s_aec_debug_log_count = 0; // Reset log on toggle
    Serial.printf("AEC Debug: %s\n", enable ? "ON" : "OFF"); 
}
void setAecDelay(int delay) { s_aec_target_delay = delay; Serial.printf("AEC Target Delay: %d samples\n", delay); }
void setAecAttenuation(int atten) { s_aec_attenuation = (atten >= 0 && atten <= 100) ? atten : 75; }
void setAecCutoff(int cutoff) { s_aec_cutoff = cutoff >= 0 ? cutoff : 0; }
void setAecGain(int gain) { s_aec_gain = gain; Serial.printf("AEC Ref Gain: %d\n", gain); }
void setAecPhase(bool invert) { s_aec_invert = invert; Serial.printf("AEC Phase Invert: %s\n", invert ? "ON" : "OFF"); }
void setInputBalance(int balance) { 
    s_input_balance = balance; 
    if (balance < 0) {
        s_gain_l = 256;
        s_gain_r = (256 * (100 - abs(balance))) / 100;
    }
    else if (balance > 0) {
        s_gain_l = (256 * (100 - balance)) / 100;
        s_gain_r = 256;
    }
    else {
        s_gain_l = 256;
        s_gain_r = 256;
    }
}
void getAudioLevels(int* l, int* r) { *l = s_peak_l; *r = s_peak_r; }

static volatile bool s_interrupt_mode = false;
static volatile bool s_aec_bypass = false; // Flag to force raw recording

void setInterruptMode(bool mode) { s_interrupt_mode = mode; }
void setAecBypass(bool bypass) { s_aec_bypass = bypass; }

static RingbufHandle_t s_processed_ringbuf = NULL; // Buffer for clean audio
static i2s_chan_handle_t s_rx_handle = NULL;
static volatile bool s_is_recording = false; // Flag to pause AEC task

void flushAecBuffer() {
    if (s_processed_ringbuf) {
        size_t bytesFetched;
        void* data;
        // Rapidly drain the ringbuffer of any residual audio
        while ((data = xRingbufferReceive(s_processed_ringbuf, &bytesFetched, 0)) != NULL) {
            vRingbufferReturnItem(s_processed_ringbuf, data);
        }
    }
}

// Buffer Health Stats
static volatile uint32_t s_aec_overflows = 0;
static volatile uint32_t s_aec_underflows = 0;
static volatile size_t s_aec_max_usage = 0;
static volatile size_t s_aec_bytes_written = 0;

extern bool g_isSubMenuActive; // To track if the VU meter is actually visible

// Pointer to the buffer for the static callback
static float *s_inference_buffer = nullptr;

// Callback function for Edge Impulse to fetch data
static int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
    memcpy(out_ptr, s_inference_buffer + offset, length * sizeof(float));
    return 0;
}

// Background Task to feed the AFE pipeline
void feed_Task(void *arg) {
    // Speex operates on frames. 20ms at 16kHz = 320 samples.
    const int FRAME_SIZE = 256; 
    int16_t mic_frame[FRAME_SIZE];
    int16_t ref_frame[FRAME_SIZE];
    int16_t out_frame[FRAME_SIZE];
    
    int32_t i2s_raw_buff[FRAME_SIZE * 2]; // Stereo 32-bit input
    size_t bytes_read = 0;

    while (true) {
        // Pause feeding AFE if we are recording raw audio for transcription
        if (s_is_recording) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (s_rx_handle && st && den) {
            // 1. Read Microphone (Blocking, wait for full frame)
            // We need FRAME_SIZE samples. Stereo * 32-bit = 8 bytes per sample.
            if (i2s_channel_read(s_rx_handle, i2s_raw_buff, sizeof(i2s_raw_buff), &bytes_read, portMAX_DELAY) == ESP_OK) {
                
                // Apply Input Balance
                if (s_gain_l != 256 || s_gain_r != 256) {
                    for (int i = 0; i < FRAME_SIZE; i++) {
                        i2s_raw_buff[i*2] = (i2s_raw_buff[i*2] * s_gain_l) >> 8;
                        i2s_raw_buff[i*2+1] = (i2s_raw_buff[i*2+1] * s_gain_r) >> 8;
                    }
                }
                
                // Debug: Analyze Stereo Input Levels (Only calculate if VU Meter is visible or AEC debug is on!)
                if (g_isSubMenuActive || s_debug_aec) {
                    int32_t max_l = 0;
                    int32_t max_r = 0;
                    for (int i = 0; i < FRAME_SIZE; i++) {
                        int32_t l = abs(i2s_raw_buff[i*2] >> 14);
                        int32_t r = abs(i2s_raw_buff[i*2+1] >> 14);
                        if (l > max_l) max_l = l;
                        if (r > max_r) max_r = r;
                    }
                    s_peak_l = max_l;
                    s_peak_r = max_r;
                }

                // 2. Read Reference from Circular Buffer
                bool has_ref = false;
                if (s_ref_mutex) {
                    xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
                    size_t available = (s_ref_write_index >= s_ref_read_index) 
                        ? (s_ref_write_index - s_ref_read_index) 
                        : (REF_BUFFER_SIZE - (s_ref_read_index - s_ref_write_index));
                    
                    // Sync Delay: Hold reference back to align with physical echo
                    // Latency ~100ms. Delay Ref by ~1600 samples
                    // const size_t TARGET_DELAY = 1600; 
                    if (available >= (FRAME_SIZE + s_aec_target_delay)) {
                        int local_gain = s_aec_gain; // Cache volatile
                        for (int i = 0; i < FRAME_SIZE; i++) {
                            // Apply Gain to Reference
                            int32_t ref_val = s_ref_buffer[s_ref_read_index] * local_gain;
                            if (ref_val > 32767) ref_val = 32767;
                            else if (ref_val < -32768) ref_val = -32768;
                            ref_frame[i] = (int16_t)ref_val;
                            
                            s_ref_read_index = (s_ref_read_index + 1) & (REF_BUFFER_SIZE - 1); // Fast Modulo
                        }
                        has_ref = true;
                    }
                    xSemaphoreGive(s_ref_mutex);
                }

                if (!has_ref) {
                    memset(ref_frame, 0, sizeof(ref_frame)); // Silence
                }

                int local_mic_mode = settings.micMode; // Cache external
                bool local_invert = s_aec_invert;      // Cache volatile
                // 3. Prepare Mic Frame (Convert 32-bit Stereo to 16-bit Mono)
                for (int i = 0; i < FRAME_SIZE; i++) {
                    // Prepare Mono Frame for AEC based on Mic Mode
                    int32_t l = i2s_raw_buff[i*2] >> 14;
                    int32_t r = i2s_raw_buff[i*2+1] >> 14;
                    int32_t val = 0;

                    if (local_mic_mode == 2) val = r;      // Right
                    else if (local_mic_mode == 1) val = l; // Left
                    else val = (l + r) >> 1;                 // Stereo Mix (Bitshift for speed)

                    if (val > 32767) val = 32767; else if (val < -32768) val = -32768;
                    if (local_invert) val = -val;
                    mic_frame[i] = (int16_t)val;
                }

                // 4. Run Echo Cancellation
                speex_echo_cancellation(st, mic_frame, ref_frame, out_frame);

                // 5. Run Noise Suppression
                speex_preprocess_run(den, out_frame);

                // Debug Output
                if (s_debug_aec) {
                    int16_t max_ref = 0;
                    int16_t max_out = 0;
                    for (int i = 0; i < FRAME_SIZE; i++) {
                        if (abs(ref_frame[i]) > max_ref) max_ref = abs(ref_frame[i]);
                        if (abs(out_frame[i]) > max_out) max_out = abs(out_frame[i]);
                    }
                    
                    static int debug_skip = 0;
                    if (++debug_skip >= 10) { // Store every ~200ms
                        debug_skip = 0;
                        if (s_aec_debug_log_count < MAX_AEC_DEBUG_LOGS) {
                            s_aec_debug_log[s_aec_debug_log_count].refBuf = (uint16_t)((s_ref_write_index >= s_ref_read_index) ? (s_ref_write_index - s_ref_read_index) : (REF_BUFFER_SIZE - (s_ref_read_index - s_ref_write_index)));
                            s_aec_debug_log[s_aec_debug_log_count].micL = s_peak_l;
                            s_aec_debug_log[s_aec_debug_log_count].micR = s_peak_r;
                            s_aec_debug_log[s_aec_debug_log_count].refPk = max_ref;
                            s_aec_debug_log[s_aec_debug_log_count].outPk = max_out;
                            s_aec_debug_log_count++;
                        }
                    }
                }

                // 6. Output to Processed Buffer
                if (s_processed_ringbuf) {
                    if (xRingbufferSend(s_processed_ringbuf, out_frame, sizeof(out_frame), 0) != pdTRUE) {
                        s_aec_overflows = s_aec_overflows + 1;
                        }
                        else {
                        s_aec_bytes_written += sizeof(out_frame);
                        if (s_debug_aec) {
                            UBaseType_t uxFree, uxRead, uxWrite, uxAcquire, uxItemsWaiting;
                            vRingbufferGetInfo(s_processed_ringbuf, &uxFree, &uxRead, &uxWrite, &uxAcquire, &uxItemsWaiting);
                            size_t used = (16 * 1024) - uxFree; // Fixed buffer size calculation
                            if (used > s_aec_max_usage) s_aec_max_usage = used;
                        }
                    }
                }

                // Yield to prevent WDT starvation (IDLE0). 
                // Yielding every 4th frame reduces RTOS context switching 
                // overhead by 75% while keeping the watchdog safely fed.
                static uint8_t yield_cnt = 0;
                if (++yield_cnt >= 4) {
                    vTaskDelay(1);
                    yield_cnt = 0;
                }
            }
            }
            else {
             vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

SpeechManager::SpeechManager() {}

void SpeechManager::begin() {
    setupI2S();
    run_classifier_init();
    
    s_rx_handle = rx_handle;

    // Allocate inference buffer in PSRAM
    inference_buffer = (float*)ps_malloc(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));
    s_inference_buffer = inference_buffer;
    
    if (!inference_buffer) {
        Serial.println("SpeechManager: Failed to allocate inference buffer!");
    }

    if (settings.debugMode) {
        Serial.println("SpeechManager: ICS-43434 I2S Initialized");
        Serial.printf("Edge Impulse: %d classification labels loaded.\n", EI_CLASSIFIER_LABEL_COUNT);
        Serial.printf("Edge Impulse: Expected Sample Rate: %d Hz\n", EI_CLASSIFIER_FREQUENCY);
    }

    // Initialize Speex AEC (Enabled for all modes)
    s_ref_mutex = xSemaphoreCreateMutex();
    s_processed_ringbuf = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_BYTEBUF); // Increased to 16KB
    
    int sampleRate = 16000;
    int frameSize = 256;
    int filterLen = 1024; // ~64ms tail length

    st = speex_echo_state_init(frameSize, filterLen);
    den = speex_preprocess_state_init(frameSize, sampleRate);
    
    speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);
    speex_preprocess_ctl(den, SPEEX_PREPROCESS_SET_ECHO_STATE, st);

    // Optional: Enable AGC and Noise Suppression explicitly
    int i = 1;
    speex_preprocess_ctl(den, SPEEX_PREPROCESS_SET_DENOISE, &i);
    
    // Pin to Core 0 to prevent starving the UI/Main Loop on Core 1
    xTaskCreatePinnedToCore(feed_Task, "Speex_Feed", 10240, NULL, 5, NULL, 0);
    if (settings.debugMode) Serial.println("SpeechManager: Speex AEC Task Started");
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

bool SpeechManager::detectWakeWord(float threshold) {
    if (!inference_buffer) return false;

    int samplesRead = 0;
    int16_t* processed_buff = NULL;
    int32_t raw_i2s_buffer[256]; // Buffer for raw fallback
    size_t bytes_fetched = 0;

    // 1. Fetch Audio
    if (s_processed_ringbuf) {
        // AEC Mode: Fetch clean audio from Speex pipeline
        processed_buff = (int16_t*)xRingbufferReceive(s_processed_ringbuf, &bytes_fetched, 0);
        if (processed_buff) {
            samplesRead = bytes_fetched / sizeof(int16_t);
            }
            else {
            return false; // No data ready
        }
        }
        else {
        // Fallback: Raw I2S Mode
        size_t bytes_read = 0;
        if (i2s_channel_read(rx_handle, raw_i2s_buffer, sizeof(raw_i2s_buffer), &bytes_read, 20) == ESP_OK) {
            if (bytes_read > 0) {
                samplesRead = bytes_read / 8; // 8 bytes per stereo frame
            }
        }
        if (samplesRead == 0) return false;
    }
    
    // Use static to persist peak level across multiple calls until inference runs
    static int32_t max_audio_level = 0;
    static int32_t max_l_level = 0;
    static int32_t max_r_level = 0;
    static int32_t dc_offset_int = 0;
    static int32_t debug_min_val = 32767;
    static int32_t debug_max_val = -32768;
    
    // Capture AEC peaks if available (snapshot from feed_Task) just once per batch
    if (processed_buff) {
        if (s_peak_l > max_l_level) max_l_level = s_peak_l;
        if (s_peak_r > max_r_level) max_r_level = s_peak_r;
    }

    int local_mic_mode = settings.micMode;
    bool local_interrupt = s_interrupt_mode;
    int local_attenuation_mult = 100 - s_aec_attenuation; // Compute once outside the loop

    for (int i = 0; i < samplesRead; i++) { 
        int32_t raw;
        
        if (processed_buff) {
            // Speex returns 16-bit clean audio
            raw = processed_buff[i];
        }
        else {
            // Fallback processing: Convert 32-bit Stereo to 16-bit Mono
            int32_t l = raw_i2s_buffer[i*2] >> 14;
            int32_t r = raw_i2s_buffer[i*2+1] >> 14;
            
            if (abs(l) > max_l_level) max_l_level = abs(l);
            if (abs(r) > max_r_level) max_r_level = abs(r);
            
            // Respect Microphone Mode setting
            if (local_mic_mode == 2) raw = r;      // Right
            else if (local_mic_mode == 1) raw = l; // Left
            else raw = (l + r) >> 1;                 // Stereo Mix (Bitshift for speed)

            // DC Offset removal (High-pass filter) using fast fixed-point math
            // alpha = ~0.0039 (1/256), functionally identical to 0.005 but uses 0 hardware multiply cycles
            dc_offset_int += raw - (dc_offset_int >> 8);
            raw -= (dc_offset_int >> 8);
            
            if (raw > 32767) raw = 32767; else if (raw < -32768) raw = -32768;
        }

        // Lower microphone sensitivity specifically during Voice Interrupt (Barge-in)
        // to help prevent the AI's own early echo from triggering the wake word
        if (local_interrupt) {
            raw = (raw * local_attenuation_mult) / 100; 
        }

        if (abs(raw) > max_audio_level) max_audio_level = abs(raw);
        if (raw < debug_min_val) debug_min_val = raw;
        if (raw > debug_max_val) debug_max_val = raw;

        if (inference_buf_ptr < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
            inference_buffer[inference_buf_ptr++] = (float)raw;
        }

        // ... (Rest of inference logic remains the same)
    }

    // Return item to ringbuffer if we used it
    if (processed_buff) {
        vRingbufferReturnItem(s_processed_ringbuf, processed_buff);
    }

    // ... (Rest of function: inference execution)
    // 3. Run Inference if buffer is full
    if (inference_buf_ptr >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
        // ... (Existing inference code)
        // (Keep the existing code block here, just ensure the closing braces match)
        
        signal_t signal;
        signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
        signal.get_data = &raw_feature_get_data;

        ei_impulse_result_t result = { 0 };
        bool debug_mode = false;

        EI_IMPULSE_ERROR res = run_classifier(&signal, &result, debug_mode);
        if (res != EI_IMPULSE_OK) {
            Serial.printf("ERR: Failed to run classifier (%d)\n", res);
            return false;
        }

        // This is the most performance-critical debug output. To prevent audio artifacts,
        // only print if the Serial monitor is connected and has enough buffer space.
        if (settings.debugMode && Serial && Serial.availableForWrite() > 64) {
            char buf[256];
            int len = snprintf(buf, sizeof(buf), "Raw: %d | L: %d R: %d | Time: %dms | ", max_audio_level, max_l_level, max_r_level, result.timing.dsp + result.timing.classification);
            for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                // Safely append to the buffer, ensuring we never write out of bounds
                if (len >= 0 && len < sizeof(buf)) {
                    int written = snprintf(buf + len, sizeof(buf) - len, "%s: %.2f ", result.classification[ix].label, result.classification[ix].value);
                    if (written > 0) len += written;
                }
            }
            Serial.println(buf);
        }
        
        int32_t current_max_level = max_audio_level; // Capture before reset

        max_audio_level = 0;
        max_l_level = 0;
        max_r_level = 0;
        debug_min_val = 32767;
        debug_max_val = -32768;

        bool wake_word_detected = false;
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            // Standard Wake Word Detection (Only when NOT in interrupt mode)
            if (!s_interrupt_mode) {
                if (result.classification[ix].value > threshold) {
                    const char* label = result.classification[ix].label;
                    if (strcmp(label, "noise") != 0 && strcmp(label, "unknown") != 0) {
                        if (settings.debugMode) Serial.printf(">>> WAKE WORD DETECTED: %s (%.2f) <<<\n", label, result.classification[ix].value);
                        wake_word_detected = true;
                    }
                }
            }
            
            // Voice Barge-in Detection (Trigger on ANY loud speech "unknown" if confidence is 0.95+)
            if (s_interrupt_mode && strcmp(result.classification[ix].label, "unknown") == 0 && result.classification[ix].value >= 0.95f) {
                if (current_max_level > s_aec_cutoff) { // Amplitude gate to block AEC speaker bleed
                    if (settings.debugMode) Serial.printf(">>> INTERRUPT (UNKNOWN) DETECTED: %s (%.2f) <<<\n", result.classification[ix].label, result.classification[ix].value);
                    wake_word_detected = true;
                }
            }
        }
        
        if (wake_word_detected) {
            inference_buf_ptr = 0;
            return true;
        }
        
        int slide = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE / 2;
        memmove(inference_buffer, inference_buffer + slide, (EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - slide) * sizeof(float));
        inference_buf_ptr -= slide;
    }

    return false;
}

void SpeechManager::feedReference(const int16_t *data, size_t samples) {
    // Feed reference data to the Ring Buffer
    if (s_ref_mutex) {
        xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
        for (size_t i = 0; i < samples; i++) {
            s_ref_buffer[s_ref_write_index] = data[i];
            s_ref_write_index = (s_ref_write_index + 1) & (REF_BUFFER_SIZE - 1); // Fast Modulo
            // If write catches read, bump read (overwrite oldest)
            if (s_ref_write_index == s_ref_read_index) {
                s_ref_read_index = (s_ref_read_index + 1) & (REF_BUFFER_SIZE - 1);
            }
        }
        xSemaphoreGive(s_ref_mutex);
    }
}

uint8_t* SpeechManager::record(int durationMs, size_t* outSize, int silenceThreshold) {
    bool useAec = (s_processed_ringbuf != NULL) && !s_aec_bypass;

    if (!useAec) {
        s_is_recording = true; // Pause the AEC feed task only if NOT using AEC
        delay(50); // Give the task time to yield
    }

    size_t sampleRate = 16000;
    size_t numSamples = (sampleRate * durationMs) / 1000;
    size_t dataSize = numSamples * 2; // 16-bit = 2 bytes per sample
    size_t fileSize = sizeof(WavHeader) + dataSize;

    // Allocate memory in PSRAM
    uint8_t* wavBuffer = (uint8_t*)ps_malloc(fileSize);
    if (!wavBuffer) {
        Serial.println("SpeechManager: Failed to allocate PSRAM for recording");
        s_is_recording = false;
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

    // Flush Input Buffer (Ringbuffer or I2S)
    if (useAec) {
        size_t bytesFetched;
        void* data;
        while ((data = xRingbufferReceive(s_processed_ringbuf, &bytesFetched, 0)) != NULL) {
            vRingbufferReturnItem(s_processed_ringbuf, data);
        }
        // Reset Stats
        s_aec_overflows = 0;
        s_aec_underflows = 0;
        s_aec_max_usage = 0;
        s_aec_bytes_written = 0;
    } else {
        size_t bytesRead;
        int32_t flushBuffer[128];
        while (i2s_channel_read(rx_handle, flushBuffer, sizeof(flushBuffer), &bytesRead, 0) == ESP_OK && bytesRead > 0);
    }

    if (settings.debugMode) Serial.println("Recording...");
    int16_t* pcmBuffer = (int16_t*)(wavBuffer + sizeof(WavHeader));
    size_t samplesRead = 0;
    int32_t sampleBuffer[128 * 2]; // Buffer for 32-bit Stereo
    
    unsigned long silenceStart = millis();
    const unsigned long SILENCE_DURATION = 2000; // Stop after 2 seconds of silence
    const unsigned long MAX_INITIAL_SILENCE = 5000; // Stop after 5 seconds if no speech detected
    bool voiceDetectedTotal = false;
    int local_mic_mode = settings.micMode; // Cache external variable

    while (samplesRead < numSamples) {
        bool voiceDetectedInBatch = false;
        size_t bytesRead = 0;

        if (useAec) {
            // Read from AEC Ringbuffer
            size_t bytesFetched = 0;
            int16_t* processed_data = (int16_t*)xRingbufferReceive(s_processed_ringbuf, &bytesFetched, pdMS_TO_TICKS(100));
            
            if (processed_data && bytesFetched > 0) {
                int samplesInBatch = bytesFetched / sizeof(int16_t);
                for (int i = 0; i < samplesInBatch && samplesRead < numSamples; i++) {
                    int16_t raw = processed_data[i];
                    if (!voiceDetectedInBatch && abs(raw) > silenceThreshold) {
                        voiceDetectedInBatch = true;
                        voiceDetectedTotal = true;
                    }
                    pcmBuffer[samplesRead++] = raw;
                }
                vRingbufferReturnItem(s_processed_ringbuf, processed_data);
            }
            else {
                s_aec_underflows = s_aec_underflows + 1;
            }
        }
        else {
            // Read from Raw I2S
            if (i2s_channel_read(rx_handle, sampleBuffer, sizeof(sampleBuffer), &bytesRead, portMAX_DELAY) == ESP_OK) {
                int samplesInBatch = bytesRead / 8; // 8 bytes per stereo frame
                for (int i = 0; i < samplesInBatch && samplesRead < numSamples; i++) {
                    int32_t l = sampleBuffer[i*2] >> 14;
                    int32_t r = sampleBuffer[i*2+1] >> 14;
                    int32_t val = 0;
                    
                    if (local_mic_mode == 2) val = r;
                    else if (local_mic_mode == 1) val = l;
                    else val = (l + r) >> 1;

                    if (val > 32767) val = 32767; else if (val < -32768) val = -32768;
                    
                    if (!voiceDetectedInBatch && abs(val) > silenceThreshold) {
                        voiceDetectedInBatch = true;
                        voiceDetectedTotal = true;
                    }
                    pcmBuffer[samplesRead++] = (int16_t)val;
                }
            }
        }

        // Common Silence Logic
        if (voiceDetectedInBatch) {
            silenceStart = millis();
        }
        else {
            unsigned long silenceTime = millis() - silenceStart;
            if (voiceDetectedTotal && (silenceTime > SILENCE_DURATION)) {
                if (settings.debugMode) Serial.println("Silence detected. Stopping recording.");
                break;
            }
            if (!voiceDetectedTotal && (silenceTime > MAX_INITIAL_SILENCE)) {
                if (settings.debugMode) Serial.println("No speech detected (Timeout). Aborting.");
                free(wavBuffer);
                if (!useAec) s_is_recording = false;
                return nullptr;
            }
        }
    }

    if (!voiceDetectedTotal) {
        if (settings.debugMode) Serial.println("No speech detected (Max Duration). Aborting.");
        free(wavBuffer);
        if (!useAec) s_is_recording = false;
        return nullptr;
    }

    // Update WAV Header with actual size
    size_t actualDataSize = samplesRead * 2;
    size_t actualFileSize = sizeof(WavHeader) + actualDataSize;
    WavHeader* headerPtr = (WavHeader*)wavBuffer;
    headerPtr->dataSize = actualDataSize;
    headerPtr->overallSize = actualFileSize - 8;
    
    *outSize = actualFileSize;
    if (useAec && settings.debugMode) {
        Serial.println("\n--- AEC Buffer Health Report ---");
        Serial.printf("Total Bytes Written: %u\n", s_aec_bytes_written);
        Serial.printf("Overflows (Write Fails): %u\n", s_aec_overflows);
        Serial.printf("Underflows (Read Fails): %u\n", s_aec_underflows);
        Serial.printf("Max Buffer Usage: %u / %u bytes\n", s_aec_max_usage, 16 * 1024);
        
        if (s_debug_aec && s_aec_debug_log_count > 0) {
            Serial.println("\n--- AEC Performance Log ---");
            for (size_t i = 0; i < s_aec_debug_log_count; i++) {
                Serial.printf("AEC: RefBuf=%5d | MicL=%5d MicR=%5d | RefPk=%5d | OutPk=%5d\n", 
                    s_aec_debug_log[i].refBuf, 
                    s_aec_debug_log[i].micL, 
                    s_aec_debug_log[i].micR, 
                    s_aec_debug_log[i].refPk, 
                    s_aec_debug_log[i].outPk);
            }
            s_aec_debug_log_count = 0; // Clear after printing
        }
        Serial.println("--------------------------------");
    }
    else {
        s_is_recording = false; // Resume AEC task if it was paused
    }
    return wavBuffer;
}

void runEdgeImpulseForwarder() {
    s_is_recording = true; // Pause the background AEC task
    
    int32_t sampleBuffer[128 * 2]; // 128 stereo frames
    size_t bytesRead;
    int local_mic_mode = settings.micMode;
    
    // Flush any initial stale data from hardware buffers
    while (i2s_channel_read(s_rx_handle, sampleBuffer, sizeof(sampleBuffer), &bytesRead, 0) == ESP_OK && bytesRead > 0);

    while (true) {
        if (i2s_channel_read(s_rx_handle, sampleBuffer, sizeof(sampleBuffer), &bytesRead, portMAX_DELAY) == ESP_OK) {
            int samples = bytesRead / 8; // 8 bytes per 32-bit stereo frame
            for (int i = 0; i < samples; i++) {
                int32_t l = sampleBuffer[i*2] >> 14;
                int32_t r = sampleBuffer[i*2+1] >> 14;
                int32_t val = (local_mic_mode == 2) ? r : (local_mic_mode == 1) ? l : ((l + r) >> 1);
                Serial.println(val); // Data Forwarder requires one sample per line
            }
        }
    }
}