/*
  LLMClient.cpp - ESP32 AI Module LLM Interface
  Handles HTTP communication with OpenAI-compatible APIs for Chat and TTS.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/
#include "LLMClient.h"
#include <FS.h>
#include <LittleFS.h>
#include "SettingsManager.h"
#include "DisplayManager.h"
#include <lvgl.h>

extern SettingsManager settings;
extern DisplayManager display;

extern bool enableTTSChunking;
extern bool playChunkedTTS(String text);
extern float ttsSpeed;
extern void flushAecBuffer();

String LLMClient::getTtsBaseUrl() {
    if (settings.ttsProvider == 1 && strlen(settings.ttsUrl) > 0) {
        // Direct Mode: Use the specified URL
        return String(settings.ttsUrl);
    }
    else {
        // OpenWebUI Mode: Derive from main API URL (preserve port/host)
        String url = _apiUrl;
        
        // Strip the specific chat endpoint to get the base
        // e.g. http://192.168.1.50:3000/api/chat/completions -> http://192.168.1.50:3000/api
        int split = url.indexOf("/chat/completions");
        if (split != -1) url = url.substring(0, split);

        if (url.endsWith("/")) url = url.substring(0, url.length() - 1);
        // If the base ends in /v1, strip it because downloadTTS appends /v1/audio/speech
        if (url.endsWith("/v1")) url = url.substring(0, url.length() - 3);

        return url;
    }
}

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

    String url = _apiUrl;
    int chatIndex = url.indexOf("/chat/completions");
    if (chatIndex != -1) url = url.substring(0, chatIndex);
    if (url.endsWith("/")) url = url.substring(0, url.length() - 1);

    // Decouple LLM Model fetching from the TTS Provider setting.
    // If the user points to a local /v1 endpoint, force it to /api/models for OpenWebUI compatibility.
    int v1Index = url.indexOf("/v1");
    if (v1Index != -1 && url.indexOf("api.openai.com") == -1) {
        // User configured a /v1 endpoint for a local server, assume OpenWebUI
        url = url.substring(0, v1Index) + "/api/models";
    } else if (url.endsWith("/api")) {
        // Ambiguous /api endpoint. Check for Ollama's default port.
        if (url.indexOf(":11434") != -1) {
            url += "/tags"; // Native Ollama
        } else {
            url += "/models"; // Assume OpenWebUI
        }
    } else {
        // Standard OpenAI or other compatible service
        if (!url.endsWith("/v1")) url += "/v1";
        url += "/models"; // Standard OpenAI
    }

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return "Error: Host resolution failed";

    if (settings.debugMode) Serial.println("Getting models from: " + serverPath);
    client.setTimeout(10000);
    http.setTimeout(10000);
    http.setConnectTimeout(10000);

    if (http.begin(client, serverPath)) {
        http.addHeader("Authorization", "Bearer " + _apiKey);
        int httpCode = http.GET();
        
        String result = "";
        if (httpCode == 200) {
            // Filter to extract only id and name from the data array
            JsonDocument filter;
            filter["data"][0]["id"] = true;
            filter["data"][0]["name"] = true;
            filter["models"][0]["name"] = true; // Ollama native fallback

            JsonDocument doc;
            // Parse directly from stream to save memory (fixes OOM on boot)
            DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));

            if (!error) {
                if (doc.containsKey("data")) {
                    JsonArray data = doc["data"];
                    for (JsonObject v : data) {
                        const char* id = v["id"] ? v["id"] : v["name"];
                        if (id) {
                            if (result.length() > 0) result += "\n";
                            result += id;
                        }
                    }
                } else if (doc.containsKey("models")) {
                    JsonArray data = doc["models"];
                    for (JsonObject v : data) {
                        const char* id = v["name"];
                        if (id) {
                            if (result.length() > 0) result += "\n";
                            result += id;
                        }
                    }
                }
                if (result.length() == 0) result = "Error: No models found in JSON array";
            }
            else {
                result = "Error: JSON Parsing failed - " + String(error.c_str());
            }
        }
        else {
            String errorResponse = http.getString();
            if (settings.debugMode) Serial.println("Models HTTP Error " + String(httpCode) + ": " + errorResponse);
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
    
    String url = _apiUrl;
    if (url.endsWith("/")) url = url.substring(0, url.length() - 1);

    
    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") {
        return "Error: Host resolution failed";
    }
    
    if (settings.debugMode) Serial.println("Sending request to: " + serverPath);

    client.setTimeout(60000);
    http.setTimeout(60000);
    
    // Sliding window: Limit history to prevent OOM
    // Keep system prompt (if exists) and last N messages
    const int MAX_HISTORY = 8; 
    while (_history.size() > MAX_HISTORY) {
        if (_history[0]["role"] == "system" && _history.size() > 1) {
            _history.remove(1);
        }
        else {
            _history.remove(0);
        }
    }

    // Add user message to history
    JsonObject userMsg = _history.add<JsonObject>();
    userMsg["role"] = "user";
    userMsg["content"] = prompt;

    JsonDocument doc;
    doc["model"] = _model;
    doc["stream"] = true;
    doc["messages"] = _history;
    
    // OpenWebUI Workaround: Prevent NoneType crash by supplying empty chat_id
    doc["metadata"]["chat_id"] = "";
    doc["chat_id"] = "";
    doc["session_id"] = "";
    
    // Check for web search trigger phrases in the prompt
    String lowerPrompt = prompt;
    lowerPrompt.toLowerCase();
    bool useWebSearch = (lowerPrompt.indexOf("search the web") != -1) ||
                        (lowerPrompt.indexOf("do a search") != -1) ||
                        (lowerPrompt.indexOf("check online") != -1) ||
                        (lowerPrompt.indexOf("search online") != -1) ||
                        (lowerPrompt.indexOf("look up online") != -1) ||
                        (lowerPrompt.indexOf("search the internet") != -1);

    if (useWebSearch) {
        
        // Dynamically append web search rules to the system prompt for this request
        if (doc["messages"].size() > 0 && doc["messages"][0]["role"] == "system") {
            String sysText = doc["messages"][0]["content"].as<String>();
            sysText += " Use web search ONLY for real-time info. For basic facts or math, answer immediately. IMPORTANT: Provide direct answers only. Never mention 'searching the web,' 'according to the website,' or 'my search results.' Do not use phrases like 'I'll do a web search' or 'Searching online.' If you find information via tools, integrate it naturally into your speech as if you already knew it. No citations or [1] brackets. Short, conversational responses only.";
            doc["messages"][0]["content"] = sysText;
        }
    }

    // Serialize to PSRAM to save Internal RAM
    size_t requestSize = measureJson(doc);
    char* requestBuffer = (char*)ps_malloc(requestSize + 1);
    if (!requestBuffer) return "Error: OOM (Request Buffer)";
    serializeJson(doc, requestBuffer, requestSize + 1);

    if (settings.debugMode) {
        Serial.println("Request Body:");
        Serial.println(requestBuffer);
    }

    String result = "";
    int httpResponseCode = -1;

    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", "Bearer " + _apiKey);

        httpResponseCode = http.POST((uint8_t*)requestBuffer, requestSize);
        if (httpResponseCode == 200) {
            if (settings.debugMode) {
                Serial.print("HTTP Response code: ");
                Serial.println(httpResponseCode);
                Serial.println("--- Stream START ---");
            }

            WiFiClient* stream = http.getStreamPtr();
            String full_response = "";
            full_response.reserve(2048); // Pre-allocate to prevent heap fragmentation
            
            String streamBuffer = "";

            while (http.connected() || stream->available()) {
                if (stream->available()) {
                    char buf[128];
                    int available = stream->available();
                    int toRead = available > 127 ? 127 : available;
                    int bytesRead = stream->read((uint8_t*)buf, toRead);
                    
                    if (bytesRead > 0) {
                        buf[bytesRead] = '\0';
                        streamBuffer += buf;
                        
                        // Parse Standard Server-Sent Events (SSE) / JSON Streams
                        int newlineIdx;
                        while ((newlineIdx = streamBuffer.indexOf('\n')) != -1) {
                            String line = streamBuffer.substring(0, newlineIdx);
                            streamBuffer = streamBuffer.substring(newlineIdx + 1);
                            line.trim();
                            
                            if (line.startsWith("data:")) {
                                String jsonStr = line.substring(5);
                                jsonStr.trim();
                                if (jsonStr != "[DONE]") {
                                    JsonDocument chunkDoc;
                                    DeserializationError err = deserializeJson(chunkDoc, jsonStr);
                                    if (!err) {
                                        const char* content = chunkDoc["choices"][0]["delta"]["content"];
                                        if (!content) content = chunkDoc["message"]["content"];
                                        if (content) full_response += content;
                                    }
                                }
                            }
                        }
                    }
                }
                else {
                    flushAecBuffer(); // Prevent AEC buffer overflow while blocked by network
                    delay(2); // Yield to watchdogs while waiting for next network packet
                }
            }
            
            if (settings.debugMode) {
                Serial.println("\n--- Stream END ---");
                Serial.println("Parsed Response: " + full_response);
            }
            
            // Final UI update to show the complete response now that audio processing has finished
            display.showResponse(full_response);
            lv_timer_handler();
            
            if (full_response.length() > 0) {
                result = full_response;
                JsonObject assistantMsg = _history.add<JsonObject>();
                assistantMsg["role"] = "assistant";
                assistantMsg["content"] = result;
            }
            else {
                result = "Error: Empty stream response";
            }
        }
        else {
            result = "Error: HTTP " + String(httpResponseCode);
            if (httpResponseCode > 0) result += " " + http.getString();
            if (_history.size() > 0) _history.remove(_history.size() - 1);
        }
        http.end();
    }
    else {
        result = "Error: Connection failed";
        if (_history.size() > 0) _history.remove(_history.size() - 1);
    }

    free(requestBuffer);
    return result;
}

bool LLMClient::downloadTTS(String text, WiFiManager& netMgr, const char* filename, String voice, ProgressCallback cb) {
    (void)cb; // Mark unused to prevent compiler warnings
    if (!netMgr.isConnected()) return false;

    // Construct TTS URL using helper
    String baseUrl = getTtsBaseUrl();
    if (baseUrl.length() == 0) {
        if (settings.debugMode) Serial.println("TTS Error: TTS URL is not configured for Direct mode.");
        return false;
    }

    String url = baseUrl;

    // Only modify URL if we are in OpenWebUI mode (0).
    // In Direct Mode (1), we use the URL exactly as input.
    if (settings.ttsProvider == 0) {
        int voicesIdx = url.indexOf("/v1/audio/voices");
        if (voicesIdx != -1) url = url.substring(0, voicesIdx);

        if (url.indexOf("/v1/audio/speech") == -1) {
            url += (url.endsWith("/") ? "" : "/") + String("v1/audio/speech");
        }
    }

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return false;

    if (settings.debugMode) Serial.println("Downloading TTS from: " + serverPath);

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
        doc["speed"] = ttsSpeed;

        String requestBody;
        serializeJson(doc, requestBody);

        if (settings.debugMode) {
            Serial.println("TTS Request Body:");
            Serial.println(requestBody);
        }

        int httpCode = http.POST(requestBody);
        if (httpCode == 200) {
            File file = LittleFS.open(filename, "w");
            if (!file) {
                if (settings.debugMode) Serial.println("TTS Error: Failed to open file for writing");
                http.end();
                return false;
            }

            http.writeToStream(&file);
            file.close();
            http.end();
            
            flushAecBuffer(); // Clear any microphone ringbuffer overflows that occurred while blocked by the network
            
            if (settings.debugMode) Serial.println("TTS Download Complete");
            return true;
        }
        else {
            if (settings.debugMode) Serial.printf("TTS Error: HTTP %d\n", httpCode);
            if (httpCode > 0 && settings.debugMode) Serial.println(http.getString());
        }
        http.end();
    }
    return false;
}

String LLMClient::transcribeAudio(uint8_t* audioData, size_t size, WiFiManager& netMgr) {
    if (!netMgr.isConnected()) return "Error: WiFi not connected";
    if (!audioData || size == 0) return "Error: Empty audio data";

    // Construct Transcription URL from _apiUrl
    // e.g. http://host:8080/api/chat/completions -> http://host:8080/api/audio/transcriptions
    String url = _apiUrl;
    int chatIndex = url.indexOf("/chat/completions");
    if (chatIndex != -1) {
        url = url.substring(0, chatIndex);
    }
    if (url.endsWith("/")) url = url.substring(0, url.length() - 1);
    
    // Decouple transcription endpoint from TTS Provider.
    // OpenWebUI strictly hosts the native transcription engine at /api/v1/audio/transcriptions
    int v1Index = url.indexOf("/v1");
    if (v1Index != -1 && url.indexOf("api.openai.com") == -1) {
        url = url.substring(0, v1Index);
        url += "/api/v1/audio/transcriptions";
    }
    else if (url.endsWith("/v1")) {
        url += "/audio/transcriptions";
    }
    else if (url.endsWith("/api")) {
        url += "/v1/audio/transcriptions";
    }
    else {
        url += "/audio/transcriptions";
    }

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return "Error: Host resolution failed";

    if (settings.debugMode) {
        Serial.println("[" + String(millis()) + "] Transcribing at: " + serverPath);
        Serial.println("[" + String(millis()) + "] Audio Size: " + String(size));
    }

    // Parse Host, Port, Path from resolvedUrl
    int protocolEnd = serverPath.indexOf("://");
    int hostStart = (protocolEnd == -1) ? 0 : protocolEnd + 3;
    int pathStart = serverPath.indexOf('/', hostStart);
    
    String hostPort = (pathStart == -1) ? serverPath.substring(hostStart) : serverPath.substring(hostStart, pathStart);
    String path = (pathStart == -1) ? "/" : serverPath.substring(pathStart);
    
    String host = hostPort;
    int port = 80;
    int portIndex = hostPort.indexOf(':');
    if (portIndex != -1) {
        host = hostPort.substring(0, portIndex);
        port = hostPort.substring(portIndex + 1).toInt();
    }

    WiFiClient client;
    if (!client.connect(host.c_str(), port)) {
        if (settings.debugMode) Serial.println("[" + String(millis()) + "] Connection failed to " + host + ":" + String(port));
        return "Error: Connection failed";
    }
    if (settings.debugMode) Serial.println("[" + String(millis()) + "] Connected to " + host + ":" + String(port));
    client.setTimeout(60000);

    // 1. Define the Boundary
    String boundary = "------------------------ESP32Boundary" + String(millis());
    
    // 2. Construct the Body Parts
    // Part 1: Model + File Header
    String part1 = "--" + boundary + "\r\n" +
                   "Content-Disposition: form-data; name=\"model\"\r\n" +
                   "\r\n" +
                   "whisper-1\r\n" +
                   "--" + boundary + "\r\n" +
                   "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n" +
                   "Content-Type: audio/wav\r\n" +
                   "\r\n";
                   
    // Part 2: Footer
    String part2 = "\r\n--" + boundary + "--\r\n";

    // 3. Calculate Total Length
    size_t totalLength = part1.length() + size + part2.length();

    // 4. Send HTTP Headers
    if (settings.debugMode) Serial.println("[" + String(millis()) + "] Sending Headers...");
    client.println("POST " + path + " HTTP/1.1");
    client.println("Host: " + host + ":" + String(port));
    client.println("Authorization: Bearer " + _apiKey);
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.println("Content-Length: " + String(totalLength));
    client.println("User-Agent: ESP32");
    client.println("Connection: close");
    client.println(); // End of headers

    // 5. Send the Body
    if (settings.debugMode) Serial.println("[" + String(millis()) + "] Sending Body Part 1...");
    client.print(part1);
    
    if (settings.debugMode) Serial.println("[" + String(millis()) + "] Sending Audio Data (" + String(size) + " bytes)...");
    size_t bytesWritten = 0;
    size_t chunkSize = 1024;
    int retryCount = 0;
    while (bytesWritten < size) {
        // Intercept early server responses (usually HTTP errors) before writing to a potentially closed socket
        if (client.available() || !client.connected()) {
            if (client.available()) {
                if (settings.debugMode) {
                    Serial.println("[" + String(millis()) + "] SERVER REJECTED UPLOAD: Reading early response...");
                    while(client.available()) {
                        String line = client.readStringUntil('\n');
                        line.trim();
                        if (line.length() > 0) Serial.println(">> " + line);
                    }
                }
            }
            if (!client.connected()) {
                if (settings.debugMode) Serial.println("[" + String(millis()) + "] ERROR: Client disconnected at byte " + String(bytesWritten));
            }
            break;
        }
        size_t toWrite = (size - bytesWritten) < chunkSize ? (size - bytesWritten) : chunkSize;
        size_t written = client.write(audioData + bytesWritten, toWrite);
        
        if (written == 0) {
            retryCount++;
            if (settings.debugMode && retryCount % 10 == 0) {
                Serial.println("[" + String(millis()) + "] Network buffer full, waiting... (Retry " + String(retryCount) + "/100)");
            }
            if (retryCount > 100) { // 1 second timeout
                if (settings.debugMode) Serial.println("[" + String(millis()) + "] ERROR: Timeout writing audio data at byte " + String(bytesWritten));
                break;
            }
            delay(10);
        } else {
            bytesWritten += written;
            retryCount = 0;
        }
        flushAecBuffer(); // Prevent AEC buffer overflow while writing to network
    }
    if (settings.debugMode) Serial.println("[" + String(millis()) + "] Audio transmission finished. Sent: " + String(bytesWritten) + "/" + String(size));
    
    if (!client.connected() && bytesWritten < size) {
        client.stop();
        return "Error: Upload rejected by server (See Debug)";
    }

    if (settings.debugMode) Serial.println("[" + String(millis()) + "] Sending Body Part 2...");
    client.print(part2);

    // 6. Read Response
    if (settings.debugMode) Serial.println("[" + String(millis()) + "] Waiting for response headers...");
    String response = "";
    bool headersFinished = false;
    int contentLength = -1;
    unsigned long start = millis();
    String httpStatusLine = "";
    
    while (client.connected() || client.available()) {
        if (millis() - start > 60000) {
            if (settings.debugMode) Serial.println("[" + String(millis()) + "] ERROR: Timeout waiting for response");
            break;
        }
        flushAecBuffer(); // Prevent AEC buffer overflow while blocked by network
        if (client.available()) {
            if (!headersFinished) {
                String line = client.readStringUntil('\n');
                line.trim();
                if (httpStatusLine == "") {
                    httpStatusLine = line;
                    if (settings.debugMode) Serial.println("[" + String(millis()) + "] HTTP Status: " + httpStatusLine);
                } else if (line == "") {
                    headersFinished = true;
                    if (settings.debugMode) Serial.println("[" + String(millis()) + "] Headers finished. Content-Length: " + String(contentLength));
                }
                else {
                    if (settings.debugMode) Serial.println("> Header: " + line);
                    String lowerLine = line;
                    lowerLine.toLowerCase();
                    if (lowerLine.startsWith("content-length:")) {
                        contentLength = line.substring(15).toInt();
                        if (contentLength > 0) response.reserve(contentLength); // Prevent heap fragmentation!
                    }
                }
            } else {
                // Read Body
                if (contentLength != -1) {
                    // If we know the length, read exactly that much and stop
                    while (response.length() < (unsigned int)contentLength && (client.connected() || client.available())) {
                        if (client.available()) {
                            uint8_t buf[256];
                            int bytesRead = client.read(buf, sizeof(buf));
                            if (bytesRead > 0) response.concat((const char*)buf, bytesRead);
                        }
                        else {
                            flushAecBuffer(); // Prevent AEC buffer overflow during large body downloads
                            delay(1);
                        }
                    }
                    break; // Done reading
                }
                else {
                    // Fallback: read until close
                    uint8_t buf[256];
                    int bytesRead = client.read(buf, sizeof(buf));
                    if (bytesRead > 0) response.concat((const char*)buf, bytesRead);
                }
            }
        }
        else {
            delay(2); // Yield to prevent WDT starvation while waiting for network packet
        }
    }
    client.stop();

    if (settings.debugMode) {
        Serial.println("[" + String(millis()) + "] Raw Transcription Response:");
        Serial.println("========================================");
        Serial.println(response);
        Serial.println("========================================");
    }
    
    // Parse JSON result
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    String result = "";
    
    if (error) {
        result = "Error: JSON " + String(error.c_str());
    }
    else if (!doc["error"].isNull()) {
        result = "Error: " + doc["error"]["message"].as<String>();
    }
    else if (!doc["detail"].isNull()) {
        result = "Error: " + doc["detail"].as<String>();
    }
    else if (!doc["text"].isNull()) {
        result = doc["text"].as<String>();
        if (result.length() == 0) result = "Error: No speech";
    }
    else {
        result = "Error: Invalid API response";
    }
    
    return result;
}

String LLMClient::getVoices(WiFiManager& netMgr) {
    if (!netMgr.isConnected()) return "Error: WiFi not connected";

    // Construct TTS URL using helper
    String baseUrl = getTtsBaseUrl();
    if (baseUrl.length() == 0) {
        if (settings.debugMode) Serial.println("GetVoices Error: TTS URL is not configured for Direct mode.");
        return "Error: TTS URL not configured";
    }

    // Sanitize: If user pasted the speech URL into config by mistake, strip it
    int speechIdx = baseUrl.indexOf("/v1/audio/speech");
    if (speechIdx != -1) baseUrl = baseUrl.substring(0, speechIdx);

    String url = baseUrl;
    if (url.indexOf("/v1/audio/voices") == -1) {
        url += (url.endsWith("/") ? "" : "/") + String("v1/audio/voices");
    }

    String serverPath = netMgr.resolveHost(url);
    if (serverPath == "") return "Error: Host resolution failed";

    if (settings.debugMode) Serial.println("Getting voices from: " + serverPath);

    WiFiClient client;
    HTTPClient http;
    client.setTimeout(10000);
    http.setTimeout(10000);
    http.setConnectTimeout(10000);

    if (http.begin(client, serverPath)) {
        http.addHeader("Authorization", "Bearer " + _apiKey);
        int httpCode = http.GET();
        String result = "";
        if (httpCode == 200) {
            JsonDocument filter;
            filter["voices"][0]["id"] = true; // Only keep the ID string in memory
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
            
            if (!error && doc["voices"].is<JsonArray>()) {
                JsonArray voices = doc["voices"];
                for (JsonVariant v : voices) {
                    const char* voiceName = v.is<JsonObject>() ? v["id"] : v.as<const char*>();
                    if (voiceName && strlen(voiceName) > 0) {
                        char firstChar = voiceName[0];
                        if (firstChar == 'a' || firstChar == 'b' || firstChar == 'd') {
                            if (result.length() > 0) result += '\n';
                            result += voiceName;
                        }
                    }
                }
                if (result.length() == 0) result = "Error: No matching voices found";
            }
            else {
                result = "Error: JSON Parsing failed - " + String(error.c_str());
            }
        }
        else {
            String errorResponse = http.getString();
            if (settings.debugMode) Serial.println("Voices HTTP Error " + String(httpCode) + ": " + errorResponse);
            result = "Error: HTTP " + String(httpCode);
        }
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