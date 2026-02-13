#include "LLMClient.h"

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
        return "Error: Invalid API URL format";
    }

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return "Error: Host resolution failed";

    Serial.println("Getting models from: " + serverPath);
    client.setTimeout(10000);
    http.setTimeout(10000);

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
                    if (name) {
                        result += String(name) + " (" + String(id) + ")";
                    } else if (id) {
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
    int retries = 3;

    while (retries > 0) {
        // Create fresh clients for each attempt to ensure clean state
        WiFiClient client;
        HTTPClient http;
        
        client.setTimeout(60000);
        http.setTimeout(60000);

        if (http.begin(client, serverPath)) {
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Authorization", "Bearer " + _apiKey);

            httpResponseCode = http.POST((uint8_t*)requestBuffer, requestSize);
            if (httpResponseCode > 0) {
                // Success! Process response immediately
                Serial.print("HTTP Response code: ");
                Serial.println(httpResponseCode);

                JsonDocument filter;
                filter["choices"][0]["message"]["content"] = true;

                JsonDocument responseDoc;
                // Parse directly from stream to save memory
                DeserializationError error = deserializeJson(responseDoc, http.getStream(), DeserializationOption::Filter(filter));

                if (!error) {
                    const char* content = responseDoc["choices"][0]["message"]["content"];
                    if (content) {
                        result = String(content);
                        // Add assistant response to history
                        Serial.printf("Free PSRAM after response: %u bytes\n", ESP.getFreePsram());
                        JsonObject assistantMsg = _history.add<JsonObject>();
                        assistantMsg["role"] = "assistant";
                        assistantMsg["content"] = result;
                    } else {
                        result = "Error: No content in response";
                    }
                } else {
                    result = "Error: JSON Parsing failed";
                }
                http.end();
                free(requestBuffer);
                return result;
            }
            
            Serial.printf("Request failed (Error: %d), retrying...\n", httpResponseCode);
            if (httpResponseCode == -1) {
                Serial.println("Hint: Check Server Binding (0.0.0.0) and Firewall rules.");
            }
            http.end();
        }
        Serial.printf("Request failed (Error: %d), retrying...\n", httpResponseCode);
        retries--;
        delay(1000);
        if (retries > 0) delay(1000);
    }

    free(requestBuffer);
    // If we reached here, all retries failed
    result = "Error: HTTP " + String(httpResponseCode);
    if (_history.size() > 0) _history.remove(_history.size() - 1);
    
    return result;
}

bool LLMClient::downloadTTS(String text, WiFiManager& netMgr, uint8_t** outBuffer, size_t* outSize, ProgressCallback cb) {
    if (!netMgr.isConnected()) return false;

    // Construct TTS URL based on Docker configuration (Port 8880)
    String url = _apiUrl;
    
    // Extract base URL (protocol + host)
    int doubleSlash = url.indexOf("//");
    int pathStart = url.indexOf("/", doubleSlash + 2);
    String base = (pathStart == -1) ? url : url.substring(0, pathStart);
    
    // Strip existing port if present (e.g. :3000)
    int portSep = base.lastIndexOf(":");
    if (portSep > doubleSlash) {
        base = base.substring(0, portSep);
    }
    
    url = base + ":8880/v1/audio/speech";

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return false;

    Serial.println("Downloading TTS from: " + serverPath);
    Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());

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
        doc["voice"] = "alloy"; // Options: alloy, echo, fable, onyx, nova, shimmer
        doc["response_format"] = "mp3"; // Options: mp3, opus, aac, flac, wav, pcm

        String requestBody;
        serializeJson(doc, requestBody);

        Serial.println("TTS Request Body:");
        Serial.println(requestBody);

        int httpCode = http.POST(requestBody);
        if (httpCode == 200) {
            int len = http.getSize();
            size_t allocSize;
            if (len > 0) {
                allocSize = len;
            } else {
                // If chunked (-1), allocate largest available PSRAM block minus safety margin (150KB)
                size_t maxBlock = ESP.getMaxAllocPsram();
                allocSize = (maxBlock > 150000) ? (maxBlock - 150000) : (1024 * 1024);
            }
            
            uint8_t* buffer = (uint8_t*)ps_malloc(allocSize);
            if (!buffer) {
                Serial.println("TTS Error: OOM (PSRAM)");
                http.end();
                return false;
            }

            int bytesWritten = 0;
            int lastBytesWritten = 0;
            WiFiClient *stream = http.getStreamPtr();
            unsigned long lastLog = millis();

            while (http.connected() && (len > 0 || len == -1)) {
                size_t size = stream->available();
                
                // Optimization: Wait for more data to arrive to read in larger chunks (up to 4KB)
                // This reduces overhead of calling read() too frequently for small packets
                if (size > 0 && size < 4096 && http.connected()) {
                    delay(1);
                    size = stream->available();
                }

                if (size) {
                    // Read directly into PSRAM buffer to increase speed and save stack
                    size_t remaining = allocSize - bytesWritten;
                    size_t readSize = (size > remaining) ? remaining : size;

                    // Use read() instead of readBytes() for potentially lower overhead
                    int c = stream->read(buffer + bytesWritten, readSize);

                    if (c > 0) {
                        bytesWritten += c;
                        if (len > 0) len -= c;
                    }

                    if (bytesWritten >= allocSize && len == -1) {
                         Serial.printf("TTS Warning: Buffer full. Limit: %u bytes. Truncating.\n", allocSize);
                         break;
                    }
                } else {
                    // Give the TCP stack a moment to receive more packets
                    delay(1); 
                }

                if (millis() - lastLog > 1000) {
                    float speed = (bytesWritten - lastBytesWritten) / 1024.0;
                    Serial.printf("Speed: %.1f KB/s. RSSI: %d dBm. DL: %d / %u bytes.\n", 
                        speed, netMgr.getSignalStrength(), bytesWritten, allocSize);
                    lastLog = millis();
                    lastBytesWritten = bytesWritten;
                }

                // Update UI more frequently than once per second
                if (cb && bytesWritten % 8192 == 0) {
                    cb((int)((bytesWritten * 100) / allocSize), 0);
                }
            }

            http.end();
            if (bytesWritten > 0) {
                *outBuffer = buffer;
                *outSize = bytesWritten;
                return true;
            } else {
                free(buffer);
                Serial.println("TTS Error: Downloaded 0 bytes");
                return false;
            }
        } else {
            Serial.printf("TTS Error: HTTP %d\n", httpCode);
            if (httpCode > 0) Serial.println(http.getString());
        }
        http.end();
    }
    return false;
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