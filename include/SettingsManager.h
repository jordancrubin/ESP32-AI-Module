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

private:
    Preferences prefs;
};

extern SettingsManager settings;