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

static RingbufHandle_t s_ref_ringbuf = NULL;
static RingbufHandle_t s_processed_ringbuf = NULL; // Buffer for clean audio
static i2s_chan_handle_t s_rx_handle = NULL;
static volatile bool s_is_recording = false; // Flag to pause AEC task

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
    const int FRAME_SIZE = 320; 
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
                
                // 2. Read Reference (Non-blocking attempt)
                size_t ref_bytes_received = 0;
                void* ref_data = NULL;
                if (s_ref_ringbuf) {
                    // Try to get exactly one frame of reference audio
                    ref_data = xRingbufferReceive(s_ref_ringbuf, &ref_bytes_received, 0);
                }
                
                // Prepare Reference Frame
                if (ref_data && ref_bytes_received >= sizeof(ref_frame)) {
                    memcpy(ref_frame, ref_data, sizeof(ref_frame));
                    vRingbufferReturnItem(s_ref_ringbuf, ref_data);
                } else {
                    if (ref_data) vRingbufferReturnItem(s_ref_ringbuf, ref_data); // Return partial/wrong size
                    memset(ref_frame, 0, sizeof(ref_frame)); // Silence
                }

                // 3. Prepare Mic Frame (Convert 32-bit Stereo to 16-bit Mono)
                for (int i = 0; i < FRAME_SIZE; i++) {
                    // Use Left Channel (or mix)
                    int32_t raw = i2s_raw_buff[i*2] >> 13; // Scale 24-bit to 16-bit
                    if (raw > 32767) raw = 32767; else if (raw < -32768) raw = -32768;
                    mic_frame[i] = (int16_t)raw;
                }

                // 4. Run Echo Cancellation
                speex_echo_cancellation(st, mic_frame, ref_frame, out_frame);

                // 5. Run Noise Suppression
                speex_preprocess_run(den, out_frame);

                // 6. Output to Processed Buffer
                if (s_processed_ringbuf) {
                    xRingbufferSend(s_processed_ringbuf, out_frame, sizeof(out_frame), 0);
                }

                // Yield to prevent WDT starvation (IDLE0) if processing takes >20ms
                vTaskDelay(1); 
            }
        } else {
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

    Serial.println("SpeechManager: ICS-43434 I2S Initialized");
    Serial.printf("Edge Impulse: %d classification labels loaded.\n", EI_CLASSIFIER_LABEL_COUNT);
    Serial.printf("Edge Impulse: Expected Sample Rate: %d Hz\n", EI_CLASSIFIER_FREQUENCY);

    // Initialize Speex AEC if in Stereo Mode (Mode 0)
    if (settings.micMode == 0) {
        s_ref_ringbuf = xRingbufferCreate(8 * 1024, RINGBUF_TYPE_BYTEBUF);
        s_processed_ringbuf = xRingbufferCreate(8 * 1024, RINGBUF_TYPE_BYTEBUF);
        
        int sampleRate = 16000;
        int frameSize = 320; // 20ms
        int filterLen = 3200; // 200ms tail length

        st = speex_echo_state_init(frameSize, filterLen);
        den = speex_preprocess_state_init(frameSize, sampleRate);
        
        speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);
        speex_preprocess_ctl(den, SPEEX_PREPROCESS_SET_ECHO_STATE, st);

        // Optional: Enable AGC and Noise Suppression explicitly
        int i = 1;
        speex_preprocess_ctl(den, SPEEX_PREPROCESS_SET_DENOISE, &i);
        // speex_preprocess_ctl(den, SPEEX_PREPROCESS_SET_AGC, &i); // Uncomment if volume is too low
        
        // Pin to Core 0 to prevent starving the UI/Main Loop on Core 1
        xTaskCreatePinnedToCore(feed_Task, "Speex_Feed", 10240, NULL, 5, NULL, 0);
        Serial.println("SpeechManager: Speex AEC Task Started");
    } else {
        Serial.println("SpeechManager: AEC disabled (Not in Stereo Mode).");
    }
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
    
    // Use static to persist peak level across multiple calls until inference runs
    static int32_t max_audio_level = 0;
    static float dc_offset = 0.0f;
    static int32_t debug_min_val = 32767;
    static int32_t debug_max_val = -32768;

    for (int i = 0; i < samplesRead; i++) { 
        int32_t raw;
        
        if (processed_buff) {
            // Speex returns 16-bit clean audio
            raw = processed_buff[i];
        } else {
            // Fallback processing: Convert 32-bit Stereo to 16-bit Mono
            int32_t l = raw_i2s_buffer[i*2] >> 13;
            int32_t r = raw_i2s_buffer[i*2+1] >> 13;
            raw = (l + r) / 2; 

            // DC Offset removal (High-pass filter)
            dc_offset = (dc_offset * 0.995f) + ((float)raw * 0.005f);
            raw -= (int32_t)dc_offset;
            if (raw > 32767) raw = 32767; else if (raw < -32768) raw = -32768;
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

        // Print Debug Info
        Serial.printf("Raw: %d | Time: %dms | ", max_audio_level, result.timing.dsp + result.timing.classification);
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            Serial.printf("%s: %.2f ", result.classification[ix].label, result.classification[ix].value);
        }
        Serial.println();
        
        max_audio_level = 0;
        debug_min_val = 32767;
        debug_max_val = -32768;

        bool wake_word_detected = false;
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            if (result.classification[ix].value > threshold) {
                const char* label = result.classification[ix].label;
                if (strcmp(label, "noise") != 0 && strcmp(label, "unknown") != 0) {
                    Serial.printf(">>> WAKE WORD DETECTED: %s (%.2f) <<<\n", label, result.classification[ix].value);
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
    if (s_ref_ringbuf && settings.micMode == 0) {
        // Send data. If buffer full, we drop (better to drop ref than block playback)
        xRingbufferSend(s_ref_ringbuf, (void*)data, samples * sizeof(int16_t), 0);
    }
}

uint8_t* SpeechManager::record(int durationMs, size_t* outSize, int silenceThreshold) {
    s_is_recording = true; // Pause the AEC feed task
    delay(50); // Give the task time to yield

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

    // Flush I2S buffer
    size_t bytesRead;
    int32_t flushBuffer[128];
    while (i2s_channel_read(rx_handle, flushBuffer, sizeof(flushBuffer), &bytesRead, 0) == ESP_OK && bytesRead > 0);

    Serial.println("Recording...");
    int16_t* pcmBuffer = (int16_t*)(wavBuffer + sizeof(WavHeader));
    size_t samplesRead = 0;
    int32_t sampleBuffer[128 * 2]; // Buffer for 32-bit Stereo
    
    unsigned long silenceStart = millis();
    const unsigned long SILENCE_DURATION = 2000; // Stop after 2 seconds of silence
    const unsigned long MAX_INITIAL_SILENCE = 5000; // Stop after 5 seconds if no speech detected
    bool voiceDetectedTotal = false;

    while (samplesRead < numSamples) {
        if (i2s_channel_read(rx_handle, sampleBuffer, sizeof(sampleBuffer), &bytesRead, portMAX_DELAY) == ESP_OK) {
            int samplesInBatch = bytesRead / 8; // 8 bytes per stereo frame
            bool voiceDetectedInBatch = false;

            for (int i = 0; i < samplesInBatch && samplesRead < numSamples; i++) {
                // Convert 32-bit Stereo to 16-bit Mono (Left Channel)
                int32_t raw = sampleBuffer[i*2] >> 13;
                if (raw > 32767) raw = 32767; else if (raw < -32768) raw = -32768;
                
                if (abs(raw) > silenceThreshold) {
                    voiceDetectedInBatch = true;
                    voiceDetectedTotal = true;
                }
                pcmBuffer[samplesRead++] = (int16_t)raw;
            }

            if (voiceDetectedInBatch) {
                silenceStart = millis();
            } else {
                unsigned long silenceTime = millis() - silenceStart;
                if (voiceDetectedTotal && (silenceTime > SILENCE_DURATION)) {
                    Serial.println("Silence detected. Stopping recording.");
                    break;
                }
                if (!voiceDetectedTotal && (silenceTime > MAX_INITIAL_SILENCE)) {
                    Serial.println("No speech detected (Timeout). Aborting.");
                    free(wavBuffer);
                    s_is_recording = false;
                    return nullptr;
                }
            }
        }
    }

    if (!voiceDetectedTotal) {
        Serial.println("No speech detected (Max Duration). Aborting.");
        free(wavBuffer);
        s_is_recording = false;
        return nullptr;
    }

    // Update WAV Header with actual size
    size_t actualDataSize = samplesRead * 2;
    size_t actualFileSize = sizeof(WavHeader) + actualDataSize;
    WavHeader* headerPtr = (WavHeader*)wavBuffer;
    headerPtr->dataSize = actualDataSize;
    headerPtr->overallSize = actualFileSize - 8;
    
    *outSize = actualFileSize;
    s_is_recording = false; // Resume AEC task
    return wavBuffer;
}