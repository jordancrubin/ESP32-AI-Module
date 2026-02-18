#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "WiFiManager.h"

typedef void (*ProgressCallback)(int percent, float speed);

class LLMClient {
public:
    LLMClient(const char* apiUrl, const char* apiKey, const char* model);
    void setConfig(String apiUrl, String apiKey, String model);
    String sendPrompt(String prompt, WiFiManager& netMgr);
    String getModels(WiFiManager& netMgr);
    String getVoices(WiFiManager& netMgr);
    void clearHistory();
    void setSystemPrompt(const char* prompt);
    bool downloadTTS(String text, WiFiManager& netMgr, const char* filename, String voice = "alloy", ProgressCallback cb = nullptr);
    String transcribeAudio(uint8_t* audioData, size_t size, WiFiManager& netMgr);

private:
    String _apiUrl;
    String _apiKey;
    String _model;
    JsonDocument _historyDoc;
    JsonArray _history;
    char* _systemPrompt;
};