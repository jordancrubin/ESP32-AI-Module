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
    silenceThreshold = 800;
    strlcpy(openWeatherKey, "", sizeof(openWeatherKey));
    strlcpy(weatherLocation, "New York,US", sizeof(weatherLocation));
    micMode = 1; // Default to Left Channel Only (0=Stereo, 1=Left, 2=Right)
    debugMode = false;
    inputBalance = 0; // Center
    strlcpy(systemPrompt, "You are a helpful AI assistant running on an ESP32-S3.", sizeof(systemPrompt));
    ttsProvider = 0; // Default to OpenWebUI
    ttsUrl[0] = '\0';
    enableWebSearch = false; // Default to false
    enableMemory = false;
    enableInterrupt = false;
    aecDelay = 640; // Default ~40ms
    aecAttenuation = 4; // Default /4 (75% attenuation)
    aecCutoff = 1000; // Default 1000 raw amplitude
    strlcpy(knowledgeId, "", sizeof(knowledgeId));
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
    silenceThreshold = prefs.getInt("sil_thresh", silenceThreshold);
    if (prefs.isKey("ow_key")) prefs.getString("ow_key", openWeatherKey, sizeof(openWeatherKey));
    if (prefs.isKey("ow_loc")) prefs.getString("ow_loc", weatherLocation, sizeof(weatherLocation));
    micMode = prefs.getInt("mic_mode", micMode);
    debugMode = prefs.getBool("debug", debugMode);
    inputBalance = prefs.getInt("in_bal", inputBalance);
    if (prefs.isKey("sys_prompt")) prefs.getString("sys_prompt", systemPrompt, sizeof(systemPrompt));
    ttsProvider = prefs.getInt("tts_prov", ttsProvider);
    if (prefs.isKey("tts_url")) prefs.getString("tts_url", ttsUrl, sizeof(ttsUrl));
    enableWebSearch = prefs.getBool("web_srch", enableWebSearch);
    enableMemory = prefs.getBool("mem_en", enableMemory);
    enableInterrupt = prefs.getBool("int_en", enableInterrupt);
    aecDelay = prefs.getInt("aec_dly", aecDelay);
    aecAttenuation = prefs.getInt("aec_att", aecAttenuation);
    aecCutoff = prefs.getInt("aec_cut", aecCutoff);
    if (prefs.isKey("know_id")) prefs.getString("know_id", knowledgeId, sizeof(knowledgeId));
    
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
    prefs.putInt("sil_thresh", silenceThreshold);
    prefs.putString("ow_key", openWeatherKey);
    prefs.putString("ow_loc", weatherLocation);
    prefs.putInt("mic_mode", micMode);
    prefs.putBool("debug", debugMode);
    prefs.putInt("in_bal", inputBalance);
    prefs.putString("sys_prompt", systemPrompt);
    prefs.putInt("tts_prov", ttsProvider);
    prefs.putString("tts_url", ttsUrl);
    prefs.putBool("web_srch", enableWebSearch);
    prefs.putBool("mem_en", enableMemory);
    prefs.putBool("int_en", enableInterrupt);
    prefs.putInt("aec_dly", aecDelay);
    prefs.putInt("aec_att", aecAttenuation);
    prefs.putInt("aec_cut", aecCutoff);
    prefs.putString("know_id", knowledgeId);
}