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

String inputBuffer = "";
bool isSpeaking = false;
bool isWebServerActive = false;
bool forceConfig = false;

void onVolumeChange(int value) {
    settings.volume = value;
    Serial.printf("Volume: %d (Tone disabled)\n", settings.volume);
    speaker.setVolume(settings.volume);
    settings.save();
}

void onAdminConfig(String pass) {
    if (pass.length() > 0) {
        adminPassword = pass;
        adminPrefs.putString("pass", adminPassword);
        display.showStatus("Admin Password Saved");
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
        if (isSpeaking) {
            speaker.stop();
            isSpeaking = false;
            display.showStatus("Tap to Talk");
            return;
        }

        // 1. Record
        display.showStatus("Listening...");
        lv_timer_handler(); // Force UI update
        
        size_t wavSize = 0;
        // Record for 5 seconds (adjust as needed)
        uint8_t* wavData = speech.record(5000, &wavSize);
        
        if (wavData && wavSize > 0) {
            // 2. Transcribe
            display.showStatus("Transcribing...");
            lv_timer_handler();
            String text = llm.transcribeAudio(wavData, wavSize, network);
            free(wavData); // Free PSRAM immediately
            
            if (text.startsWith("Error")) {
                Serial.println("Transcription Failed: " + text);
                display.showStatus(text.c_str());
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
                } else {
                    display.showStatus("TTS Failed");
                }
            }
        } else {
            display.showStatus("Record Failed");
        }
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

    // Temporarily apply config to test connection
    llm.setConfig(newApiUrl, newApiKey, settings.llmModel);
    String models = llm.getModels(network);

    if (models.startsWith("Error")) {
        // Revert to old settings
        llm.setConfig(settings.apiUrl, settings.apiKey, settings.llmModel);
        String html = "<html><body><h1>Connection Failed</h1><p>Error: " + models + "</p><p>Settings were <b>NOT</b> saved.</p><a href='/'>Go Back</a></body></html>";
        server.send(200, "text/html", html);
    } else {
        // Success - Save to NVRAM
        strlcpy(settings.apiKey, newApiKey.c_str(), sizeof(settings.apiKey));
        strlcpy(settings.apiUrl, newApiUrl.c_str(), sizeof(settings.apiUrl));
        strlcpy(settings.timeZone, newTz.c_str(), sizeof(settings.timeZone));
        strlcpy(settings.clockColor, newColor.c_str(), sizeof(settings.clockColor));
        settings.save();
        forceConfig = false;
        
        // Apply Timezone immediately
        setenv("TZ", settings.timeZone, 1);
        tzset();
        
        server.send(200, "text/html", "<html><body><h1>Saved & Verified!</h1><p>Connection successful.</p><a href='/'>Back</a></body></html>");
        Serial.println("Settings updated and verified via Web Interface");
    }
}

void onWiFiConfig(String ssid, String pass) {
    network.setCredentials(ssid, pass);

    for (int i = 1; i <= 2; i++) {
        display.showStatus(("Connecting (" + String(i) + "/2)...").c_str());
        lv_timer_handler(); // Force UI update to show attempt count
        network.connect();
        if (network.isConnected()) {
            strlcpy(settings.wifiSSID, ssid.c_str(), sizeof(settings.wifiSSID));
            strlcpy(settings.wifiPass, pass.c_str(), sizeof(settings.wifiPass));
            return;
        }
    }

    display.showStatus("Fail to connect");
    lv_timer_handler();
    delay(2000);
    display.showWiFiConfig();
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
          Serial.printf("Touch Cal:  %s (%d,%d to %d,%d)\n", 
            settings.calibration.isValid ? "Valid" : "Invalid",
            settings.calibration.xMin, settings.calibration.yMin,
            settings.calibration.xMax, settings.calibration.yMax);
          Serial.println("------------------------\n");
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
        } else if (inputBuffer.startsWith("/llm ")) {
          String newModel = inputBuffer.substring(5);
          newModel.trim();
          if (newModel.length() > 0) {
            strlcpy(settings.llmModel, newModel.c_str(), sizeof(settings.llmModel));
            settings.save();
            llm.setConfig(settings.apiUrl, settings.apiKey, settings.llmModel);
            Serial.println("LLM Model updated to: " + newModel);
          }
        } else if (inputBuffer == "/test_mic") {
          Serial.println("Testing Microphone (5s recording)...");
          display.showStatus("Recording (5s)...");
          speaker.stop(); // Stop any playback
          
          size_t wavSize = 0;
          uint8_t* wavData = speech.record(5000, &wavSize);
          
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
        } else if (inputBuffer == "/new") {
          llm.clearHistory();
          Serial.println("Conversation history cleared.");
        } else if (inputBuffer == "/voices") {
          updateVoiceList();
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

  display.begin(settings.calibration);
  display.setVolumeCallback(onVolumeChange);
  display.setWiFiConfigCallback(onWiFiConfig);
  display.setAdminConfigCallback(onAdminConfig);
  display.setVoiceCallback(onVoiceChange);
  display.setSetupModeCallback(onSetupMode);

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
  delay(3000); // Wait 3 seconds so we can see the logo

  // Admin Password Check
  adminPassword = adminPrefs.getString("pass", "");
  ttsVoice = adminPrefs.getString("voice", "alloy");
  if (adminPassword == "") {
      display.showAdminConfig();
      while (adminPassword == "") {
          lv_timer_handler();
          handleSerialCommands();
          delay(5);
      }
  }

  // WiFi Connection Logic
  if (strlen(settings.wifiSSID) == 0 || strcmp(settings.wifiSSID, "YOUR_WIFI_SSID") == 0 || 
      strlen(settings.wifiPass) == 0 || strcmp(settings.wifiPass, "YOUR_WIFI_PASSWORD") == 0) {
      display.showWiFiConfig();
      while (!network.isConnected()) {
          lv_timer_handler();
          handleSerialCommands();
          delay(5);
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
      }

      if (!network.isConnected()) {
          display.showStatus("WiFi Failed|FAIL");
          display.showWiFiError("WiFi Connection Failed");
          display.showWiFiError("WiFi Connection Failed after 5 attempts.");
          while (!network.isConnected()) {
              lv_timer_handler();
              handleSerialCommands();
              delay(5);
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

      // 2. Verify API Connection (3 attempts)
      bool apiVerified = false;
      for (int i = 0; i < 3; i++) {
          display.showStatus(("Verifying API (" + String(i + 1) + "/3)...").c_str());
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
  llm.setSystemPrompt("You are a helpful AI assistant running on an ESP32-S3.");
  
  // Final UI Load
  display.showMainUI(ttsVoice, settings.volume, voiceOptions);
  display.showStatus("Tap to Talk");
  
  Serial.println("Boot complete. Type prompt in Serial.");
  
  // Stop web server after boot configuration is complete to save cycles
  server.stop();
  isWebServerActive = false;
}

void loop() {
  // Handle LVGL GUI - Skip updates during playback to prioritize audio bus bandwidth
  if (!isSpeaking) {
    lv_timer_handler();
  }

  // Keep loop responsive
  delay(5);

  // Check if speaking finished
  if (isSpeaking && !speaker.isRunning()) {
      isSpeaking = false;
      String waitMsg = "Tap to Talk\nRSSI: " + String(network.getSignalStrength()) + " dBm";
      display.showStatus(waitMsg.c_str());
  }
  handleSerialCommands();
  if (isWebServerActive) {
      server.handleClient();
  }
}