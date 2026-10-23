/*
  SpeechManager.cpp - ESP32 AI Module Audio Input
  Handles I2S microphone recording (ICS-43434) and buffer management.
  
  https://www.youtube.com/@rubin-tech
  2026 Jordan Rubin.
*/

#include "SpeechManager.h"
#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>
#include <frontend.h>
#include <frontend_util.h>
#include "excuse_me_hal.h"
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

// microWakeWord Settings from excuse_me_hal.json
static const char *const MWW_NAME = "Excuse me Hal";
static const size_t MWW_SLIDING_WINDOW_SIZE = 5;
static const size_t MWW_TENSOR_ARENA_SIZE = 40 * 1024; // 40KB (model requires 32000)
static const size_t MWW_VAR_ARENA_SIZE = 2048;
static const uint8_t MWW_FEATURE_SIZE = 40;
static const uint16_t MWW_STEP_SAMPLES = 160; // 10ms @ 16kHz
static const int16_t MWW_MIN_IGNORE_WINDOWS = 74; // Warmup windows before triggering

// microWakeWord State
static uint8_t *s_mww_tensor_arena = nullptr;
static uint8_t *s_mww_var_arena = nullptr;
static tflite::MicroAllocator *s_mww_ma = nullptr;
static tflite::MicroResourceVariables *s_mww_mrv = nullptr;
static tflite::MicroMutableOpResolver<20> s_mww_op_resolver;
static tflite::MicroInterpreter *s_mww_interpreter = nullptr;
static struct FrontendConfig s_mww_frontend_config;
static struct FrontendState s_mww_frontend_state;
static bool s_mww_initialized = false;

static uint8_t s_mww_sliding_window[MWW_SLIDING_WINDOW_SIZE] = {0};
static size_t s_mww_sliding_window_idx = 0;
static uint8_t s_mww_current_stride_step = 0;
static int16_t s_mww_ignore_windows = -MWW_MIN_IGNORE_WINDOWS;

// Audio accumulator for microWakeWord
static const size_t MWW_AUDIO_ACCUM_MAX = 2048;
static int16_t s_mww_audio_accum[MWW_AUDIO_ACCUM_MAX];
static size_t s_mww_audio_accum_len = 0;

static bool register_mww_ops(tflite::MicroMutableOpResolver<20> &resolver) {
    if (resolver.AddCallOnce() != kTfLiteOk) return false;
    if (resolver.AddVarHandle() != kTfLiteOk) return false;
    if (resolver.AddReshape() != kTfLiteOk) return false;
    if (resolver.AddReadVariable() != kTfLiteOk) return false;
    if (resolver.AddStridedSlice() != kTfLiteOk) return false;
    if (resolver.AddConcatenation() != kTfLiteOk) return false;
    if (resolver.AddAssignVariable() != kTfLiteOk) return false;
    if (resolver.AddConv2D() != kTfLiteOk) return false;
    if (resolver.AddMul() != kTfLiteOk) return false;
    if (resolver.AddAdd() != kTfLiteOk) return false;
    if (resolver.AddMean() != kTfLiteOk) return false;
    if (resolver.AddFullyConnected() != kTfLiteOk) return false;
    if (resolver.AddLogistic() != kTfLiteOk) return false;
    if (resolver.AddQuantize() != kTfLiteOk) return false;
    if (resolver.AddDepthwiseConv2D() != kTfLiteOk) return false;
    if (resolver.AddAveragePool2D() != kTfLiteOk) return false;
    if (resolver.AddMaxPool2D() != kTfLiteOk) return false;
    if (resolver.AddPad() != kTfLiteOk) return false;
    if (resolver.AddPack() != kTfLiteOk) return false;
    if (resolver.AddSplitV() != kTfLiteOk) return false;
    return true;
}

static void resetMicroWakeWord() {
    memset(s_mww_sliding_window, 0, sizeof(s_mww_sliding_window));
    s_mww_sliding_window_idx = 0;
    s_mww_current_stride_step = 0;
    s_mww_ignore_windows = -MWW_MIN_IGNORE_WINDOWS;
    s_mww_audio_accum_len = 0;
    if (s_mww_initialized) {
        FrontendReset(&s_mww_frontend_state);
    }
}

static bool initMicroWakeWord() {
    if (s_mww_initialized) return true;

    // 1. Setup Audio Frontend
    FrontendFillConfigWithDefaults(&s_mww_frontend_config);
    s_mww_frontend_config.window.size_ms = 30;
    s_mww_frontend_config.window.step_size_ms = 10;
    s_mww_frontend_config.filterbank.num_channels = MWW_FEATURE_SIZE;
    s_mww_frontend_config.filterbank.lower_band_limit = 125.0;
    s_mww_frontend_config.filterbank.upper_band_limit = 7500.0;
    s_mww_frontend_config.noise_reduction.smoothing_bits = 10;
    s_mww_frontend_config.noise_reduction.even_smoothing = 0.025;
    s_mww_frontend_config.noise_reduction.odd_smoothing = 0.06;
    s_mww_frontend_config.noise_reduction.min_signal_remaining = 0.05;
    s_mww_frontend_config.pcan_gain_control.enable_pcan = 1;
    s_mww_frontend_config.pcan_gain_control.strength = 0.95;
    s_mww_frontend_config.pcan_gain_control.offset = 80.0;
    s_mww_frontend_config.pcan_gain_control.gain_bits = 21;
    s_mww_frontend_config.log_scale.enable_log = 1;
    s_mww_frontend_config.log_scale.scale_shift = 6;

    if (!FrontendPopulateState(&s_mww_frontend_config, &s_mww_frontend_state, 16000)) {
        Serial.println("SpeechManager [mWW]: Failed to populate frontend state");
        FrontendFreeStateContents(&s_mww_frontend_state);
        return false;
    }

    // 2. Register TFLM Ops
    if (!register_mww_ops(s_mww_op_resolver)) {
        Serial.println("SpeechManager [mWW]: Failed to register streaming ops");
        return false;
    }

    // 3. Allocate Arenas in PSRAM (fallback to internal RAM)
    if (!s_mww_tensor_arena) {
        s_mww_tensor_arena = (uint8_t*)ps_malloc(MWW_TENSOR_ARENA_SIZE);
        if (!s_mww_tensor_arena) s_mww_tensor_arena = (uint8_t*)malloc(MWW_TENSOR_ARENA_SIZE);
    }
    if (!s_mww_var_arena) {
        s_mww_var_arena = (uint8_t*)ps_malloc(MWW_VAR_ARENA_SIZE);
        if (!s_mww_var_arena) s_mww_var_arena = (uint8_t*)malloc(MWW_VAR_ARENA_SIZE);
    }

    if (!s_mww_tensor_arena || !s_mww_var_arena) {
        Serial.println("SpeechManager [mWW]: Failed to allocate arena memory");
        return false;
    }

    // 4. Create Resource Variables
    s_mww_ma = tflite::MicroAllocator::Create(s_mww_var_arena, MWW_VAR_ARENA_SIZE);
    s_mww_mrv = tflite::MicroResourceVariables::Create(s_mww_ma, 20);

    // 5. Load Model & Create Interpreter
    const tflite::Model *model = tflite::GetModel(excuse_me_hal_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        Serial.println("SpeechManager [mWW]: Model schema version mismatch");
        return false;
    }

    s_mww_interpreter = new tflite::MicroInterpreter(
        model, s_mww_op_resolver, s_mww_tensor_arena, MWW_TENSOR_ARENA_SIZE, s_mww_mrv);

    if (s_mww_interpreter->AllocateTensors() != kTfLiteOk) {
        Serial.println("SpeechManager [mWW]: Failed to allocate model tensors");
        return false;
    }

    Serial.printf("SpeechManager [mWW]: Initialized model '%s' (Arena used: %d bytes)\n",
                  MWW_NAME, s_mww_interpreter->arena_used_bytes());

    resetMicroWakeWord();
    s_mww_initialized = true;
    return true;
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
                            s_aec_debug_log_count = s_aec_debug_log_count + 1;
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
    s_rx_handle = rx_handle;

    if (!initMicroWakeWord()) {
        Serial.println("SpeechManager: Failed to initialize microWakeWord!");
    }

    if (settings.debugMode) {
        Serial.println("SpeechManager: ICS-43434 I2S Initialized");
        Serial.printf("microWakeWord: Loaded model '%s'\n", MWW_NAME);
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
    if (!s_mww_initialized) return false;

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
        } else {
            return false; // No data ready
        }
    } else {
        // Fallback: Raw I2S Mode
        size_t bytes_read = 0;
        if (i2s_channel_read(rx_handle, raw_i2s_buffer, sizeof(raw_i2s_buffer), &bytes_read, 20) == ESP_OK) {
            if (bytes_read > 0) {
                samplesRead = bytes_read / 8; // 8 bytes per stereo frame
            }
        }
        if (samplesRead == 0) return false;
    }

    static int32_t max_audio_level = 0;
    static int32_t max_l_level = 0;
    static int32_t max_r_level = 0;
    static int32_t dc_offset_int = 0;

    if (processed_buff) {
        if (s_peak_l > max_l_level) max_l_level = s_peak_l;
        if (s_peak_r > max_r_level) max_r_level = s_peak_r;
    }

    int local_mic_mode = settings.micMode;
    bool local_interrupt = s_interrupt_mode;
    int local_attenuation_mult = 100 - s_aec_attenuation;

    for (int i = 0; i < samplesRead; i++) {
        int32_t raw;
        if (processed_buff) {
            raw = processed_buff[i];
        } else {
            int32_t l = raw_i2s_buffer[i*2] >> 14;
            int32_t r = raw_i2s_buffer[i*2+1] >> 14;

            if (abs(l) > max_l_level) max_l_level = abs(l);
            if (abs(r) > max_r_level) max_r_level = abs(r);

            if (local_mic_mode == 2) raw = r;
            else if (local_mic_mode == 1) raw = l;
            else raw = (l + r) >> 1;

            dc_offset_int += raw - (dc_offset_int >> 8);
            raw -= (dc_offset_int >> 8);

            if (raw > 32767) raw = 32767; else if (raw < -32768) raw = -32768;
        }

        if (local_interrupt) {
            raw = (raw * local_attenuation_mult) / 100;
        }

        if (abs(raw) > max_audio_level) max_audio_level = abs(raw);

        if (s_mww_audio_accum_len < MWW_AUDIO_ACCUM_MAX) {
            s_mww_audio_accum[s_mww_audio_accum_len++] = (int16_t)raw;
        }
    }

    if (processed_buff) {
        vRingbufferReturnItem(s_processed_ringbuf, processed_buff);
    }

    bool wake_word_detected = false;

    // Process all full 160-sample (10ms) slices in accumulator
    while (s_mww_audio_accum_len >= MWW_STEP_SAMPLES) {
        size_t samples_read = 0;
        struct FrontendOutput frontend_output = FrontendProcessSamples(
            &s_mww_frontend_state, s_mww_audio_accum, MWW_STEP_SAMPLES, &samples_read);

        s_mww_audio_accum_len -= MWW_STEP_SAMPLES;
        if (s_mww_audio_accum_len > 0) {
            memmove(s_mww_audio_accum, s_mww_audio_accum + MWW_STEP_SAMPLES, s_mww_audio_accum_len * sizeof(int16_t));
        }

        if (frontend_output.size != MWW_FEATURE_SIZE || frontend_output.values == NULL) {
            continue;
        }

        // Quantize features to int8 matching microWakeWord training pipeline
        int8_t audio_features[MWW_FEATURE_SIZE];
        for (size_t i = 0; i < MWW_FEATURE_SIZE; ++i) {
            constexpr int32_t value_scale = 256;
            constexpr int32_t value_div = 666;
            int32_t value = ((frontend_output.values[i] * value_scale) + (value_div / 2)) / value_div;
            value -= 128;
            if (value < -128) value = -128;
            if (value > 127) value = 127;
            audio_features[i] = (int8_t)value;
        }

        // Feed features into model input
        TfLiteTensor *input = s_mww_interpreter->input(0);
        uint8_t stride = input->dims->data[1]; // stride (typically 1 or 2)
        int8_t *input_data = tflite::GetTensorData<int8_t>(input);
        memcpy(input_data + (MWW_FEATURE_SIZE * s_mww_current_stride_step), audio_features, MWW_FEATURE_SIZE);
        s_mww_current_stride_step++;

        if (s_mww_current_stride_step >= stride) {
            s_mww_current_stride_step = 0;

            TfLiteStatus invoke_status = s_mww_interpreter->Invoke();
            if (invoke_status != kTfLiteOk) {
                Serial.println("SpeechManager [mWW]: Interpreter invoke failed");
                continue;
            }

            TfLiteTensor *output = s_mww_interpreter->output(0);
            uint8_t raw_prob = output->data.uint8[0];

            // Sliding window rolling buffer
            s_mww_sliding_window[s_mww_sliding_window_idx] = raw_prob;
            s_mww_sliding_window_idx = (s_mww_sliding_window_idx + 1) % MWW_SLIDING_WINDOW_SIZE;

            // Startup / post-detection warmup
            if (s_mww_ignore_windows < 0) {
                s_mww_ignore_windows++;
                continue;
            }

            // Calculate rolling window average
            uint32_t sum = 0;
            for (size_t k = 0; k < MWW_SLIDING_WINDOW_SIZE; k++) {
                sum += s_mww_sliding_window[k];
            }
            float avg_prob = (float)sum / (float)(255 * MWW_SLIDING_WINDOW_SIZE);
            float current_prob = (float)raw_prob / 255.0f;

            // Debug logging
            if (settings.debugMode && Serial && Serial.availableForWrite() > 64) {
                static uint32_t s_last_log_ms = 0;
                if (millis() - s_last_log_ms > 250 || avg_prob > 0.35f) {
                    Serial.printf("mWW ['%s']: Avg: %.2f | Instant: %.2f | Thresh: %.2f | Level: %d\n",
                                  MWW_NAME, avg_prob, current_prob, threshold, max_audio_level);
                    s_last_log_ms = millis();
                }
            }

            // Detection check
            if (!s_interrupt_mode) {
                if (avg_prob >= threshold) {
                    if (settings.debugMode) {
                        Serial.printf(">>> WAKE WORD DETECTED: '%s' (avg: %.2f, instant: %.2f) <<<\n",
                                      MWW_NAME, avg_prob, current_prob);
                    }
                    wake_word_detected = true;
                }
            } else {
                // Voice Interrupt (Barge-in) during audio playback
                if (avg_prob >= threshold && max_audio_level > s_aec_cutoff) {
                    if (settings.debugMode) {
                        Serial.printf(">>> INTERRUPT WAKE WORD DETECTED: '%s' (avg: %.2f) <<<\n",
                                      MWW_NAME, avg_prob);
                    }
                    wake_word_detected = true;
                }
            }

            if (wake_word_detected) {
                resetMicroWakeWord();
                break;
            }
        }
    }

    max_audio_level = 0;
    max_l_level = 0;
    max_r_level = 0;

    return wake_word_detected;
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