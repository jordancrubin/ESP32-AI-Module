/*
  main.cpp - ESP32 AI Module Firmware
  Main application entry point. Orchestrates Display, WiFi, LLM, and Audio subsystems.
  Functionality requires WiFi connection and valid API credentials.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "Config.h"
#include "DisplayManager.h"
#include "WiFiManager.h"
#include "LLMClient.h"
#include "SpeechManager.h"
#include "SpeakerManager.h"
#include "SettingsManager.h"
#include "CommandProcessor.h"

DisplayManager display;
WiFiManager network("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD");
LLMClient llm("http://your-api-endpoint/api/chat/completions", "your_api_key_here", "llama3.2:3b");
SpeechManager speech;
SpeakerManager speaker;
SettingsManager settings;
WebServer server(80);
Preferences adminPrefs;
String adminPassword = "";
String ttsVoice = "alloy";
String voiceOptions = "alloy";
String modelOptions = "";
float wakeThreshold = 0.8;

String inputBuffer = "";
bool isSpeaking = false;
bool isWebServerActive = false;
bool forceConfig = false;

unsigned long lastWeatherUpdate = 0;
bool shouldConnectWiFi = false;
String pendingSSID = "";
String pendingPass = "";
bool isProcessing = false;
static bool stopRequested = false;
extern bool g_isSubMenuActive;

// External functions from SpeechManager.cpp
extern void setAecDebug(bool enable);
extern void setAecDelay(int delay);
extern void setAecGain(int gain);
extern void setAecPhase(bool invert);
extern void setInputBalance(int balance);
extern void getAudioLevels(int* l, int* r);
extern void setAecAttenuation(int atten);
extern void setAecCutoff(int cutoff);
extern void setInterruptMode(bool mode);
extern void setAecBypass(bool bypass);
extern void renderBSOD();
extern void renderGuruMeditation();
extern void forceClockScreen();
extern void showAecTuningUI();
extern void updateAecTuningUI(int percent, const char* msg);
extern void showHelpScreen();

// Forward declarations
void testAEC();
void tuneAEC();
void onSetupMode(bool enabled);

// Chime Configuration
const char* CHIME_FILENAME = "/chime.mp3";

// Onboard RGB LED
Adafruit_NeoPixel pixels(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

void onVolumeChange(int value) {
    settings.volume = value;
    if (settings.debugMode) Serial.printf("Volume: %d (Tone disabled)\n", settings.volume);
    speaker.setVolume(settings.volume);
    settings.save();
}

void onBalanceChange(int value) {
    settings.inputBalance = value;
    setInputBalance(value);
    settings.save();
}

void onAdminConfig(String pass) {
    if (pass.length() > 0) {
        adminPassword = pass;
        adminPrefs.putString("pass", adminPassword);
    }
}

void onVoiceChange(String voice) {
    if (voice.startsWith("MODEL:")) {
        String newModel = voice.substring(6);
        // Extract ID if format is "Name (ID)"
        int openParen = newModel.lastIndexOf('(');
        int closeParen = newModel.lastIndexOf(')');
        if (openParen != -1 && closeParen != -1 && closeParen > openParen) {
            newModel = newModel.substring(openParen + 1, closeParen);
        }
        strlcpy(settings.llmModel, newModel.c_str(), sizeof(settings.llmModel));
        settings.save();
        llm.setConfig(settings.apiUrl, settings.apiKey, settings.llmModel);
        if (settings.debugMode) Serial.println("Model changed to: " + String(settings.llmModel));
    } else if (voice == "TALK_ACTION") {
        if (isProcessing) {
            stopRequested = true;
            speaker.stop();
            isSpeaking = false;
            display.showStatus("Ready");
            setLedColor(0, 0, 0); // LED Off
            return;
        }

        isProcessing = true;
        stopRequested = false;
        bool triggeredEasterEgg = false;

        while (!stopRequested) {
            // 1. Record
            display.showStatus("Listening...");
            setLedColor(0, 255, 0); // Green while recording
            lv_timer_handler(); // Force UI update
            
            size_t wavSize = 0;
            // Record for 15 seconds (adjust as needed)
            uint8_t* wavData = speech.record(15000, &wavSize, settings.silenceThreshold);
            setLedColor(255, 0, 0); // Red after recording (Processing)
            
            if (wavData && wavSize > 0) {
                // 2. Transcribe
                display.showStatus("Transcribing...");
                lv_timer_handler();
                String text = llm.transcribeAudio(wavData, wavSize, network);
                free(wavData); // Free PSRAM immediately
                
                if (text.startsWith("Error")) {
                    if (settings.debugMode) Serial.println("Transcription Failed: " + text);
                    display.showStatus(text.c_str());
                    break; // Stop loop on error
                } else {
                    if (settings.debugMode) Serial.println("Transcription: " + text);
                    
                    // Process local commands
                    CommandResult cmd = CommandProcessor::processCommand(text);
                    bool handledLocally = cmd.handled;
                    String localResponse = cmd.response;
                    bool runAecTest = cmd.runAecTest;
                    bool systemReboot = cmd.systemReboot;
                    bool showBSOD = cmd.showBSOD;
                    bool showGuruMeditation = cmd.showGuruMeditation;
                    bool calibrateTouch = cmd.calibrateTouch;
                    bool enterConfigMode = cmd.enterConfigMode;
                    bool tuneAEC_flag = cmd.tuneAEC;
                    bool showHelp = cmd.showHelp;
                    if (showBSOD || showGuruMeditation || showHelp) triggeredEasterEgg = true;

                    String answer;
                    if (handledLocally) {
                        if (!showHelp) {
                            display.showStatus("Local Command...");
                            lv_timer_handler();
                        }
                        answer = localResponse;
                        if (settings.debugMode) Serial.println("Local Action: " + answer);
                    } else {
                        // 3. Send to LLM
                        display.showStatus("Thinking...");
                        lv_timer_handler();
                        
                        answer = llm.sendPrompt(text, network);
                        if (settings.debugMode) Serial.println("Answer: " + answer);
                    }
                    
                    // 4. TTS
                    if (showHelp) {
                        showHelpScreen();
                    } else {
                        display.showStatus("Speaking...");
                    }
                    // Ensure any previous TTS file is removed to free space before downloading
                    if (LittleFS.exists("/speech.mp3")) LittleFS.remove("/speech.mp3");
                    
                    if (llm.downloadTTS(answer, network, "/speech.mp3", ttsVoice)) {
                        speaker.playSpeechFromFile("/speech.mp3");
                        isSpeaking = true;
                        lv_timer_handler(); // Update UI once to show "Speaking"
                        
                        bool interrupted = false;
                        if (settings.enableInterrupt) setInterruptMode(true);
                        unsigned long playbackStart = millis();

                        // Wait for playback to finish, or poll for interruptions
                        while (speaker.isRunning()) {
                            if (settings.enableInterrupt) {
                                bool triggered = speech.detectWakeWord(wakeThreshold);
                                if (triggered && (millis() - playbackStart > 2000)) {
                                    if (settings.debugMode) Serial.println("Playback interrupted by user!");
                                    speaker.stop();
                                    speaker.playSpeechFromFile(CHIME_FILENAME);
                                    while(speaker.isRunning()) delay(30);
                                    interrupted = true;
                                    break;
                                }
                                delay(5); // Fast loop to drain AEC buffer safely
                            } else {
                                delay(50);
                            }
                        }
                        
                        if (settings.enableInterrupt) setInterruptMode(false);
                        isSpeaking = false;
                        
                        if (interrupted) {
                            continue; // Skip the rest, loop back to "Listening..."
                        }

                        if (runAecTest) {
                            display.showStatus("Running AEC Test...");
                            lv_timer_handler();
                            testAEC();
                        }

                        if (tuneAEC_flag) {
                            display.showStatus("Auto-Tuning AEC...");
                            lv_timer_handler();
                            tuneAEC();
                        }
                        
                        if (systemReboot) {
                            display.showStatus("Rebooting...");
                            delay(1000);
                            ESP.restart();
                        }
                        
                        if (showBSOD) {
                            renderBSOD();
                            delay(5000); // Show BSOD for 5 seconds
                            break;       // End the conversation loop
                        }
                        
                        if (showGuruMeditation) {
                            renderGuruMeditation();
                            delay(5000); // Show Guru Meditation for 5 seconds
                            break;       // End the conversation loop
                        }
                        
                        if (calibrateTouch) {
                            display.calibrateTouch(settings.calibration);
                            settings.save();
                            display.showMainUI(ttsVoice, settings.volume, voiceOptions);
                            break;       // End the conversation loop
                        }
                        
                        if (enterConfigMode) {
                            onSetupMode(true);
                            display.showWebConfig(WiFi.localIP().toString(), "aiesp.local");
                            break;       // End the conversation loop
                        }
                    } else {
                        display.showStatus("TTS Failed");
                        break;
                    }
                }
            } else {
                display.showStatus("No Speech");
                delay(1500);
                
                // Flush history on silence/abort
                llm.clearHistory();
                if (settings.debugMode) Serial.println("Conversation ended (Silence). History cleared.");
                break;
            }
        }
        
        isProcessing = false;
        if (triggeredEasterEgg) {
            forceClockScreen(); // Jump straight to the clock
        } else if (!isWebServerActive) {
            display.showStatus("Ready"); // Normal recovery
        }
        setLedColor(0, 0, 0); // LED Off
    } else {
        ttsVoice = voice;
        adminPrefs.putString("voice", ttsVoice);
        if (settings.debugMode) Serial.println("Voice changed to: " + ttsVoice);
    }
}

void onSetupMode(bool enabled) {
    if (enabled) {
        server.begin();
        isWebServerActive = true;
        if (settings.debugMode) Serial.println("Web Server Started (Setup Mode)");
    } else {
        server.stop();
        isWebServerActive = false;
        if (settings.debugMode) Serial.println("Web Server Stopped");
    }
}

// Helper to URL encode the input string
String urlEncode(String str) {
    String encodedString = "";
    char c;
    char code0;
    char code1;
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (isalnum(c)) {
            encodedString += c;
        } else {
            code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
            c = (c >> 4) & 0xf;
            code0 = c + '0';
            if (c > 9) code0 = c - 10 + 'A';
            encodedString += '%';
            encodedString += code0;
            encodedString += code1;
        }
    }
    return encodedString;
}

bool getWeather() {
    if (strlen(settings.openWeatherKey) == 0) return false;
    if (!network.isConnected()) return false;

    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/weather?q=" + 
                 urlEncode(String(settings.weatherLocation)) + "&appid=" + 
                 String(settings.openWeatherKey) + "&units=metric";
    
    http.begin(url);
    int httpCode = http.GET();
    bool success = false;
    if (httpCode == 200) {
        String payload = http.getString();
        JsonDocument doc;
        deserializeJson(doc, payload);
        float temp = doc["main"]["temp"];
        const char* desc = doc["weather"][0]["main"];
        char tempBuf[16];
        snprintf(tempBuf, sizeof(tempBuf), "%.0fC", temp);
        display.updateWeather(tempBuf, desc);
        success = true;
    } else {
        if (settings.debugMode) {
            Serial.printf("Weather Update Failed. HTTP Code: %d\n", httpCode);
            Serial.println("Response: " + http.getString());
        }
    }
    http.end();
    return success;
}

void handleWebRoot() {
    if (settings.debugMode) Serial.println("Web Request: /");
    if (!server.authenticate("admin", adminPassword.c_str())) {
        server.sendHeader("WWW-Authenticate", "Basic realm=\"Login Required\"");
        server.send(401, "text/html", "Unauthorized\n");
        return;
    }

    String html = "<html><head><title>AI ESP32 Config</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;padding:20px;} input, select{width:100%;padding:10px;margin:5px 0;} .btn{background-color:#4CAF50;color:white;border:none;cursor:pointer;} .btn-blue{background-color:#008CBA;color:white;border:none;cursor:pointer;padding:10px;width:100%;margin:5px 0;}</style></head><body>";
    html += "<h2>Configuration</h2>";
    html += "<div style='background:#e9ecef; padding:10px; margin-bottom:15px; border-radius:5px;'><b>System Update:</b> Navigate to <a href='/update'>/update</a> to upload new Firmware or Filesystem images.</div>";
    html += "<form action='/save' method='POST'>";
    html += "API Key: <input type='text' name='apiKey' value='" + String(settings.apiKey) + "'><br>";
    html += "API URL: <input type='text' name='apiUrl' value='" + String(settings.apiUrl) + "'><br>";
    
    html += "TTS Provider: <select name='ttsProvider'>";
    html += "<option value='0'" + String(settings.ttsProvider == 0 ? " selected" : "") + ">OpenWebUI (Kokoro)</option>";
    html += "<option value='1'" + String(settings.ttsProvider == 1 ? " selected" : "") + ">Direct</option>";
    html += "</select><br>";
    html += "Direct TTS URL: <input type='text' name='ttsUrl' value='" + String(settings.ttsUrl) + "' placeholder='e.g., http://host:port'><br>";
    html += "<small>Used when TTS Provider is 'Direct'. Must be full base URL.</small><br><br>";
    
    html += "System Prompt:<br>";
    html += "<textarea name='systemPrompt' rows='3' style='width:100%'>" + String(settings.systemPrompt) + "</textarea><br>";

    html += "Timezone: <select name='timezone'>";
    String tzs[] = {"UTC0", "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0", "MST7", "GMT0BST,M3.5.0/1,M10.5.0", "CET-1CEST,M3.5.0,M10.5.0/3", "JST-9", "CST-8", "AEST-10AEDT,M10.1.0,M4.1.0/3"};
    String names[] = {"UTC", "US Eastern", "US Central", "US Mountain", "US Pacific", "US Arizona", "London", "Paris/Berlin", "Tokyo", "Shanghai", "Sydney"};
    
    for (int i = 0; i < 11; i++) {
        html += "<option value='" + tzs[i] + "'";
        if (String(settings.timeZone) == tzs[i]) html += " selected";
        html += ">" + names[i] + "</option>";
    }
    // Allow custom entry if not in list
    if (strlen(settings.timeZone) > 0 && html.indexOf("selected") == -1) {
         html += "<option value='" + String(settings.timeZone) + "' selected>Custom (" + String(settings.timeZone) + ")</option>";
    }
    html += "</select><br>";

    html += "Clock Color: <select name='clockColor'>";
    String colors[] = {"red", "green", "white"};
    String colorNames[] = {"Red", "Green", "White"};
    for (int i = 0; i < 3; i++) {
        html += "<option value='" + colors[i] + "'";
        if (String(settings.clockColor) == colors[i]) html += " selected";
        html += ">" + colorNames[i] + "</option>";
    }
    html += "</select><br>";

    html += "Wake Sensitivity (0.4-0.9): <input type='number' name='wakeThreshold' value='" + String(wakeThreshold) + "' step='0.05' min='0.4' max='0.9'><br>";

    html += "Silence Threshold (300-2000): <input type='number' name='silenceThreshold' value='" + String(settings.silenceThreshold) + "' step='50' min='300' max='2000'><br>";

    html += "Screen Brightness: <input type='range' name='brightness' min='10' max='255' value='" + String(settings.brightness) + "' oninput='this.nextElementSibling.value = this.value'> <output>" + String(settings.brightness) + "</output><br>";
    html += "<small>Adjusts display backlight (10-255)</small><br>";

    html += "Microphone Mode: <select name='micMode'>";
    String micModes[] = {"Stereo (Beamforming)", "Left Channel Only", "Right Channel Only"};
    for (int i = 0; i < 3; i++) {
        html += "<option value='" + String(i) + "'";
        if (settings.micMode == i) html += " selected";
        html += ">" + micModes[i] + "</option>";
    }
    html += "</select><br>";

    html += "AEC Target Delay: <input type='number' name='aecDelay' value='" + String(settings.aecDelay) + "' step='10' min='160' max='1600'><br>";
    html += "<small>Echo alignment in samples. You can Auto-Tune this via voice command.</small><br>";

    html += "AEC Attenuation (Divisor): <input type='number' name='aecAttenuation' value='" + String(settings.aecAttenuation) + "' min='1' max='32'><br>";
    html += "<small>Reduces mic sensitivity during AI playback (1 = 0%, 2 = 50%, 4 = 75%, 8 = 87%).</small><br>";

    html += "AEC Barge-in Cutoff: <input type='number' name='aecCutoff' value='" + String(settings.aecCutoff) + "' min='0' max='32767'><br>";
    html += "<small>Minimum raw volume required to interrupt the AI. 0 disables the volume gate.</small><br>";

    html += "<h3>AI Features</h3>";
    html += "Enable Web Search: <input type='checkbox' name='webSearch' value='1'" + String(settings.enableWebSearch ? " checked" : "") + "><br>";
    html += "<small>Allows supported models to search the internet for real-time information.</small><br>";
    html += "Enable Memory: <input type='checkbox' name='memory' value='1'" + String(settings.enableMemory ? " checked" : "") + "><br>";
    html += "<small>Allows the AI to remember user details across sessions.</small><br>";
    html += "Enable Voice Interrupt: <input type='checkbox' name='interrupt' value='1'" + String(settings.enableInterrupt ? " checked" : "") + "><br>";
    html += "<small>Allows you to interrupt the AI by speaking over it.</small><br>";
    html += "Knowledge ID: <input type='text' name='knowledgeId' value='" + String(settings.knowledgeId) + "' placeholder='e.g., collection_id or document_id'><br>";
    html += "<small>OpenWebUI Collection/File ID to enable RAG/Knowledge features.</small><br>";

    html += "<h3>Debug</h3>";
    html += "Enable Debug Logging: <input type='checkbox' name='debugMode' value='1'" + String(settings.debugMode ? " checked" : "") + "><br>";
    html += "<small>Serial Baud: 115200</small><br>";

    html += "<h3>Weather (OpenWeatherMap)</h3>";
    html += "API Key: <input type='text' name='owKey' value='" + String(settings.openWeatherKey) + "' placeholder='Leave empty to disable'><br>";
    html += "Location (City,CC): <input type='text' name='owLoc' value='" + String(settings.weatherLocation) + "'><br>";

    html += "<input type='submit' value='Save & Verify' class='btn'>";
    html += "</form>";

    html += "<hr><h3>Edge Impulse</h3>";
    html += "<p>Use this mode to collect raw microphone data via USB using the <b>edge-impulse-data-forwarder</b> CLI.</p>";
    html += "<form action='/ei_mode' method='POST'>";
    html += "<input type='submit' value='Reboot to Data Collection Mode' class='btn-blue' style='background-color:#ff9800;'>";
    html += "</form>";

    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleWebSave() {
    if (!server.authenticate("admin", adminPassword.c_str())) {
        return server.requestAuthentication();
    }
    
    String newApiKey = server.hasArg("apiKey") ? server.arg("apiKey") : settings.apiKey;
    String newApiUrl = server.hasArg("apiUrl") ? server.arg("apiUrl") : settings.apiUrl;
    String newTz = server.hasArg("timezone") ? server.arg("timezone") : settings.timeZone;
    String newColor = server.hasArg("clockColor") ? server.arg("clockColor") : settings.clockColor;
    String newOwKey = server.hasArg("owKey") ? server.arg("owKey") : settings.openWeatherKey;
    String newOwLoc = server.hasArg("owLoc") ? server.arg("owLoc") : settings.weatherLocation;
    String newSysPrompt = server.hasArg("systemPrompt") ? server.arg("systemPrompt") : settings.systemPrompt;
    String newKnowledgeId = server.hasArg("knowledgeId") ? server.arg("knowledgeId") : settings.knowledgeId;

    // TTS Settings
    int newTtsProvider = server.hasArg("ttsProvider") ? server.arg("ttsProvider").toInt() : settings.ttsProvider;
    String newTtsUrl = server.hasArg("ttsUrl") ? server.arg("ttsUrl") : settings.ttsUrl;

    // If Direct is chosen but URL is empty, revert to OpenWebUI
    if (newTtsProvider == 1 && newTtsUrl.length() == 0) {
        newTtsProvider = 0;
    }
    settings.ttsProvider = newTtsProvider;

    if (server.hasArg("wakeThreshold")) {
        float val = server.arg("wakeThreshold").toFloat();
        if (val >= 0.1 && val <= 1.0) {
            wakeThreshold = val;
            adminPrefs.putFloat("wake_thresh", wakeThreshold);
        }
    }

    if (server.hasArg("silenceThreshold")) {
        int val = server.arg("silenceThreshold").toInt();
        if (val >= 300 && val <= 2000) {
            settings.silenceThreshold = val;
        }
    }

    if (server.hasArg("aecDelay")) {
        int val = server.arg("aecDelay").toInt();
        if (val >= 160 && val <= 1600) {
            settings.aecDelay = val;
            setAecDelay(val);
        }
    }

    if (server.hasArg("aecAttenuation")) {
        int val = server.arg("aecAttenuation").toInt();
        if (val >= 1 && val <= 32) {
            settings.aecAttenuation = val;
            setAecAttenuation(val);
        }
    }

    if (server.hasArg("aecCutoff")) {
        int val = server.arg("aecCutoff").toInt();
        if (val >= 0 && val <= 32767) {
            settings.aecCutoff = val;
            setAecCutoff(val);
        }
    }

    if (server.hasArg("brightness")) {
        int val = server.arg("brightness").toInt();
        if (val < 10) val = 10; // Prevent setting brightness to 0
        if (val > 255) val = 255;
        settings.brightness = val;
        display.setBacklight(settings.brightness);
    }

    if (server.hasArg("micMode")) {
        settings.micMode = server.arg("micMode").toInt();
    }

    settings.debugMode = server.hasArg("debugMode");
    settings.enableWebSearch = server.hasArg("webSearch");
    settings.enableMemory = server.hasArg("memory");
    settings.enableInterrupt = server.hasArg("interrupt");

    // Save to NVRAM immediately
    strlcpy(settings.apiKey, newApiKey.c_str(), sizeof(settings.apiKey));
    strlcpy(settings.apiUrl, newApiUrl.c_str(), sizeof(settings.apiUrl));
    strlcpy(settings.timeZone, newTz.c_str(), sizeof(settings.timeZone));
    strlcpy(settings.clockColor, newColor.c_str(), sizeof(settings.clockColor));
    strlcpy(settings.openWeatherKey, newOwKey.c_str(), sizeof(settings.openWeatherKey));
    strlcpy(settings.weatherLocation, newOwLoc.c_str(), sizeof(settings.weatherLocation));
    strlcpy(settings.systemPrompt, newSysPrompt.c_str(), sizeof(settings.systemPrompt));
    strlcpy(settings.ttsUrl, newTtsUrl.c_str(), sizeof(settings.ttsUrl));
    strlcpy(settings.knowledgeId, newKnowledgeId.c_str(), sizeof(settings.knowledgeId));
    settings.save();
    
    forceConfig = false;

    // Apply config and test connection
    llm.setConfig(settings.apiUrl, settings.apiKey, settings.llmModel);
    setenv("TZ", settings.timeZone, 1);
    tzset();
    llm.setSystemPrompt(settings.systemPrompt);

    // Refresh weather immediately if configured
    if (newOwKey.length() > 0) getWeather();

    String models = llm.getModels(network);

    if (models.startsWith("Error")) {
        String html = "<html><body><h1>Saved (Verification Failed)</h1><p>Settings saved, but API check failed: " + models + "</p><a href='/'>Go Back</a></body></html>";
        server.send(200, "text/html", html);
    } else {
        server.send(200, "text/html", "<html><body><h1>Saved & Verified!</h1><p>Connection successful.</p><a href='/'>Back</a></body></html>");
        if (settings.debugMode) Serial.println("Settings updated and verified via Web Interface");
    }
}

void onWiFiConfig(String ssid, String pass) {
    pendingSSID = ssid;
    pendingPass = pass;
    shouldConnectWiFi = true;
}

void updateVoiceList() {
    String jsonResponse = llm.getVoices(network);
    if (jsonResponse.startsWith("Error")) {
        if (settings.debugMode) Serial.println("Failed to fetch voices: " + jsonResponse);
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonResponse);

    if (error) {
        if (settings.debugMode) Serial.print(F("deserializeJson() failed: "));
        if (settings.debugMode) Serial.println(error.f_str());
        return;
    }

    if (!doc["voices"].is<JsonArray>()) {
        if (settings.debugMode) Serial.println(F("JSON response missing 'voices' key"));
        return;
    }

    JsonArray voices = doc["voices"];
    String newOptions = "";
    newOptions.reserve(1024);

    for (JsonVariant v : voices) {
        const char* voiceName = nullptr;
        if (v.is<JsonObject>()) {
            voiceName = v["id"];
        } else {
            voiceName = v.as<const char*>();
        }

        if (voiceName && strlen(voiceName) > 0) {
            char firstChar = voiceName[0];
            if (firstChar == 'a' || firstChar == 'b' || firstChar == 'd') {
                if (newOptions.length() > 0) {
                    newOptions += '\n';
                }
                newOptions += voiceName;
            }
        }
    }

    if (newOptions.length() > 0) {
        voiceOptions = newOptions;
        // Combine with modelOptions if available
        if (modelOptions.length() > 0) {
            voiceOptions += "||" + modelOptions;
        }
        // Update UI with new options, keeping current voice and volume
        display.showMainUI(ttsVoice, settings.volume, voiceOptions);
        if (settings.debugMode) Serial.println("Voice list updated in dropdown.");
    } else {
        if (settings.debugMode) Serial.println("No matching voices found.");
    }
}

void testAEC() {
    if (!settings.debugMode) return;

    Serial.println("Downloading TTS for AEC Test...");
    String testPhrase = "this is a test from the ai esp32 to see how speaker cancellation is functioning. Like and Subscribe to retro tech and electronics as well as classic wrench today. Beep Beep!";
    const char* ttsFile = "/aec_test_source.mp3";
    
    if (!network.isConnected()) {
        Serial.println("WiFi not connected. Cannot download TTS.");
        return;
    }

    if (!llm.downloadTTS(testPhrase, network, ttsFile, ttsVoice)) {
        Serial.println("TTS Download failed. Aborting test.");
        return;
    }
    
    // --- Test 1: With AEC ---
    Serial.println("\n--- Test 1: With AEC (Cancellation Enabled) ---");
    Serial.println("Playing TTS & Recording (10s)...");
    setAecDebug(true); // Enable debug stats
    speaker.playSpeechFromFile(ttsFile);
    
    // Record with 0 threshold to ensure capture
    size_t wavSize1 = 0;
    uint8_t* wavData1 = speech.record(10000, &wavSize1, 0);
    
    speaker.stop();
    setAecDebug(false); // Disable debug stats
    
    if (wavData1 && wavSize1 > 0) {
        File f = LittleFS.open("/aec_with.wav", "w");
        if (f) {
            f.write(wavData1, wavSize1);
            f.close();
            Serial.println("Saved /aec_with.wav");
        }
        free(wavData1);
    } else {
        Serial.println("Recording 1 failed.");
    }

    delay(2000);

    // --- Test 2: Without AEC (Raw) ---
    Serial.println("\n--- Test 2: Without AEC (Raw Input) ---");
    setAecBypass(true); // Force SpeechManager to bypass the Speex Ringbuffer
    
    Serial.println("Playing TTS & Recording (10s)...");
    speaker.playSpeechFromFile(ttsFile);
    
    size_t wavSize2 = 0;
    uint8_t* wavData2 = speech.record(10000, &wavSize2, 0);
    
    speaker.stop();
    setAecBypass(false); // Restore normal AEC operation

    if (wavData2 && wavSize2 > 0) {
        File f = LittleFS.open("/aec_raw.wav", "w");
        if (f) {
            f.write(wavData2, wavSize2);
            f.close();
            Serial.println("Saved /aec_raw.wav");
        }
        free(wavData2);
    } else {
        Serial.println("Recording 2 failed.");
    }

    // --- Playback ---
    Serial.println("\n--- Playback: With AEC ---");
    speaker.playSpeechFromFile("/aec_with.wav");
    while(speaker.isRunning()) delay(100);
    
    delay(1000);
    
    Serial.println("\n--- Playback: Without AEC ---");
    speaker.playSpeechFromFile("/aec_raw.wav");
    while(speaker.isRunning()) delay(100);
    
    Serial.println("\nAEC Test Complete.");
}

void tuneAEC() {
    Serial.println("\n--- Starting AEC Auto-Tuning ---");
    showAecTuningUI(); // Load graphical UI
    
    Serial.println("Downloading TTS for AEC Tuning...");
    // Repetitive diagnostic phrase ensures active audio during the 2.5s cut-off window
    String testPhrase = "Testing alignment. Testing alignment. Testing alignment. Testing alignment. Testing alignment.";
    const char* ttsFile = "/aec_tune.mp3";
    
    if (!network.isConnected()) { Serial.println("WiFi not connected."); return; }
    if (!llm.downloadTTS(testPhrase, network, ttsFile, ttsVoice)) { Serial.println("TTS Download failed."); return; }
    if (!network.isConnected()) { 
        Serial.println("WiFi not connected."); 
        display.showStatus("Tuning Failed\nNo WiFi");
        return; 
    }
    if (!llm.downloadTTS(testPhrase, network, ttsFile, ttsVoice)) { 
        Serial.println("TTS Download failed."); 
        display.showStatus("Tuning Failed\nTTS Error");
        return; 
    }

    // Test range: 10ms to 70ms (160 to 1120 samples at 16kHz)
    int testDelays[] = {160, 320, 480, 640, 800, 960, 1120}; 
    long avgScores[7] = {0};
    int bestDelay = 640;
    long bestScore = 99999999; // Lower amplitude is better

    Serial.println("Please remain completely silent for about a minute...");
    
    int totalSteps = 9 * 3 + 5 + 1; // 7 coarse + 2 fine + 5 attenuation + 1 cutoff
    int currentStep = 0;

    // Define the test sequence as a reusable lambda function
    auto runDelayTest = [&](int d) -> long {
        setAecDelay(d);
        
        long totalScore = 0;
        int validRuns = 0;
        Serial.printf("\nTesting Delay: %d samples (~%d ms)\n", d, d/16);

        for (int run = 0; run < 3; run++) {
            Serial.printf("  Run %d/3... ", run + 1);
            
            int percent = (currentStep * 100) / totalSteps;
            String uiMsg = "Testing Delay: " + String(d/16) + "ms (" + String(run+1) + "/3)";
            updateAecTuningUI(percent, uiMsg.c_str());
            
            speaker.playSpeechFromFile(ttsFile);
            
            // Discard first 1 second of audio to let the AEC filter adapt to the room
            size_t discardSize = 0;
            uint8_t* discardData = speech.record(1000, &discardSize, 0);
            if (discardData) free(discardData);

            // Record 1.5 seconds for actual measurement
            size_t wavSize = 0;
            uint8_t* wavData = speech.record(1500, &wavSize, 0);
            
            speaker.stop();
            
            long score = 99999999;
            if (wavData && wavSize > 44) {
                long sum = 0;
                int16_t* samples = (int16_t*)(wavData + 44);
                int sampleCount = (wavSize - 44) / 2;
                for(int j=0; j<sampleCount; j++) sum += abs(samples[j]);
                score = sum / sampleCount; // Calculate average absolute amplitude
                free(wavData);
                
                totalScore += score;
                validRuns++;
                Serial.printf("Score: %ld\n", score);
            } else {
                Serial.println("Failed to record.");
            }

            currentStep++;
            
            percent = (currentStep * 100) / totalSteps;
            uiMsg = "Testing Delay: " + String(d/16) + "ms (" + String(run+1) + "/3)\nWaiting for room echo to settle...";
            updateAecTuningUI(percent, uiMsg.c_str());

            delay(2000); // 2 second pause between tests to let room echo settle
        }
        
        if (validRuns > 0) {
            return totalScore / validRuns;
        } else {
            return 99999999;
        }
    };

    // 1. Coarse Tuning Pass
    for (int i = 0; i < 7; i++) {
        avgScores[i] = runDelayTest(testDelays[i]);
    }

    Serial.println("\n--- Coarse Tuning Results ---");
    for (int i = 0; i < 7; i++) {
        Serial.printf("Delay %d samples (~%d ms) -> Average Score: %ld\n", testDelays[i], testDelays[i]/16, avgScores[i]);
        if (avgScores[i] < bestScore) {
            bestScore = avgScores[i];
            bestDelay = testDelays[i];
        }
    }

    // 2. Fine Tuning Pass (+/- 80 samples)
    int fineDelays[] = {bestDelay - 80, bestDelay + 80};
    if (fineDelays[0] < 0) fineDelays[0] = 0; // Prevent negative delays
    
    long fineScores[2] = {0};
    Serial.printf("\nBest Coarse Delay: %d samples. Starting Fine-Tuning (+/- 80 samples)...\n", bestDelay);

    for (int i = 0; i < 2; i++) {
        fineScores[i] = runDelayTest(fineDelays[i]);
    }

    Serial.println("\n--- Fine Tuning Results ---");
    for (int i = 0; i < 2; i++) {
        Serial.printf("Delay %d samples (~%d ms) -> Average Score: %ld\n", fineDelays[i], fineDelays[i]/16, fineScores[i]);
        if (fineScores[i] < bestScore) {
            bestScore = fineScores[i];
            bestDelay = fineDelays[i];
        }
    }
    
    // 3. Attenuation Tuning Pass
    Serial.println("\n--- Attenuation Tuning Pass (Target Max Amp < 150) ---");
    int testAttenuations[] = {2, 4, 8, 16, 32}; // Removed 1 to guarantee baseline attenuation
    int bestAtten = 16; // Default to safest high value
    
    for (int i = 0; i < 5; i++) {
        int att = testAttenuations[i];
        Serial.printf("Testing Attenuation: %d%%... ", 100 - (100 / att));
        
        int percent = (currentStep * 100) / totalSteps;
        String uiMsg = "Testing Attenuation: " + String(100 - (100 / att)) + "%";
        updateAecTuningUI(percent, uiMsg.c_str());
        
        speaker.playSpeechFromFile(ttsFile);
        
        size_t discardSize = 0;
        uint8_t* discardData = speech.record(1000, &discardSize, 0);
        if (discardData) free(discardData);

        size_t wavSize = 0;
        uint8_t* wavData = speech.record(1500, &wavSize, 0);
        
        speaker.stop();
        
        long max_amp = 0;
        if (wavData && wavSize > 44) {
            int16_t* samples = (int16_t*)(wavData + 44);
            int sampleCount = (wavSize - 44) / 2;
            for(int j=0; j<sampleCount; j++) {
                long val = abs(samples[j]) / att; // Simulate the mathematical attenuation
                if (val > max_amp) max_amp = val;
            }
            free(wavData);
            Serial.printf("Max Amp: %ld\n", max_amp);
        } else {
            Serial.println("Failed to record.");
        }

        currentStep++;
        delay(2000);
        
        if (max_amp > 0 && max_amp < 150) {
            bestAtten = att;
            Serial.printf("Found optimal attenuation: %d%%\n", 100 - (100 / bestAtten));
            currentStep += (4 - i); // Fast-forward progress bar for skipped steps
            break;
        }
    }

    // 4. Cutoff Tuning Pass
    Serial.println("\n--- Cutoff Tuning Pass ---");
    Serial.printf("Testing final max amplitude with Attenuation %d%%... ", 100 - (100 / bestAtten));
    
    int percent = (currentStep * 100) / totalSteps;
    updateAecTuningUI(percent, "Measuring Amplitude Cutoff...");
    
    speaker.playSpeechFromFile(ttsFile);
    size_t discardSize = 0;
    uint8_t* discardData = speech.record(1000, &discardSize, 0);
    if (discardData) free(discardData);

    size_t wavSize = 0;
    uint8_t* wavData = speech.record(1500, &wavSize, 0);
    speaker.stop();
    
    long final_max_amp = 0;
    if (wavData && wavSize > 44) {
        int16_t* samples = (int16_t*)(wavData + 44);
        int sampleCount = (wavSize - 44) / 2;
        for(int j=0; j<sampleCount; j++) {
            long val = abs(samples[j]) / bestAtten;
            if (val > final_max_amp) final_max_amp = val;
        }
        free(wavData);
        Serial.printf("Final Max Amp: %ld\n", final_max_amp);
    }
    
    currentStep++;
    int bestCutoff = final_max_amp + 500; // Add generous safety margin to prevent self-interruptions
    if (bestCutoff < 1000) bestCutoff = 1000; // Hard minimum floor so AI never triggers itself

    Serial.println("----------------------------------");
    Serial.printf("Ultimate Best AEC Delay: %d samples\n", bestDelay);
    Serial.printf("Ultimate Best Attenuation: %d%%\n", 100 - (100 / bestAtten));
    Serial.printf("Ultimate Best Cutoff: %d\n", bestCutoff);
    
    settings.aecDelay = bestDelay;
    settings.aecAttenuation = bestAtten;
    settings.aecCutoff = bestCutoff;
    settings.save();
    
    setAecDelay(bestDelay); // Lock it in
    setAecAttenuation(bestAtten);
    setAecCutoff(bestCutoff);
    Serial.println("Saved to NVRAM and applied!");
    
    // Display Final Results
    String resultMsg = "Tuning Complete!\nBest Delay: " + String(bestDelay) + " samples\nAtten: " + String(100 - (100 / bestAtten)) + "% | Cutoff: " + String(bestCutoff);
    updateAecTuningUI(100, resultMsg.c_str());
    delay(5000);
    delay(30000); // Wait 30 seconds so the user can read the results before it clears
    display.showMainUI(ttsVoice, settings.volume, voiceOptions); // Return to default UI
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      inputBuffer.trim();
      if (inputBuffer.length() > 0) {
        if (inputBuffer == "/help") {
          Serial.println("\n--- Available Commands ---");
          Serial.println("/settings       - Show current configuration");
          Serial.println("/calibrate      - Start touch calibration");
          Serial.println("/reset_cal      - Reset touch calibration");
          Serial.println("/say <text>     - Speak text immediately");
          Serial.println("/new            - Clear conversation history");
          Serial.println("/test_mic       - Record 5s clip to test mic (Debug only)");
          Serial.println("/test_aec       - Run AEC diagnostics (Debug only)");
          Serial.println("/tune_aec       - Auto-tune AEC delay alignment");
          Serial.println("/debug_aec      - Toggle AEC debug stats");
          Serial.println("/aec_delay <n>  - Set AEC delay samples");
          Serial.println("/aec_gain <n>   - Set AEC gain multiplier");
          Serial.println("/aec_invert     - Toggle AEC phase inversion");
          Serial.println("<text>          - Send prompt to AI");
          Serial.println("--------------------------\n");
        } else if (inputBuffer == "/settings") {
          Serial.println("\n--- Current Settings ---");
          Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
          Serial.printf("CPU Freq:   %d MHz\n", getCpuFrequencyMhz());
          Serial.printf("RSSI:       %d dBm\n", network.getSignalStrength());
          Serial.printf("WiFi SSID:  %s\n", settings.wifiSSID);
          Serial.printf("WiFi Pass:  ******\n");
          Serial.printf("API URL:    %s\n", settings.apiUrl);
          Serial.printf("API Key:    ******\n");
          Serial.printf("LLM Model:  %s\n", settings.llmModel);
          Serial.printf("Volume:     %d / 21\n", settings.volume);
          Serial.printf("Timezone:   %s\n", settings.timeZone);
          Serial.printf("Clock Color:%s\n", settings.clockColor);
          Serial.printf("Brightness: %d / 255\n", settings.brightness);
          Serial.printf("Wake Thresh: %.2f\n", wakeThreshold);
          Serial.printf("Silence Thr: %d\n", settings.silenceThreshold);
          Serial.printf("Touch Cal:  %s (%d,%d to %d,%d)\n", 
            settings.calibration.isValid ? "Valid" : "Invalid",
            settings.calibration.xMin, settings.calibration.yMin,
            settings.calibration.xMax, settings.calibration.yMax);
          Serial.printf("Mic Mode:   %d\n", settings.micMode);
          Serial.printf("Input Bal:  %d\n", settings.inputBalance);
          Serial.printf("AEC Delay:  %d samples\n", settings.aecDelay);
          Serial.printf("AEC Atten:  %d%%\n", 100 - (100 / settings.aecAttenuation));
          Serial.printf("AEC Cutoff: %d\n", settings.aecCutoff);
          Serial.printf("Debug Mode: %s\n", settings.debugMode ? "ON" : "OFF");
          Serial.printf("Web Search: %s\n", settings.enableWebSearch ? "ON" : "OFF");
          Serial.printf("Memory:     %s\n", settings.enableMemory ? "ON" : "OFF");
          Serial.printf("Interrupt:  %s\n", settings.enableInterrupt ? "ON" : "OFF");
          Serial.printf("Knowledge:  %s\n", settings.knowledgeId);
          Serial.printf("Sys Prompt: %s\n", settings.systemPrompt);
          Serial.printf("TTS Provider: %s\n", settings.ttsProvider == 0 ? "OpenWebUI" : "Direct");
          Serial.printf("TTS URL:    %s\n", settings.ttsUrl);
          Serial.printf("Weather Key:******\n");
          Serial.printf("Weather Loc:%s\n", settings.weatherLocation);
          Serial.println("Note: Redacted values can be viewed from the configuration web page.");
          Serial.println("------------------------\n");
        } else if (inputBuffer == "/debug_aec") {
          static bool d = false;
          d = !d;
          setAecDebug(d);
        } else if (inputBuffer.startsWith("/aec_delay ")) {
          int d = inputBuffer.substring(11).toInt();
          if (d > 0) {
              settings.aecDelay = d;
              settings.save();
              setAecDelay(d);
          }
        } else if (inputBuffer.startsWith("/aec_gain ")) {
          int g = inputBuffer.substring(10).toInt();
          if (g > 0) setAecGain(g);
        } else if (inputBuffer == "/aec_invert") {
          static bool inv = false;
          inv = !inv;
          setAecPhase(inv);
        } else if (inputBuffer == "/calibrate") {
          Serial.println("Starting manual touch calibration...");
          display.calibrateTouch(settings.calibration);
          settings.save();
          Serial.println("Calibration complete.");
          display.showMainUI();
        } else if (inputBuffer == "/reset_cal") {
          Serial.println("Resetting touch calibration...");
          settings.calibration = {0, 0, 0, 0, false};
          settings.save();
          Serial.println("Calibration reset. Restart the device to recalibrate.");
        } else if (inputBuffer == "/test_aec") {
          testAEC();
        } else if (inputBuffer == "/tune_aec") {
          tuneAEC();
        } else if (inputBuffer.startsWith("/say ")) {
          String textToSay = inputBuffer.substring(5);
          textToSay.trim();
          if (textToSay.length() > 0) {
            if (settings.debugMode) Serial.println("Direct TTS: " + textToSay);
            speaker.stop();
            isSpeaking = false;
            display.showMainUI(ttsVoice, settings.volume, voiceOptions);
            display.showStatus("Direct TTS...");
            
            // Download TTS to file, then play
            if (llm.downloadTTS(textToSay, network, "/speech.mp3", ttsVoice)) {
                speaker.playSpeechFromFile("/speech.mp3");
                isSpeaking = true;
                lv_timer_handler();
                if (settings.enableInterrupt) setInterruptMode(true);
                unsigned long playbackStart = millis();
                while(speaker.isRunning()) {
                    if (settings.enableInterrupt) {
                        bool triggered = speech.detectWakeWord(wakeThreshold);
                        if (triggered && (millis() - playbackStart > 2000)) {
                            speaker.stop();
                            speaker.playSpeechFromFile(CHIME_FILENAME);
                            while(speaker.isRunning()) delay(30);
                            break;
                        }
                        delay(5);
                    } else {
                        delay(50);
                    }
                }
                if (settings.enableInterrupt) setInterruptMode(false);
                isSpeaking = false;
            } else {
                display.showResponse("TTS Failed");
            }
            display.showStatus("Ready");
          }
        } else if (inputBuffer == "/test_mic" && settings.debugMode) {
          Serial.println("Testing Microphone (5s recording)...");
          display.showMainUI(ttsVoice, settings.volume, voiceOptions);
          display.showStatus("Recording (5s)...");
          speaker.stop(); // Stop any playback
          
          size_t wavSize = 0;
          uint8_t* wavData = speech.record(5000, &wavSize, settings.silenceThreshold);
          
          if (wavData && wavSize > 0) {
              Serial.printf("Recording complete. Size: %d bytes\n", wavSize);
              
              // Calculate average amplitude for debug
              long sum = 0;
              int16_t* samples = (int16_t*)(wavData + 44); // Skip WAV header
              int sampleCount = (wavSize - 44) / 2;
              for(int i=0; i<sampleCount; i++) sum += abs(samples[i]);
              int avg = sampleCount > 0 ? sum / sampleCount : 0;
              Serial.printf("Average Amplitude: %d\n", avg);

              display.showStatus("Saving...");
              File file = LittleFS.open("/mic_test.wav", "w");
              if (file) {
                  file.write(wavData, wavSize);
                  file.close();
                  Serial.println("Saved to /mic_test.wav");
                  display.showStatus("Playing back...");
                  speaker.playSpeechFromFile("/mic_test.wav");
                  isSpeaking = true;
                  lv_timer_handler();
                  while(speaker.isRunning()) delay(50);
                  isSpeaking = false;
              } else {
                  Serial.println("Failed to open file for writing");
                  display.showStatus("Save Failed");
              }
              free(wavData);
          } else {
              Serial.println("Recording failed");
              display.showStatus("Record Failed");
          }
          display.showStatus("Ready");
        } else if (inputBuffer == "/test_mic" && !settings.debugMode) {
            Serial.println("Debug mode disabled. Enable debug to run mic test.");
        } else if (inputBuffer == "/new") {
          llm.clearHistory();
          if (settings.debugMode) Serial.println("Conversation history cleared.");
        } else {
          speaker.stop(); // Stop any current playback before processing new request
          isSpeaking = false;
          isProcessing = true;
          display.showMainUI(ttsVoice, settings.volume, voiceOptions);
          display.showThinking(true);
          display.showStatus("Thinking...");
          // Force UI update before the blocking API call
          lv_timer_handler(); 
          
          // NOTE: This call is blocking. UI will be unresponsive until it returns.
          String answer = llm.sendPrompt(inputBuffer, network);
          display.showThinking(false);
          
          if (settings.debugMode) Serial.println("Answer:");
          if (settings.debugMode) Serial.println(answer);

          display.showResponse("Speaking...");
          
          // Download TTS to file, then play
          if (llm.downloadTTS(answer, network, "/speech.mp3", ttsVoice)) {
              speaker.playSpeechFromFile("/speech.mp3");
              isSpeaking = true;
              lv_timer_handler();
              if (settings.enableInterrupt) setInterruptMode(true);
              unsigned long playbackStart = millis();
              while(speaker.isRunning()) {
                  if (settings.enableInterrupt) {
                      bool triggered = speech.detectWakeWord(wakeThreshold);
                      if (triggered && (millis() - playbackStart > 2000)) {
                          speaker.stop();
                          speaker.playSpeechFromFile(CHIME_FILENAME);
                          while(speaker.isRunning()) delay(30);
                          break;
                      }
                      delay(5);
                  } else {
                      delay(50);
                  }
              }
              if (settings.enableInterrupt) setInterruptMode(false);
              isSpeaking = false;
          } else {
              display.showResponse("TTS Failed");
          }
          isProcessing = false;
          display.showStatus("Ready");
        }
      }
      inputBuffer = "";
    } else {
      inputBuffer += c;
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(0, INPUT_PULLUP); // Initialize BOOT button (GPIO 0)
  setCpuFrequencyMhz(240); // Lock CPU at 240MHz for maximum performance
  // Initialize LED
  pixels.begin();
  pixels.setBrightness(20); // Low brightness
  pixels.setPixelColor(0, pixels.Color(0, 0, 255)); // Blue = Boot Window
  pixels.show();

  unsigned long start = millis();
    while (!Serial && (millis() - start < 3000));

  Serial.printf("System Starting at %d MHz...\n", getCpuFrequencyMhz());
  // Initialize File System
  if (!LittleFS.begin(true)) {
      Serial.println("LittleFS Mount Failed");
  }
  // Load Settings
  settings.begin();
  adminPrefs.begin("admin", false);

  Serial.println("Waiting 3 seconds... Press BOOT button now for Factory Reset.");
  bool factoryReset = false;
  start = millis();
  while (millis() - start < 3000) {
      if (digitalRead(0) == LOW) {
          factoryReset = true;
          pixels.setPixelColor(0, pixels.Color(255, 0, 0)); // Red = Reset triggered
          pixels.show();
          break;
      }
      delay(10);
  }

  pixels.clear();
  pixels.show();

  // Check for Factory Reset
  if (factoryReset) {
      Serial.println("BOOT button held: Performing Factory Reset...");
      settings.wifiSSID[0] = '\0';
      settings.wifiPass[0] = '\0';
      settings.apiKey[0] = '\0';
      settings.calibration.isValid = false;
      settings.save();
      adminPrefs.clear();
      Serial.println("WiFi, API Key, Calibration, and Admin Password cleared.");
      // Wait for button release to avoid accidental double-triggering
      while(digitalRead(0) == LOW) delay(10);
  }

  // Apply loaded settings
  network.setCredentials(settings.wifiSSID, settings.wifiPass);
  // Migration: Fix API URL suffix in NVRAM if it matches the old format
  if (String(settings.apiUrl).endsWith("/v1/chat/completions")) {
      String tempUrl = settings.apiUrl;
      tempUrl.replace("/v1/chat/completions", "/api/chat/completions");
      strlcpy(settings.apiUrl, tempUrl.c_str(), sizeof(settings.apiUrl));
      settings.save();
      if (settings.debugMode) Serial.println("Migrated API URL to /api/chat/completions");
  }

  llm.setConfig(settings.apiUrl, settings.apiKey, settings.llmModel);
  // Speaker volume will be applied after begin()

  // Initialize Speaker early so volume buttons work immediately
  speaker.begin();
  speaker.setVolume(settings.volume);

  // Check for chime file
  if (!LittleFS.exists(CHIME_FILENAME)) {
      if (settings.debugMode) Serial.println("Warning: /chime.mp3 not found. Please upload it to LittleFS.");
  }

  display.begin(settings.calibration);
  display.setVolumeCallback(onVolumeChange);
  display.setWiFiConfigCallback(onWiFiConfig);
  display.setAdminConfigCallback(onAdminConfig);
  display.setVoiceCallback(onVoiceChange);
  display.setSetupModeCallback(onSetupMode);
  display.setBalanceCallback(onBalanceChange);
  
  // Apply saved brightness
  display.setBacklight(settings.brightness);

  // Check for touch calibration status
  if (settings.calibration.isValid) {
      if (settings.debugMode) Serial.println("Touch calibration found. Loading saved values...");
  } else {
      if (settings.debugMode) Serial.println("Touch calibration NOT found or reset. Starting calibration utility...");
      display.calibrateTouch(settings.calibration);
      settings.save(); // Save the newly generated calibration to NVS
      if (settings.debugMode) Serial.println("Touch calibration completed and saved.");
  }

  display.showBootLogo(); // Show the logo immediately
  delay(3000); // Wait 3 seconds to see logo
  
  display.showStatus("AI Interactor Version " FIRMWARE_VERSION);
  display.showStatus("Retrotech&Electronics");
  display.showStatus("Powered by Edge Impulse");
  display.showStatus("UI by LVGL");

    if (adminPrefs.getBool("ei_mode", false)) {
        adminPrefs.putBool("ei_mode", false); // Clear immediately for next boot
        display.showStatus("Edge Impulse Mode\nReady for CLI\nReboot to Exit");
        
        Serial.println("\n==================================================");
        Serial.println("Edge Impulse Data Forwarder Mode active.");
        Serial.println("IMPORTANT: You must CLOSE this Serial Monitor now!");
        Serial.println("Then, open a fresh terminal and run:");
        Serial.println("  edge-impulse-data-forwarder");
        Serial.println("==================================================\n");
        delay(3000); // Give user a moment to read before blasting raw data
        
        speech.begin();
        extern void runEdgeImpulseForwarder();
        runEdgeImpulseForwarder(); 
        // Never returns. User must physically reset the board to exit.
    }

  // Admin Password Check
  adminPassword = adminPrefs.getString("pass", "");
  ttsVoice = adminPrefs.getString("voice", "alloy");
  wakeThreshold = adminPrefs.getFloat("wake_thresh", 0.8);
  setInputBalance(settings.inputBalance);
  if (adminPassword == "") {
      display.showAdminConfig();
      while (adminPassword == "") {
          lv_timer_handler();
          handleSerialCommands();
          delay(5);
      }
      display.showStatus("Admin Password Saved");
  }

  // WiFi Connection Logic
  if (strlen(settings.wifiSSID) == 0 || strcmp(settings.wifiSSID, "YOUR_WIFI_SSID") == 0 || 
      strlen(settings.wifiPass) == 0 || strcmp(settings.wifiPass, "YOUR_WIFI_PASSWORD") == 0) {
      display.showWiFiConfig();
      while (!network.isConnected()) {
          lv_timer_handler();
          handleSerialCommands();
          delay(5);
          
          if (shouldConnectWiFi) {
              shouldConnectWiFi = false;
              for (int i = 1; i <= 2; i++) {
                  display.showStatus(("Connecting (" + String(i) + "/2)...").c_str());
                  lv_timer_handler();
                  network.setCredentials(pendingSSID, pendingPass);
                  network.connect();
                  if (network.isConnected()) {
                      strlcpy(settings.wifiSSID, pendingSSID.c_str(), sizeof(settings.wifiSSID));
                      strlcpy(settings.wifiPass, pendingPass.c_str(), sizeof(settings.wifiPass));
                      break;
                  }
              }
              if (!network.isConnected()) {
                  display.showStatus("Fail to connect");
                  lv_timer_handler();
                  delay(2000);
                  display.showWiFiConfig();
              }
          }
      }
      if (settings.debugMode) Serial.println("WiFi Config Success: Saving to NVRAM.");
      settings.save();
      
      // Configure Time (NTP)
      configTime(0, 0, "pool.ntp.org");
      setenv("TZ", settings.timeZone, 1);
      display.showStatus("Time Configured|OK");
      tzset();
  } else {
      int attempts = 0;
      while (attempts < 5) {
          display.showStatus(("Connecting to WiFi (" + String(attempts + 1) + "/5)...").c_str());
          network.connect();
          if (network.isConnected()) break;
          display.showStatus("Connection Failed|FAIL");
          attempts++;
      }
      
      if (network.isConnected()) {
          display.showStatus("WiFi Connected|OK");
          display.showStatus("mDNS Started|OK");
          delay(1500); // Allow network stack to stabilize
          // Configure Time (NTP)
          configTime(0, 0, "pool.ntp.org");
          setenv("TZ", settings.timeZone, 1);
          tzset();
          display.showStatus("Time Configured|OK");

          // Initial Weather Check
          if (strlen(settings.openWeatherKey) > 0) {
              display.showStatus("Checking Weather...");
              if (getWeather()) display.showStatus("Weather Updated|OK");
              else display.showStatus("Weather Error|FAIL");
              lastWeatherUpdate = millis();
          }
      }

      if (!network.isConnected()) {
          display.showStatus("WiFi Failed|FAIL");
          display.showWiFiError("WiFi Connection Failed");
          display.showWiFiError("WiFi Connection Failed after 5 attempts.");
          while (!network.isConnected()) {
              lv_timer_handler();
              handleSerialCommands();
              delay(5);
              
              if (shouldConnectWiFi) {
                  shouldConnectWiFi = false;
                  for (int i = 1; i <= 2; i++) {
                      display.showStatus(("Connecting (" + String(i) + "/2)...").c_str());
                      lv_timer_handler();
                      network.setCredentials(pendingSSID, pendingPass);
                      network.connect();
                      if (network.isConnected()) {
                          strlcpy(settings.wifiSSID, pendingSSID.c_str(), sizeof(settings.wifiSSID));
                          strlcpy(settings.wifiPass, pendingPass.c_str(), sizeof(settings.wifiPass));
                          break;
                      }
                  }
                  if (!network.isConnected()) {
                      display.showStatus("Fail to connect");
                      lv_timer_handler();
                      delay(2000);
                      display.showWiFiConfig();
                  }
              }
          }
          if (settings.debugMode) Serial.println("WiFi Recovery Success: Saving to NVRAM.");
          settings.save();
          
          // Configure Time (NTP)
          configTime(0, 0, "pool.ntp.org");
          setenv("TZ", settings.timeZone, 1);
          tzset();
      }
  }

  // Start Web Server
  server.on("/", handleWebRoot);
  server.on("/save", HTTP_POST, handleWebSave);
  server.onNotFound([]() {
      server.send(404, "text/plain", "Not Found");
  });

  server.on("/ei_mode", HTTP_POST, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      adminPrefs.putBool("ei_mode", true);
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", "Rebooting to Edge Impulse Mode... The next reboot will return to normal operation.");
      delay(1000);
      ESP.restart();
  });

  // OTA Update Endpoints
  server.on("/update", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      server.sendHeader("Connection", "close");
      String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
      html += "<style>body{font-family:sans-serif;padding:20px;} input[type=submit]{background-color:#4CAF50;color:white;border:none;cursor:pointer;padding:10px;margin-top:10px;} a{color:#008CBA;text-decoration:none;}</style></head>";
      html += "<body><h2>System Update</h2>";
      html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
      html += "Firmware: <input type='file' name='firmware' accept='.bin'><br><br>";
      html += "Filesystem: <input type='file' name='filesystem' accept='.bin'><br><br>";
      html += "<input type='submit' value='Upload & Update'></form>";
      html += "<br><a href='/'>&larr; Back to Configuration</a></body></html>";
      server.send(200, "text/html", html);
  });

  server.on("/update", HTTP_POST, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "UPDATE FAILED" : "UPDATE SUCCESS. Device is rebooting...");
      delay(1000);
      ESP.restart();
  }, []() {
      HTTPUpload& upload = server.upload();
      
      // If a file input is left blank, skip it entirely
      if (upload.filename.length() == 0) return;

      if (upload.status == UPLOAD_FILE_START) {
          if (settings.debugMode) Serial.printf("OTA Update Started: %s (Type: %s)\n", upload.filename.c_str(), upload.name.c_str());
          
          int command = (upload.name == "filesystem") ? U_SPIFFS : U_FLASH;
          display.showStatus((command == U_SPIFFS) ? "Updating File System..." : "Updating Firmware...");
          
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.isRunning()) {
              if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
          }
      } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.isRunning()) {
              if (Update.end(true)) {
                  if (settings.debugMode) Serial.printf("OTA Update Success: %u bytes\n", upload.totalSize);
              } else { Update.printError(Serial); }
          }
      }
  });

  server.begin();
  isWebServerActive = true;
  if (settings.debugMode) {
      Serial.println("Web Server started at http://aiesp.local or http://" + WiFi.localIP().toString());
      Serial.println("OTA Update URL: http://aiesp.local/update");
  }

  // Initialize Speech Recognition (I2S)
  speech.begin();
  setAecDelay(settings.aecDelay); // Apply the saved tuning
  setAecAttenuation(settings.aecAttenuation);
  setAecCutoff(settings.aecCutoff);
  display.showStatus("Speech Init|OK");

  // API Configuration Loop (Web Based)
  unsigned long lastLog = 0;
  int wifiRetries = 0;

  while (true) {
      // If API Key/URL is missing, default, or verification failed (forceConfig)
      if (forceConfig || strlen(settings.apiKey) == 0 || strcmp(settings.apiKey, "your_api_key_here") == 0 ||
          strlen(settings.apiUrl) == 0 || strcmp(settings.apiUrl, "http://your-api-endpoint/api/chat/completions") == 0) {
          
          display.showWebConfig(WiFi.localIP().toString(), "aiesp.local");
          
          while (forceConfig || strlen(settings.apiKey) == 0 || strcmp(settings.apiKey, "your_api_key_here") == 0 ||
                 strlen(settings.apiUrl) == 0 || strcmp(settings.apiUrl, "http://your-api-endpoint/api/chat/completions") == 0) {
              lv_timer_handler();
              handleSerialCommands();
              server.handleClient();
              delay(1); 
              
              if (millis() - lastLog > 2000) {
                  if (settings.debugMode) Serial.println("Web Config Loop running...");
                  lastLog = millis();
              }
          }
      }

      // 2. Verify API Connection (1 attempt)
      bool apiVerified = false;
      for (int i = 0; i < 1; i++) {
          display.showStatus("Verifying API...");
          lv_timer_handler();
          server.handleClient(); // Keep web server alive during verification
          
          display.showStatus("Fetching Models...");
          String models = llm.getModels(network);
          if (!models.startsWith("Error")) {
              if (settings.debugMode) Serial.println("API Verified: " + models);
              display.showStatus("API Verified|OK");
              modelOptions = models;
              delay(2000);
              apiVerified = true;
              break;
          }
          display.showStatus("API Check Failed|FAIL");
          if (settings.debugMode) Serial.println("API Check Failed: " + models);
          delay(1000);
      }

      if (apiVerified) {
          Serial.println("API Config Success: Saving to NVRAM.");
          settings.save();
          updateVoiceList();
          break;
      } else {
          display.showStatus("API Connection Failed|FAIL");
          
          if (wifiRetries < 2) {
              if (settings.debugMode) Serial.println("API Check Failed. Restarting WiFi...");
              display.showStatus("Restarting WiFi...|FAIL");
              network.connect();
              wifiRetries++;
              continue;
          }
          
          delay(2000);
          forceConfig = true; // Force return to Web Config screen without wiping data
      }
  }

  // Set a system prompt using PSRAM allocation
  llm.setSystemPrompt(settings.systemPrompt);
  
  // Final UI Load
  display.showMainUI(ttsVoice, settings.volume, voiceOptions);
  display.showStatus("Ready");
  
  if (settings.debugMode) Serial.println("Boot complete. Type prompt in Serial.");
  Serial.println("Type /help for commands");
  
  // Stop web server after boot configuration is complete to save cycles
  server.stop();
  isWebServerActive = false;
}

void loop() {
  // Handle LVGL GUI - Skip updates during playback to prioritize audio bus bandwidth
  if (!isSpeaking) {
    lv_timer_handler();
    
    // Update VU Meter if active
    int l, r;
    getAudioLevels(&l, &r);
    display.updateAudioVUMeter(l, r);
  }

  // Hourly Weather Update
  if (strlen(settings.openWeatherKey) > 0 && network.isConnected()) {
      if (millis() - lastWeatherUpdate > 3600000) { // 1 hour
          getWeather();
          lastWeatherUpdate = millis();
      }
  }
  
  // Check for Wake Word if not already speaking, in web config mode, or in a sub-menu
  if (!isSpeaking && !isWebServerActive && !g_isSubMenuActive) {
      if (speech.detectWakeWord(wakeThreshold)) {
          if (settings.debugMode) Serial.println("Wake Word Detected!");
          speaker.playSpeechFromFile(CHIME_FILENAME);
          
          // Wait for chime to finish playing before proceeding to record
          // Wait for any remaining chime to finish playing before proceeding to record
          while(speaker.isRunning()) {
              delay(30);
          }
          
          onVoiceChange("TALK_ACTION");
      }
  }

  // Keep loop responsive
  delay(5);

  // Check if speaking finished
  if (isSpeaking && !speaker.isRunning()) {
      isSpeaking = false;
      String waitMsg = "Ready\nRSSI: " + String(network.getSignalStrength()) + " dBm";
      display.showStatus(waitMsg.c_str());
  }
  handleSerialCommands();
  if (isWebServerActive) {
      server.handleClient();
  }
}