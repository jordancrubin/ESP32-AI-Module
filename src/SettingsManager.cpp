#include "SettingsManager.h"

SettingsManager::SettingsManager() {
    // Initialize with defaults
    wifiSSID = "YOUR_WIFI_SSID";
    wifiPass = "YOUR_WIFI_PASSWORD";
    apiUrl = "http://your-api-endpoint/api/chat/completions";
    apiKey = "your_api_key_here";
    llmModel = "llama3.2:3b";
    volume = 21;     // Default max volume
    brightness = 255; // Default max brightness
    calibration = {0, 0, 0, 0, false};
}

void SettingsManager::begin() {
    prefs.begin("ai-config", false); // Namespace "ai-config", read/write
    load();
}

void SettingsManager::load() {
    // Load from NVS, fallback to current values (defaults) if not found
    wifiSSID = prefs.getString("ssid", wifiSSID);
    wifiPass = prefs.getString("pass", wifiPass);
    apiUrl = prefs.getString("api_url", apiUrl);
    apiKey = prefs.getString("api_key", apiKey);
    llmModel = prefs.getString("model", llmModel);
    volume = prefs.getInt("vol", volume);
    brightness = prefs.getInt("bri", brightness);
    
    if (prefs.getBytesLength("cal") == sizeof(TouchCalibration)) {
        prefs.getBytes("cal", &calibration, sizeof(TouchCalibration));
    }
}

void SettingsManager::save() {
    prefs.putString("ssid", wifiSSID);
    prefs.putString("pass", wifiPass);
    prefs.putString("api_url", apiUrl);
    prefs.putString("api_key", apiKey);
    prefs.putString("model", llmModel);
    prefs.putInt("vol", volume);
    prefs.putInt("bri", brightness);
    prefs.putBytes("cal", &calibration, sizeof(TouchCalibration));
}