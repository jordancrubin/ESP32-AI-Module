/*
  LLMClient.cpp - ESP32 AI Module LLM Interface
  Handles HTTP communication with OpenAI-compatible APIs for Chat and TTS.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/
#include "LLMClient.h"
#include <LittleFS.h>

LLMClient::LLMClient(const char* apiUrl, const char* apiKey, const char* model)
    : _apiUrl(apiUrl), _apiKey(apiKey), _model(model), _systemPrompt(nullptr) {
    _history = _historyDoc.to<JsonArray>();
}

void LLMClient::setConfig(String apiUrl, String apiKey, String model) {
    _apiUrl = apiUrl;
    _apiKey = apiKey;
    _model = model;
}

String LLMClient::getModels(WiFiManager& netMgr) {
    if (!netMgr.isConnected()) return "Error: WiFi not connected";

    WiFiClient client;
    HTTPClient http;

    // Construct models URL from _apiUrl (http://.../api/chat/completions -> http://.../api/models)
    String url = _apiUrl;
    int splitIndex = url.indexOf("/api/");
    if (splitIndex != -1) {
        url = url.substring(0, splitIndex + 5) + "models";
    } else {
        // Also try /v1/ format (OpenAI compatible)
        splitIndex = url.indexOf("/v1/");
        if (splitIndex != -1) {
            url = url.substring(0, splitIndex) + "/api/models";
        } else {
            Serial.println("LLMClient Error: Invalid API URL format. Current URL: " + _apiUrl);
            return "Error: Invalid API URL format";
        }
    }

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return "Error: Host resolution failed";

    Serial.println("Getting models from: " + serverPath);
    client.setTimeout(5000);
    http.setTimeout(5000);

    if (http.begin(client, serverPath)) {
        http.addHeader("Authorization", "Bearer " + _apiKey);
        int httpCode = http.GET();
        
        String result = "";
        if (httpCode > 0) {
            // Filter to extract only id and name from the data array
            JsonDocument filter;
            filter["data"][0]["id"] = true;
            filter["data"][0]["name"] = true;

            JsonDocument doc;
            // Parse directly from stream to save memory (fixes OOM on boot)
            DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));

            if (!error) {
                JsonArray data = doc["data"];
                for (JsonObject v : data) {
                    const char* id = v["id"];
                    const char* name = v["name"];
                    
                    if (result.length() > 0) result += "\n";
                    if (id) {
                        result += String(id);
                    }
                }
                if (result.length() == 0) result = "No models found";
            } else {
                result = "Error: JSON Parsing failed";
            }
        } else {
            result = "Error: HTTP " + String(httpCode);
        }
        http.end();
        return result;
    }
    return "Error: Connection failed";
}

String LLMClient::sendPrompt(String prompt, WiFiManager& netMgr) {
    if (!netMgr.isConnected()) return "Error: WiFi not connected";

    WiFiClient client;
    HTTPClient http;
    
    String serverPath = netMgr.resolveHost(_apiUrl);
    if (serverPath == "") {
        return "Error: Host resolution failed";
    }
    
    Serial.println("Sending request to: " + serverPath);

    client.setTimeout(60000);
    http.setTimeout(60000);
    
    // Sliding window: Limit history to prevent OOM
    // Keep system prompt (if exists) and last N messages
    const int MAX_HISTORY = 8; 
    while (_history.size() > MAX_HISTORY) {
        if (_history[0]["role"] == "system" && _history.size() > 1) {
            _history.remove(1);
        } else {
            _history.remove(0);
        }
    }

    // Add user message to history
    JsonObject userMsg = _history.add<JsonObject>();
    userMsg["role"] = "user";
    userMsg["content"] = prompt;

    JsonDocument doc;
    doc["model"] = _model;
    doc["stream"] = false;
    doc["messages"] = _history;

    // Serialize to PSRAM to save Internal RAM
    size_t requestSize = measureJson(doc);
    char* requestBuffer = (char*)ps_malloc(requestSize + 1);
    if (!requestBuffer) return "Error: OOM (Request Buffer)";
    serializeJson(doc, requestBuffer, requestSize + 1);

    Serial.println("Request Body:");
    Serial.println(requestBuffer);

    String result = "";
    int httpResponseCode = -1;

    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", "Bearer " + _apiKey);

        httpResponseCode = http.POST((uint8_t*)requestBuffer, requestSize);
        if (httpResponseCode > 0) {
            Serial.print("HTTP Response code: ");
            Serial.println(httpResponseCode);

            JsonDocument filter;
            filter["choices"][0]["message"]["content"] = true;

            JsonDocument responseDoc;
            DeserializationError error = deserializeJson(responseDoc, http.getStream(), DeserializationOption::Filter(filter));

            if (!error) {
                const char* content = responseDoc["choices"][0]["message"]["content"];
                if (content) {
                    result = String(content);
                    JsonObject assistantMsg = _history.add<JsonObject>();
                    assistantMsg["role"] = "assistant";
                    assistantMsg["content"] = result;
                } else {
                    result = "Error: No content in response";
                }
            } else {
                result = "Error: JSON Parsing failed";
            }
        } else {
            result = "Error: HTTP " + String(httpResponseCode);
            if (_history.size() > 0) _history.remove(_history.size() - 1);
        }
        http.end();
    } else {
        result = "Error: Connection failed";
        if (_history.size() > 0) _history.remove(_history.size() - 1);
    }

    free(requestBuffer);
    return result;
}

bool LLMClient::downloadTTS(String text, WiFiManager& netMgr, const char* filename, String voice, ProgressCallback cb) {
    (void)cb; // Mark unused to prevent compiler warnings
    if (!netMgr.isConnected()) return false;

    // Construct TTS URL based on Docker configuration (Port 8880)
    String url = _apiUrl;
    
    // Extract base URL (protocol + host)
    int doubleSlash = url.indexOf("//");
    int pathStart = url.indexOf("/", doubleSlash + 2);
    String base = (pathStart == -1) ? url : url.substring(0, pathStart);
    
    // Strip existing port if present (e.g. :3000) and add :8880 for Kokoro TTS
    int portSep = base.lastIndexOf(":");
    if (portSep > doubleSlash) base = base.substring(0, portSep);

    url = base + ":8880/v1/audio/speech";

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return false;

    Serial.println("Downloading TTS from: " + serverPath);

    WiFiClient client;
    HTTPClient http;
    client.setTimeout(30000);
    http.setTimeout(30000);

    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", "Bearer " + _apiKey);

        JsonDocument doc;
        doc["model"] = "tts-1";
        doc["input"] = text;
        doc["voice"] = voice; // Options: alloy, echo, fable, onyx, nova, shimmer
        doc["response_format"] = "mp3"; // Options: mp3, opus, aac, flac, wav, pcm

        String requestBody;
        serializeJson(doc, requestBody);

        Serial.println("TTS Request Body:");
        Serial.println(requestBody);

        int httpCode = http.POST(requestBody);
        if (httpCode == 200) {
            File file = LittleFS.open(filename, "w");
            if (!file) {
                Serial.println("TTS Error: Failed to open file for writing");
                http.end();
                return false;
            }

            http.writeToStream(&file);
            file.close();
            http.end();
            
            Serial.println("TTS Download Complete");
            return true;
        } else {
            Serial.printf("TTS Error: HTTP %d\n", httpCode);
            if (httpCode > 0) Serial.println(http.getString());
        }
        http.end();
    }
    return false;
}

String LLMClient::getVoices(WiFiManager& netMgr) {
    if (!netMgr.isConnected()) return "Error: WiFi not connected";

    // Construct TTS URL based on Docker configuration (Port 8880)
    String url = _apiUrl;
    
    // Extract base URL (protocol + host)
    int doubleSlash = url.indexOf("//");
    int pathStart = url.indexOf("/", doubleSlash + 2);
    String base = (pathStart == -1) ? url : url.substring(0, pathStart);
    
    // Strip existing port if present (e.g. :3000) and add :8880 for Kokoro TTS
    int portSep = base.lastIndexOf(":");
    if (portSep > doubleSlash) base = base.substring(0, portSep);

    url = base + ":8880/v1/audio/voices";

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return "Error: Host resolution failed";

    Serial.println("Getting voices from: " + serverPath);

    WiFiClient client;
    HTTPClient http;
    client.setTimeout(5000);
    http.setTimeout(5000);

    if (http.begin(client, serverPath)) {
        http.addHeader("Authorization", "Bearer " + _apiKey);
        int httpCode = http.GET();
        String result = (httpCode > 0) ? http.getString() : ("Error: HTTP " + String(httpCode));
        http.end();
        return result;
    }
    return "Error: Connection failed";
}

void LLMClient::clearHistory() {
    _historyDoc.clear();
    _history = _historyDoc.to<JsonArray>();
    
    // Re-add system prompt if it exists
    if (_systemPrompt) {
        JsonObject systemMsg = _history.add<JsonObject>();
        systemMsg["role"] = "system";
        systemMsg["content"] = _systemPrompt;
    }
}

void LLMClient::setSystemPrompt(const char* prompt) {
    if (_systemPrompt) {
        free(_systemPrompt);
        _systemPrompt = nullptr;
    }
    if (prompt) {
        // Allocate in PSRAM
        _systemPrompt = (char*)ps_malloc(strlen(prompt) + 1);
        if (_systemPrompt) {
            strcpy(_systemPrompt, prompt);
            clearHistory(); // Reset history to apply the new system prompt
        }
    }
}