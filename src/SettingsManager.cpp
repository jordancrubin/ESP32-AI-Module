/*
  SettingsManager.cpp - ESP32 AI Module Configuration
  Implementation of NVS preference storage for system settings.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/
#include "SettingsManager.h"

SettingsManager::SettingsManager() {
    // Initialize with defaults
    strlcpy(wifiSSID, "YOUR_WIFI_SSID", sizeof(wifiSSID));
    strlcpy(wifiPass, "YOUR_WIFI_PASSWORD", sizeof(wifiPass));
    strlcpy(apiUrl, "http://your-api-endpoint/api/chat/completions", sizeof(apiUrl));
    strlcpy(apiKey, "your_api_key_here", sizeof(apiKey));
    strlcpy(llmModel, "llama3.2:3b", sizeof(llmModel));
    volume = 21;     // Default max volume
    brightness = 255; // Default max brightness
    calibration = {0, 0, 0, 0, false};
    strlcpy(timeZone, "UTC0", sizeof(timeZone)); // Default to UTC
    strlcpy(clockColor, "red", sizeof(clockColor));
}

void SettingsManager::begin() {
    prefs.begin("ai-config", false); // Namespace "ai-config", read/write
    load();
}

void SettingsManager::load() {
    // Load from NVS, fallback to current values (defaults) if not found
    if (prefs.isKey("ssid")) prefs.getString("ssid", wifiSSID, sizeof(wifiSSID));
    if (prefs.isKey("pass")) prefs.getString("pass", wifiPass, sizeof(wifiPass));
    if (prefs.isKey("api_url")) prefs.getString("api_url", apiUrl, sizeof(apiUrl));
    if (prefs.isKey("api_key")) prefs.getString("api_key", apiKey, sizeof(apiKey));
    if (prefs.isKey("model")) prefs.getString("model", llmModel, sizeof(llmModel));
    volume = prefs.getInt("vol", volume);
    brightness = prefs.getInt("bri", brightness);
    if (prefs.isKey("tz")) prefs.getString("tz", timeZone, sizeof(timeZone));
    if (prefs.isKey("clk_col")) prefs.getString("clk_col", clockColor, sizeof(clockColor));
    
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
    prefs.putString("tz", timeZone);
    prefs.putString("clk_col", clockColor);
}