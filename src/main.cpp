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
static bool isProcessing = false;
static bool stopRequested = false;

// External functions from SpeechManager.cpp
extern void setAecDebug(bool enable);
extern void setAecDelay(int delay);
extern void setAecGain(int gain);
extern void setAecPhase(bool invert);
extern void setInputBalance(int balance);
extern void getAudioLevels(int* l, int* r);

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
    Serial.printf("Volume: %d (Tone disabled)\n", settings.volume);
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
        Serial.println("Model changed to: " + String(settings.llmModel));
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
                    Serial.println("Transcription Failed: " + text);
                    display.showStatus(text.c_str());
                    break; // Stop loop on error
                } else {
                    Serial.println("Transcription: " + text);
                    
                    // 3. Send to LLM
                    display.showStatus("Thinking...");
                    lv_timer_handler();
                    
                    String answer = llm.sendPrompt(text, network);
                    Serial.println("Answer: " + answer);
                    
                    // 4. TTS
                    display.showStatus("Speaking...");
                    // Ensure any previous TTS file is removed to free space before downloading
                    if (LittleFS.exists("/speech.mp3")) LittleFS.remove("/speech.mp3");
                    
                    if (llm.downloadTTS(answer, network, "/speech.mp3", ttsVoice)) {
                        speaker.playSpeechFromFile("/speech.mp3");
                        isSpeaking = true;
                        lv_timer_handler(); // Update UI once to show "Speaking"
                        
                        // Wait for playback to finish (Blocking UI to prevent audio glitches)
                        while (speaker.isRunning()) {
                            delay(50);
                        }
                        isSpeaking = false;
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
                Serial.println("Conversation ended (Silence). History cleared.");
                break;
            }
        }
        
        isProcessing = false;
        display.showStatus("Ready");
        setLedColor(0, 0, 0); // LED Off
    } else {
        ttsVoice = voice;
        adminPrefs.putString("voice", ttsVoice);
        Serial.println("Voice changed to: " + ttsVoice);
    }
}

void onSetupMode(bool enabled) {
    if (enabled) {
        server.begin();
        isWebServerActive = true;
        Serial.println("Web Server Started (Setup Mode)");
    } else {
        server.stop();
        isWebServerActive = false;
        Serial.println("Web Server Stopped");
    }
}

bool getWeather() {
    if (strlen(settings.openWeatherKey) == 0) return false;
    if (!network.isConnected()) return false;

    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/weather?q=" + 
                 String(settings.weatherLocation) + "&appid=" + 
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
    }
    http.end();
    return success;
}

void handleWebRoot() {
    Serial.println("Web Request: /");
    if (!server.authenticate("admin", adminPassword.c_str())) {
        server.sendHeader("WWW-Authenticate", "Basic realm=\"Login Required\"");
        server.send(401, "text/html", "Unauthorized\n");
        return;
    }

    String html = "<html><head><title>AI ESP32 Config</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;padding:20px;} input, select{width:100%;padding:10px;margin:5px 0;} .btn{background-color:#4CAF50;color:white;border:none;cursor:pointer;} .btn-blue{background-color:#008CBA;color:white;border:none;cursor:pointer;padding:10px;width:100%;margin:5px 0;}</style></head><body>";
    html += "<h2>Configuration</h2>";
    html += "<form action='/save' method='POST'>";
    html += "API Key: <input type='text' name='apiKey' value='" + String(settings.apiKey) + "'><br>";
    html += "API URL: <input type='text' name='apiUrl' value='" + String(settings.apiUrl) + "'><br>";
    
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

    html += "Microphone Mode: <select name='micMode'>";
    String micModes[] = {"Stereo (Beamforming)", "Left Channel Only", "Right Channel Only"};
    for (int i = 0; i < 3; i++) {
        html += "<option value='" + String(i) + "'";
        if (settings.micMode == i) html += " selected";
        html += ">" + micModes[i] + "</option>";
    }
    html += "</select><br>";

    html += "<h3>Debug</h3>";
    html += "Enable Debug Logging: <input type='checkbox' name='debugMode' value='1'" + String(settings.debugMode ? " checked" : "") + "><br>";
    html += "<small>Serial Baud: 115200</small><br>";

    html += "<h3>Weather (OpenWeatherMap)</h3>";
    html += "API Key: <input type='text' name='owKey' value='" + String(settings.openWeatherKey) + "' placeholder='Leave empty to disable'><br>";
    html += "Location (City,CC): <input type='text' name='owLoc' value='" + String(settings.weatherLocation) + "'><br>";

    html += "<input type='submit' value='Save & Verify' class='btn'>";
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

    if (server.hasArg("micMode")) {
        settings.micMode = server.arg("micMode").toInt();
    }

    settings.debugMode = server.hasArg("debugMode");

    // Save to NVRAM immediately
    strlcpy(settings.apiKey, newApiKey.c_str(), sizeof(settings.apiKey));
    strlcpy(settings.apiUrl, newApiUrl.c_str(), sizeof(settings.apiUrl));
    strlcpy(settings.timeZone, newTz.c_str(), sizeof(settings.timeZone));
    strlcpy(settings.clockColor, newColor.c_str(), sizeof(settings.clockColor));
    strlcpy(settings.openWeatherKey, newOwKey.c_str(), sizeof(settings.openWeatherKey));
    strlcpy(settings.weatherLocation, newOwLoc.c_str(), sizeof(settings.weatherLocation));
    strlcpy(settings.systemPrompt, newSysPrompt.c_str(), sizeof(settings.systemPrompt));
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
        Serial.println("Settings updated and verified via Web Interface");
    }
}

void onWiFiConfig(String ssid, String pass) {
    pendingSSID = ssid;
    pendingPass = pass;
    shouldConnectWiFi = true;
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

void updateVoiceList() {
    String jsonResponse = llm.getVoices(network);
    if (jsonResponse.startsWith("Error")) {
        Serial.println("Failed to fetch voices: " + jsonResponse);
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonResponse);

    if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
    }

    if (!doc["voices"].is<JsonArray>()) {
        Serial.println(F("JSON response missing 'voices' key"));
        return;
    }

    JsonArray voices = doc["voices"];
    String newOptions = "";
    newOptions.reserve(1024);

    for (JsonVariant v : voices) {
        const char* voiceName = v.as<const char*>();
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
        Serial.println("Voice list updated in dropdown.");
    } else {
        Serial.println("No matching voices found.");
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
    int originalMicMode = settings.micMode;
    settings.micMode = 1; // Force Left Channel Only (Bypasses AEC logic in record())
    
    Serial.println("Playing TTS & Recording (10s)...");
    speaker.playSpeechFromFile(ttsFile);
    
    size_t wavSize2 = 0;
    uint8_t* wavData2 = speech.record(10000, &wavSize2, 0);
    
    speaker.stop();
    settings.micMode = originalMicMode; // Restore settings

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

void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      inputBuffer.trim();
      if (inputBuffer.length() > 0) {
        if (inputBuffer == "/settings") {
          Serial.println("\n--- Current Settings ---");
          Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
          Serial.printf("CPU Freq:   %d MHz\n", getCpuFrequencyMhz());
          Serial.printf("RSSI:       %d dBm\n", network.getSignalStrength());
          Serial.printf("WiFi SSID:  %s\n", settings.wifiSSID);
          Serial.printf("WiFi Pass:  %s\n", settings.wifiPass);
          Serial.printf("API URL:    %s\n", settings.apiUrl);
          Serial.printf("API Key:    %s\n", settings.apiKey);
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
          Serial.println("------------------------\n");
        } else if (inputBuffer == "/debug_aec") {
          static bool d = false;
          d = !d;
          setAecDebug(d);
        } else if (inputBuffer.startsWith("/aec_delay ")) {
          int d = inputBuffer.substring(11).toInt();
          if (d > 0) setAecDelay(d);
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
        } else if (inputBuffer.startsWith("/say ")) {
          String textToSay = inputBuffer.substring(5);
          textToSay.trim();
          if (textToSay.length() > 0) {
            Serial.println("Direct TTS: " + textToSay);
            speaker.stop();
            isSpeaking = false;
            display.showStatus("Direct TTS...");
            
            // Download TTS to file, then play
            if (llm.downloadTTS(textToSay, network, "/speech.mp3", ttsVoice)) {
                speaker.playSpeechFromFile("/speech.mp3");
                isSpeaking = true;
            } else {
                display.showResponse("TTS Failed");
            }
          }
        } else if (inputBuffer == "/test_mic" && settings.debugMode) {
          Serial.println("Testing Microphone (5s recording)...");
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
              } else {
                  Serial.println("Failed to open file for writing");
                  display.showStatus("Save Failed");
              }
              free(wavData);
          } else {
              Serial.println("Recording failed");
              display.showStatus("Record Failed");
          }
        } else if (inputBuffer == "/test_mic" && !settings.debugMode) {
            Serial.println("Debug mode disabled. Enable debug to run mic test.");
        } else if (inputBuffer == "/new") {
          llm.clearHistory();
          Serial.println("Conversation history cleared.");
        } else {
          speaker.stop(); // Stop any current playback before processing new request
          isSpeaking = false;
          display.showThinking(true);
          display.showStatus("Thinking...");
          // Force UI update before the blocking API call
          lv_timer_handler(); 
          
          // NOTE: This call is blocking. UI will be unresponsive until it returns.
          String answer = llm.sendPrompt(inputBuffer, network);
          display.showThinking(false);
          
          Serial.println("Answer:");
          Serial.println(answer);

          display.showResponse("Speaking...");
          
          // Download TTS to file, then play
          if (llm.downloadTTS(answer, network, "/speech.mp3", ttsVoice)) {
              speaker.playSpeechFromFile("/speech.mp3");
              isSpeaking = true;
          } else {
              display.showResponse("TTS Failed");
          }
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
  pixels.clear();
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

  // Check for Factory Reset (BOOT button held during startup)
  if (digitalRead(0) == LOW) {
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
      Serial.println("Migrated API URL to /api/chat/completions");
  }

  llm.setConfig(settings.apiUrl, settings.apiKey, settings.llmModel);
  // Speaker volume will be applied after begin()

  // Initialize Speaker early so volume buttons work immediately
  speaker.begin();
  speaker.setVolume(settings.volume);

  // Check for chime file
  if (!LittleFS.exists(CHIME_FILENAME)) {
      Serial.println("Warning: /chime.mp3 not found. Please upload it to LittleFS.");
  }

  display.begin(settings.calibration);
  display.setVolumeCallback(onVolumeChange);
  display.setWiFiConfigCallback(onWiFiConfig);
  display.setAdminConfigCallback(onAdminConfig);
  display.setVoiceCallback(onVoiceChange);
  display.setSetupModeCallback(onSetupMode);
  display.setBalanceCallback(onBalanceChange);

  // Check for touch calibration status
  if (settings.calibration.isValid) {
      Serial.println("Touch calibration found. Loading saved values...");
  } else {
      Serial.println("Touch calibration NOT found or reset. Starting calibration utility...");
      display.calibrateTouch(settings.calibration);
      settings.save(); // Save the newly generated calibration to NVS
      Serial.println("Touch calibration completed and saved.");
  }

  display.showBootLogo(); // Show the logo immediately
  delay(3000); // Wait 3 seconds to see logo
  
  display.showStatus("AI Interactor Version " FIRMWARE_VERSION);
  display.showStatus("Retrotech&Electronics");
  display.showStatus("Powered by Edge Impulse");
  display.showStatus("UI by LVGL");

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
      Serial.println("WiFi Config Success: Saving to NVRAM.");
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
          Serial.println("WiFi Recovery Success: Saving to NVRAM.");
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
  server.begin();
  isWebServerActive = true;
  Serial.println("Web Server started at http://aiesp.local or http://" + WiFi.localIP().toString());

  // Initialize Speech Recognition (I2S)
  speech.begin();
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
                  Serial.println("Web Config Loop running...");
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
              Serial.println("API Verified: " + models);
              display.showStatus("API Verified|OK");
              modelOptions = models;
              delay(2000);
              apiVerified = true;
              break;
          }
          display.showStatus("API Check Failed|FAIL");
          Serial.println("API Check Failed: " + models);
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
              Serial.println("API Check Failed. Restarting WiFi...");
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
  
  Serial.println("Boot complete. Type prompt in Serial.");
  
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
  
  // Check for Wake Word if not already speaking or in web config mode
  if (!isSpeaking && !isWebServerActive) {
      if (speech.detectWakeWord(wakeThreshold)) {
          Serial.println("Wake Word Detected!");
          speaker.playSpeechFromFile(CHIME_FILENAME);
          
          // Visual Wake Indication (Rainbow Cycle)
          // Run for approx 1 second (6 cycles)
          for(int j=0; j<6; j++) {
              for(int i=0; i<256; i+=8) {
                  pixels.setPixelColor(0, pixels.ColorHSV(i*256));
                  pixels.show();
                  delay(5);
              }
          }
          
          // Flash Status Box instead of border
          display.flashStatusAnimation();
          
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