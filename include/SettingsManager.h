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
    String wifiSSID;
    String wifiPass;
    String apiUrl;
    String apiKey;
    String llmModel;
    int volume;
    int brightness;
    TouchCalibration calibration;

private:
    Preferences prefs;
};