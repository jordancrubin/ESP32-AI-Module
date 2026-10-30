/*
  LLMClient.cpp - ESP32 AI Module LLM Interface
  Handles HTTP communication with OpenAI-compatible APIs for Chat and TTS.
  
  https://www.youtube.com/@rubin-tech
  2026 Jordan Rubin.
*/
#include "LLMClient.h"
#include <FS.h>
#include <LittleFS.h>
#include "SettingsManager.h"
#include "DisplayManager.h"
#include <lvgl.h>
#include <time.h>

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
                if (doc["data"].is<JsonArray>()) {
                    JsonArray data = doc["data"];
                    for (JsonObject v : data) {
                        const char* id = v["id"] ? v["id"] : v["name"];
                        if (id) {
                            if (result.length() > 0) result += "\n";
                            result += id;
                        }
                    }
                } else if (doc["models"].is<JsonArray>()) {
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

static String extractOpenWebUIBaseUrl(const String& apiUrl) {
    String url = apiUrl;
    int chatIdx = url.indexOf("/chat/completions");
    if (chatIdx != -1) url = url.substring(0, chatIdx);
    if (url.endsWith("/api")) url = url.substring(0, url.length() - 4);
    if (url.endsWith("/v1")) url = url.substring(0, url.length() - 3);
    if (url.endsWith("/")) url = url.substring(0, url.length() - 1);
    return url;
}

static String fetchCalendarContext(const String& baseUrl, const String& apiKey, WiFiManager& netMgr) {
    String eventsUrl = baseUrl + "/api/v1/calendars/events/search?limit=10";
    String resolved = netMgr.resolveHost(eventsUrl);
    if (resolved == "") return "";

    WiFiClient client;
    HTTPClient http;
    client.setTimeout(4000);
    http.setTimeout(4000);

    if (!http.begin(client, resolved)) return "";
    http.addHeader("Authorization", "Bearer " + apiKey);
    int code = http.GET();
    if (code != 200) {
        http.end();
        return "";
    }

    JsonDocument filter;
    filter["items"][0]["title"] = true;
    filter["items"][0]["start_at"] = true;
    filter["items"][0]["all_day"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err || !doc["items"].is<JsonArray>()) return "";

    JsonArray items = doc["items"];
    if (items.size() == 0) return "";

    String result = "[UPCOMING CALENDAR EVENTS:\n";
    for (JsonObject ev : items) {
        const char* title = ev["title"] | "";
        if (strlen(title) == 0) continue;
        uint64_t start_ns = ev["start_at"] | 0ULL;
        time_t sec = (time_t)(start_ns / 1000000000ULL);
        char dateBuf[64] = "";
        if (sec > 0) {
            struct tm timeinfo;
            gmtime_r(&sec, &timeinfo);
            strftime(dateBuf, sizeof(dateBuf), "%B %d, %Y", &timeinfo);
        }
        result += "- ";
        result += title;
        if (strlen(dateBuf) > 0) {
            result += " (";
            result += dateBuf;
            result += ")";
        }
        result += "\n";
    }
    result += "]\nUse the above calendar events to directly and conversationally answer questions about schedule, upcoming events, or plans.";
    return result;
}

static String fetchWebSearchContext(const String& baseUrl, const String& apiKey, const String& query, WiFiManager& netMgr) {
    String searchUrl = baseUrl + "/api/v1/retrieval/process/web/search";
    String resolved = netMgr.resolveHost(searchUrl);
    if (resolved == "") return "";

    WiFiClient client;
    HTTPClient http;
    client.setTimeout(6000);
    http.setTimeout(6000);

    if (!http.begin(client, resolved)) return "";
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + apiKey);

    JsonDocument reqDoc;
    reqDoc["queries"][0] = query;
    String reqBody;
    serializeJson(reqDoc, reqBody);

    int code = http.POST(reqBody);
    if (code != 200) {
        http.end();
        return "";
    }

    JsonDocument filter;
    filter["items"][0]["title"] = true;
    filter["items"][0]["snippet"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err || !doc["items"].is<JsonArray>()) return "";

    JsonArray items = doc["items"];
    if (items.size() == 0) return "";

    String result = "[LIVE WEB SEARCH RESULTS:\n";
    int count = 0;
    for (JsonObject item : items) {
        const char* title = item["title"] | "";
        const char* snippet = item["snippet"] | "";
        if (strlen(snippet) > 0) {
            result += "- ";
            if (strlen(title) > 0) {
                result += title;
                result += ": ";
            }
            result += snippet;
            result += "\n";
            count++;
            if (count >= 3) break;
        }
    }
    result += "]\nUse the above real-time web search results to answer conversationally. Do not cite URLs or use [1] brackets.";
    return (count > 0) ? result : "";
}

static String s_cachedMemoryContext = "";
static unsigned long s_lastMemoryFetch = 0;

static String fetchMemoryContext(const String& baseUrl, const String& apiKey, WiFiManager& netMgr) {
    String memUrl = baseUrl + "/api/v1/memories/";
    String resolved = netMgr.resolveHost(memUrl);
    if (resolved == "") return "";

    WiFiClient client;
    HTTPClient http;
    client.setTimeout(4000);
    http.setTimeout(4000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, resolved)) return "";
    http.addHeader("Authorization", "Bearer " + apiKey);
    int code = http.GET();
    if (code != 200) {
        http.end();
        return "";
    }

    JsonDocument filter;
    filter[0]["content"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err || !doc.is<JsonArray>()) return "";

    JsonArray items = doc.as<JsonArray>();
    if (items.size() == 0) return "";

    String result = "[USER MEMORIES & PERSONAL CONTEXT:\n";
    int count = 0;
    for (JsonObject mem : items) {
        const char* content = mem["content"] | "";
        if (strlen(content) == 0) continue;
        result += "- ";
        result += content;
        result += "\n";
        count++;
        if (count >= 25) break; // Limit to 25 facts to keep context clean
    }
    result += "]\nUse the above user memories and personal facts to answer conversationally.";
    return (count > 0) ? result : "";
}

static String getMemoryContext(const String& baseUrl, const String& apiKey, WiFiManager& netMgr) {
    if (s_cachedMemoryContext.length() > 0 && (millis() - s_lastMemoryFetch < 60000)) {
        return s_cachedMemoryContext;
    }
    String fetched = fetchMemoryContext(baseUrl, apiKey, netMgr);
    if (fetched.length() > 0) {
        s_cachedMemoryContext = fetched;
        s_lastMemoryFetch = millis();
    }
    return s_cachedMemoryContext;
}

static bool postAddMemory(const String& baseUrl, const String& apiKey, const String& memoryContent, WiFiManager& netMgr) {
    if (memoryContent.length() == 0) return false;
    String addUrl = baseUrl + "/api/v1/memories/add";
    String resolved = netMgr.resolveHost(addUrl);
    if (resolved == "") {
        Serial.println("[Memory] Error: Could not resolve URL: " + addUrl);
        return false;
    }

    WiFiClient client;
    HTTPClient http;
    client.setTimeout(8000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, resolved)) {
        Serial.println("[Memory] Error: http.begin failed for: " + resolved);
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + apiKey);

    JsonDocument doc;
    doc["content"] = memoryContent;
    doc["type"] = "user";
    String reqBody;
    serializeJson(doc, reqBody);

    Serial.println("[Memory] Sending memory to Open-WebUI: " + reqBody);
    int code = http.POST(reqBody);
    String resp = "";
    if (code > 0) {
        resp = http.getString();
    }
    http.end();

    if (code == 200 || code == 201) {
        Serial.println("[Memory] Successfully saved memory to Open-WebUI. Response: " + resp);
        s_lastMemoryFetch = 0; // Invalidate cache so next retrieval includes this new memory
        return true;
    } else {
        Serial.printf("[Memory] Failed to add memory. HTTP code: %d, Response: %s\n", code, resp.c_str());
        return false;
    }
}

static String extractMemoryFact(const String& prompt) {
    String lower = prompt;
    lower.toLowerCase();
    lower.trim();

    // 1. Check for suffix: "... to memory", "... to my memory", "... in memory", "... in your memory"
    const char* suffixes[] = {
        " to my memory",
        " to your memory",
        " to memory",
        " in my memory",
        " in your memory",
        " in memory",
        " into my memory",
        " into your memory",
        " into memory"
    };

    for (const char* sfx : suffixes) {
        if (lower.endsWith(sfx)) {
            String sub = prompt.substring(0, prompt.length() - strlen(sfx));
            String subLower = sub;
            subLower.toLowerCase();
            subLower.trim();

            const char* startPfxs[] = {
                "can you please add that ",
                "can you please add ",
                "can you add that ",
                "can you add ",
                "could you please add that ",
                "could you please add ",
                "could you add that ",
                "could you add ",
                "please add that ",
                "please add ",
                "i want you to add that ",
                "i want you to add ",
                "add that ",
                "add ",
                "save that ",
                "save ",
                "store that ",
                "store ",
                "put that ",
                "put ",
                "keep that ",
                "keep "
            };

            for (const char* spfx : startPfxs) {
                if (subLower.startsWith(spfx)) {
                    String fact = sub.substring(strlen(spfx));
                    fact.trim();
                    while (fact.endsWith(".") || fact.endsWith("!") || fact.endsWith("?")) {
                        fact.remove(fact.length() - 1);
                        fact.trim();
                    }
                    if (fact.length() > 0) return fact;
                }
            }

            // Fallback for suffix: if user said "<fact> to memory" without leading "add"
            String fact = sub;
            fact.trim();
            while (fact.endsWith(".") || fact.endsWith("!") || fact.endsWith("?")) {
                fact.remove(fact.length() - 1);
                fact.trim();
            }
            if (fact.length() > 0) return fact;
        }
    }

    // 2. Check for prefixes: "remember ...", "add to memory ...", "save to memory ..."
    const char* prefixes[] = {
        "can you please remember that ",
        "can you please remember this: ",
        "can you please remember this ",
        "can you please remember to ",
        "can you please remember ",
        "can you remember that ",
        "can you remember this: ",
        "can you remember this ",
        "can you remember to ",
        "can you remember ",
        "could you remember that ",
        "could you remember ",
        "i want you to remember that ",
        "i want you to remember ",
        "please remember that ",
        "please remember this: ",
        "please remember this ",
        "please remember to ",
        "please remember ",
        "remember that ",
        "remember this: ",
        "remember this ",
        "remember to ",
        "remember, ",
        "remember ",
        "add to my memory that ",
        "add to my memory: ",
        "add to my memory ",
        "add to your memory that ",
        "add to your memory: ",
        "add to your memory ",
        "add to memory that ",
        "add to memory: ",
        "add to memory ",
        "add this to memory: ",
        "add this to your memory: ",
        "add this to my memory: ",
        "add this to memory ",
        "add this to your memory ",
        "add this to my memory ",
        "save to my memory that ",
        "save to my memory: ",
        "save to my memory ",
        "save to your memory that ",
        "save to your memory: ",
        "save to your memory ",
        "save to memory that ",
        "save to memory: ",
        "save to memory ",
        "save this to memory: ",
        "save this to your memory: ",
        "save this to memory ",
        "save this to your memory ",
        "add a memory that ",
        "add a memory: ",
        "add a memory ",
        "commit to memory that ",
        "commit to memory ",
        "note that ",
        "take note that ",
        "take a note that ",
        "make a note that ",
        "make a note of ",
        "keep in mind that ",
        "dont forget that ",
        "don't forget that "
    };

    for (const char* pfx : prefixes) {
        if (lower.startsWith(pfx)) {
            String fact = prompt.substring(strlen(pfx));
            fact.trim();
            while (fact.endsWith(".") || fact.endsWith("!") || fact.endsWith("?")) {
                fact.remove(fact.length() - 1);
                fact.trim();
            }
            if (fact.length() > 0) return fact;
        }
    }
    return "";
}

bool LLMClient::addMemory(const String& memoryContent, WiFiManager& netMgr) {
    String owBaseUrl = extractOpenWebUIBaseUrl(_apiUrl);
    if (owBaseUrl.length() == 0) {
        Serial.println("[Memory] Cannot add memory: Open-WebUI base URL could not be derived from " + _apiUrl);
        return false;
    }
    return postAddMemory(owBaseUrl, _apiKey, memoryContent, netMgr);
}

String LLMClient::getMemories(WiFiManager& netMgr) {
    String owBaseUrl = extractOpenWebUIBaseUrl(_apiUrl);
    if (owBaseUrl.length() == 0) return "Error: Open-WebUI base URL could not be derived from " + _apiUrl;
    return fetchMemoryContext(owBaseUrl, _apiKey, netMgr);
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

    String owBaseUrl = extractOpenWebUIBaseUrl(_apiUrl);

    // Direct Memory Addition Interception (Instant Open-WebUI memory saving)
    if (settings.enableMemory && owBaseUrl.length() > 0) {
        String memoryFact = extractMemoryFact(prompt);
        if (memoryFact.length() > 0) {
            Serial.println("[Memory] Intercepted memory addition: \"" + memoryFact + "\"");
            display.showStatus("Saving Memory...");
            lv_timer_handler();
            if (postAddMemory(owBaseUrl, _apiKey, memoryFact, netMgr)) {
                String confirmMsg = "I have saved that to your memory: " + memoryFact + ".";
                display.showResponse(confirmMsg);
                lv_timer_handler();
                JsonObject aMsg = _history.add<JsonObject>();
                aMsg["role"] = "assistant";
                aMsg["content"] = confirmMsg;
                return confirmMsg;
            } else {
                String errMsg = "I was unable to save that to your Open-WebUI memory. Please check your API key or permissions.";
                display.showResponse(errMsg);
                lv_timer_handler();
                JsonObject aMsg = _history.add<JsonObject>();
                aMsg["role"] = "assistant";
                aMsg["content"] = errMsg;
                return errMsg;
            }
        }
    }

    String lowerPrompt = prompt;
    lowerPrompt.toLowerCase();
    String contextInjection = "";

    // 1. Calendar Events Pre-injection
    if (settings.enableCalendar && owBaseUrl.length() > 0) {
        bool isCalendarQuery = (lowerPrompt.indexOf("schedule") != -1) ||
                               (lowerPrompt.indexOf("calendar") != -1) ||
                               (lowerPrompt.indexOf("events") != -1) ||
                               (lowerPrompt.indexOf("upcoming") != -1) ||
                               (lowerPrompt.indexOf("agenda") != -1) ||
                               (lowerPrompt.indexOf("appointment") != -1) ||
                               (lowerPrompt.indexOf("meeting") != -1) ||
                               (lowerPrompt.indexOf("plans") != -1) ||
                               (lowerPrompt.indexOf("what am i doing") != -1) ||
                               (lowerPrompt.indexOf("what do i have") != -1);

        if (isCalendarQuery) {
            if (settings.debugMode) Serial.println("Fetching calendar events from OpenWebUI...");
            String calContext = fetchCalendarContext(owBaseUrl, _apiKey, netMgr);
            if (calContext.length() > 0) {
                contextInjection += "\n\n" + calContext;
                if (settings.debugMode) Serial.println("Injected calendar context.");
            }
        }
    }

    // 2. Web Search Pre-injection
    if (settings.enableWebSearch && owBaseUrl.length() > 0) {
        bool useWebSearch = (lowerPrompt.indexOf("search the web") != -1) ||
                            (lowerPrompt.indexOf("do a search") != -1) ||
                            (lowerPrompt.indexOf("check online") != -1) ||
                            (lowerPrompt.indexOf("search online") != -1) ||
                            (lowerPrompt.indexOf("look up online") != -1) ||
                            (lowerPrompt.indexOf("search the internet") != -1) ||
                            (lowerPrompt.indexOf("current weather") != -1) ||
                            (lowerPrompt.indexOf("what is the weather") != -1) ||
                            (lowerPrompt.indexOf("latest news") != -1);

        if (useWebSearch) {
            String searchQuery = prompt;
            String cleanQuery = lowerPrompt;
            cleanQuery.replace("search the web for", "");
            cleanQuery.replace("search the web", "");
            cleanQuery.replace("do a search for", "");
            cleanQuery.replace("do a search on", "");
            cleanQuery.replace("check online for", "");
            cleanQuery.replace("search online for", "");
            cleanQuery.replace("look up online", "");
            cleanQuery.trim();
            if (cleanQuery.length() > 0) searchQuery = cleanQuery;

            if (settings.debugMode) Serial.println("Fetching web search for: " + searchQuery);
            String webContext = fetchWebSearchContext(owBaseUrl, _apiKey, searchQuery, netMgr);
            if (webContext.length() > 0) {
                contextInjection += "\n\n" + webContext;
                if (settings.debugMode) Serial.println("Injected web search context.");
            }
        }
    }

    // 3. User Memories Pre-injection
    if (settings.enableMemory && owBaseUrl.length() > 0) {
        if (settings.debugMode) Serial.println("Fetching user memories from OpenWebUI...");
        String memContext = getMemoryContext(owBaseUrl, _apiKey, netMgr);
        if (memContext.length() > 0) {
            contextInjection += "\n\n" + memContext;
            if (settings.debugMode) Serial.println("Injected user memory context.");
        }
    }

    // Attach pre-injected context to user message for this request
    if (contextInjection.length() > 0) {
        userMsg["content"] = prompt + contextInjection;
    }

    JsonDocument doc;
    doc["model"] = _model;
    doc["stream"] = true;
    doc["messages"] = _history;
    
    // OpenWebUI Workaround: Prevent NoneType crash by supplying empty chat_id
    doc["metadata"]["chat_id"] = "";
    doc["chat_id"] = "";
    doc["session_id"] = "";

    // Attach OpenWebUI Knowledge Collection (RAG) if configured
    if (strlen(settings.knowledgeId) > 0) {
        JsonArray filesArr = doc["files"].to<JsonArray>();
        JsonObject fObj = filesArr.add<JsonObject>();
        fObj["type"] = "collection";
        fObj["id"] = settings.knowledgeId;
        doc["metadata"]["files"] = doc["files"];
        if (settings.debugMode) Serial.printf("Attached Knowledge Collection ID: %s\n", settings.knowledgeId);
    }

    // OpenWebUI Feature Flags
    if (owBaseUrl.length() > 0) {
        if (settings.enableWebSearch) {
            doc["features"]["web_search"] = true;
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
            String toolCallName = "";
            String toolCallArgs = "";

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
                                        // 1. Check for API error
                                        if (!chunkDoc["error"].isNull()) {
                                            const char* errM = chunkDoc["error"]["message"] | chunkDoc["error"]["detail"] | "API error";
                                            full_response = "Error: " + String(errM);
                                            break;
                                        }

                                        // 2. Extract content
                                        const char* content = chunkDoc["choices"][0]["delta"]["content"];
                                        if (!content) content = chunkDoc["message"]["content"];
                                        if (content) full_response += content;

                                        // 3. Catch streamed tool_calls (fallback if model invokes a tool)
                                        JsonArray tc = chunkDoc["choices"][0]["delta"]["tool_calls"];
                                        if (!tc.isNull() && tc.size() > 0) {
                                            JsonObject fn = tc[0]["function"];
                                            const char* fnName = fn["name"];
                                            if (fnName && strlen(fnName) > 0) toolCallName += fnName;
                                            const char* fnArgs = fn["arguments"];
                                            if (fnArgs && strlen(fnArgs) > 0) toolCallArgs += fnArgs;
                                        }
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

            // Fallback for tool_calls if no content was streamed
            if (full_response.length() == 0 && toolCallName.length() > 0) {
                Serial.printf("[ToolCall] Captured tool call in stream: %s args: %s\n", toolCallName.c_str(), toolCallArgs.c_str());
                if (toolCallName.indexOf("memory") != -1 && owBaseUrl.length() > 0) {
                    JsonDocument argDoc;
                    deserializeJson(argDoc, toolCallArgs);
                    const char* memFact = argDoc["content"] | argDoc["memory"] | argDoc["text"] | "";
                    if (strlen(memFact) > 0) {
                        postAddMemory(owBaseUrl, _apiKey, String(memFact), netMgr);
                    }
                    full_response = "I have saved that to your memory.";
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
                // Restore clean prompt in history without injected context to save context window
                userMsg["content"] = prompt;
                JsonObject assistantMsg = _history.add<JsonObject>();
                assistantMsg["role"] = "assistant";
                assistantMsg["content"] = result;
            }
            else {
                Serial.println("[LLM] Stream ended with 0 content bytes. Raw stream buffer: " + streamBuffer);
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
        String fn = String(filename);
        if (fn.endsWith(".wav") || strcmp(settings.ttsFormat, "wav") == 0) {
            doc["response_format"] = "wav";
        } else {
            doc["response_format"] = "mp3";
        }
        doc["speed"] = ttsSpeed;

        String requestBody;
        serializeJson(doc, requestBody);

        if (settings.debugMode) {
            Serial.println("TTS Request Body:");
            Serial.println(requestBody);
        }

        int httpCode = http.POST(requestBody);
        if (httpCode == 200) {
            if (LittleFS.exists(filename)) LittleFS.remove(filename);
            File file = LittleFS.open(filename, "w");
            if (!file) {
                Serial.printf("TTS Error: Failed to open %s for writing (LittleFS Full?)\n", filename);
                http.end();
                return false;
            }

            int bytesWritten = http.writeToStream(&file);
            file.close();
            http.end();
            
            flushAecBuffer(); // Clear any microphone ringbuffer overflows that occurred while blocked by the network
            
            if (bytesWritten <= 0) {
                Serial.printf("TTS Error: Wrote 0 bytes to %s (Out of space?)\n", filename);
                return false;
            }

            if (settings.debugMode) Serial.printf("TTS Download Complete: %d bytes\n", bytesWritten);
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
    s_lastMemoryFetch = 0; // Force refresh of user memories on new session
    
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