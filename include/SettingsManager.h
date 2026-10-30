/*
  SettingsManager.h - ESP32 AI Module Configuration
  Defines persistent settings structure and NVS storage interface.
  
  https://www.youtube.com/@rubin-tech
  2026 Jordan Rubin.
*/
#pragma once
#include <Arduino.h>
#include <Preferences.h>

struct TouchCalibration {
    uint16_t xMin;
    uint16_t xMax;
    uint16_t yMin;
    uint16_t yMax;
    bool isValid;
};

class SettingsManager {
public:
    SettingsManager();
    void begin();
    void load();
    void save();

    // Public members for easy access
    char wifiSSID[33];
    char wifiPass[65];
    char apiUrl[128];
    char apiKey[128];
    char llmModel[65];
    char timeZone[65]; 
    char clockColor[16];
    int volume;
    int brightness;
    TouchCalibration calibration;
    int silenceThreshold;
    char openWeatherKey[65];
    char weatherLocation[65];
    int micMode; // 0=Stereo, 1=Left, 2=Right
    int inputBalance; // -100 (Left) to 100 (Right)
    bool debugMode;
    char systemPrompt[512];
    int ttsProvider;
    char ttsUrl[256];
    char ttsFormat[8]; // "mp3" or "wav"
    bool enableWebSearch;
    bool enableCalendar;
    bool enableMemory;
    bool enableInterrupt;
    int aecDelay;
    int aecAttenuation;
    int aecCutoff;
    int aecIgnore;
    int bargeStreak;  // Consecutive loud frames for yell trigger (default: 5 frames / ~80ms)
    int bargeDensity; // Active frames out of 24 for speech density trigger (default: 8 frames / ~384ms)
    char knowledgeId[64];
    bool alarmEnabled;
    int alarmHour;
    int alarmMinute;
    int speakerHpfCutoff; // High-pass filter cutoff in Hz (0 = Disabled)

private:
    Preferences prefs;
};

extern SettingsManager settings;