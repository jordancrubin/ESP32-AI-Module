#include <Arduino.h>
#include "Config.h"
#include "DisplayManager.h"
#include "WiFiManager.h"
#include "LLMClient.h"
#include "SpeechManager.h"
#include "SpeakerManager.h"
#include "SettingsManager.h"

DisplayManager display;
WiFiManager network(WIFI_SSID, WIFI_PASS);
LLMClient llm(OPENWEBUI_URL, OPENWEBUI_KEY, LLM_MODEL);
SpeechManager speech;
SpeakerManager speaker;
SettingsManager settings;

void progressCallback(int percent, float speed) {
    display.showProgress(percent, speed);
    for(int i=0; i<5; i++) lv_timer_handler(); // Process queued touch events
}

void onVolumeChange(int change) {
    settings.volume += change;
    if (settings.volume < 0) settings.volume = 0;
    if (settings.volume > 21) settings.volume = 21;
    
    Serial.printf("Volume: %d\n", settings.volume);
    speaker.setVolume(settings.volume);
    // Play a tone that changes pitch with the volume level
    int freq = 220 + (settings.volume * 30); 
    speaker.playTone(freq, 100); 
    settings.save();
}

void onBrightnessChange(int change) {
    settings.brightness += change;
    if (settings.brightness < 0) settings.brightness = 0;
    if (settings.brightness > 255) settings.brightness = 255;
    
    Serial.printf("Brightness: %d\n", settings.brightness);
    display.setBrightness(settings.brightness);
    settings.save();
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
    while (!Serial && (millis() - start < 3000));

  Serial.println("System Starting...");

  // Load Settings
  settings.begin();
  // Apply loaded settings
  network.setCredentials(settings.wifiSSID, settings.wifiPass);
  llm.setConfig(settings.apiUrl, settings.apiKey, settings.llmModel);
  // Speaker volume will be applied after begin()

  // Initialize Speaker early so volume buttons work immediately
  speaker.begin();
  speaker.setVolume(settings.volume);

  display.begin(settings.calibration);
  display.setBrightness(settings.brightness);
  display.setVolumeCallback(onVolumeChange);
  display.setBrightnessCallback(onBrightnessChange);

  // Check for touch calibration status
  if (settings.calibration.isValid) {
      Serial.println("Touch calibration found. Loading saved values...");
  } else {
      Serial.println("Touch calibration NOT found. Starting calibration utility...");
      display.calibrateTouch(settings.calibration);
      settings.save(); // Save the newly generated calibration to NVS
      Serial.println("Touch calibration completed and saved.");
  }

  display.showBootLogo(); // Show the logo immediately
  delay(3000); // Wait 3 seconds so we can see the logo
  display.showStatus("Initializing...");

  network.connect();
  display.showStatus("WiFi Connected!");
  
  // Initialize Speech Recognition (I2S)
  speech.begin();

  // Check API Connection and Models
  while (true) {
      display.showStatus("Checking API...");
      String models = llm.getModels(network);
      if (models.startsWith("Error")) {
          Serial.println("API Check Failed: " + models);
          display.showStatus("API Fail. Retrying...");
          delay(2000);
          network.connect();
      } else {
          Serial.println("API Models: " + models);
          display.showResponse(models);
          delay(3000);
          break;
      }
  }

  // Set a system prompt using PSRAM allocation
  llm.setSystemPrompt("You are a helpful AI assistant running on an ESP32-S3.");
  
  display.showStatus("Waiting...");
  
  Serial.println("Boot complete. Type prompt in Serial.");
}

void loop() {
  static String inputBuffer = "";
  static bool isSpeaking = false;
  
  // Handle LVGL GUI
  lv_timer_handler();
  delay(5);

  // Check if speaking finished
  if (isSpeaking && !speaker.isRunning()) {
      isSpeaking = false;
      String waitMsg = "Waiting...\nRSSI: " + String(network.getSignalStrength()) + " dBm";
      display.showStatus(waitMsg.c_str());
  }

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      inputBuffer.trim();
      if (inputBuffer.length() > 0) {
        if (inputBuffer == "/settings") {
          Serial.println("\n--- Current Settings ---");
          Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
          Serial.printf("RSSI:       %d dBm\n", network.getSignalStrength());
          Serial.printf("WiFi SSID:  %s\n", settings.wifiSSID.c_str());
          Serial.printf("API URL:    %s\n", settings.apiUrl.c_str());
          Serial.printf("LLM Model:  %s\n", settings.llmModel.c_str());
          Serial.printf("Volume:     %d / 21\n", settings.volume);
          Serial.printf("Brightness: %d / 255\n", settings.brightness);
          Serial.printf("Touch Cal:  %s (%d,%d to %d,%d)\n", 
            settings.calibration.isValid ? "Valid" : "Invalid",
            settings.calibration.xMin, settings.calibration.yMin,
            settings.calibration.xMax, settings.calibration.yMax);
          Serial.println("------------------------\n");
        } else {
        speaker.stop(); // Stop any current playback before processing new request
        isSpeaking = false;
        display.showThinking(true);
        display.showStatus("Thinking...");
        lv_timer_handler(); // Force UI update before blocking call
        String answer = llm.sendPrompt(inputBuffer, network);
        display.showThinking(false);
        
        Serial.println("Answer:");
        Serial.println(answer);

        String statusMsg = "Speaking...\n\n";
        statusMsg += "Free PSRAM:\n" + String(ESP.getFreePsram());
        
        display.showResponse(statusMsg);
        
        // Download TTS from local server and play
        uint8_t* audioData = nullptr;
        size_t audioSize = 0;
        
        if (llm.downloadTTS(answer, network, &audioData, &audioSize, progressCallback)) {
            speaker.playAudioFromRAM(audioData, audioSize);
            isSpeaking = true;
        } else {
            // Fallback if TTS fails so we don't stay on "Speaking..."
            display.showResponse("TTS Failed\n" + statusMsg);
        }
        }
      }
      inputBuffer = "";
    } else {
      inputBuffer += c;
    }
  }
}