/*
  main.cpp - ESP32 AI Module Firmware
  Main application entry point. Orchestrates Display, WiFi, LLM, and Audio subsystems.
  Functionality requires WiFi connection and valid API credentials.
  
  https://www.youtube.com/@rubin-tech
  2026 Jordan Rubin.
*/
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
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
float wakeThreshold = 0.5;
float ttsSpeed = 0.8;

float g_currentVocKOhms = 0.0;
int vocAlarmThreshold = 25;
unsigned long lastVocAlarmTime = 0;

bool enableBME680 = false;
bool enableTTSChunking = false;
bool bmeReady = false;
Adafruit_BME680 bme;
bool clockFormat12h = false;

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
unsigned long g_audioPlaybackStart = 0;
bool g_runAecTuneUI = false;
bool g_runAecTestUI = false;
bool g_runHarmonicTestUI = false;
bool g_runResonanceTuneUI = false;
bool g_runTuneAllUI = false;
bool g_runBargeInTuneUI = false;

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
extern int32_t getLastAecBurstLevel();
extern int32_t getLastAecEvalLevel();
extern int getEffectiveAecAttenuation();
extern int32_t getEffectiveAecCutoff();
extern void renderBSOD();
extern void renderGuruMeditation();
extern void forceClockScreen();
extern void showAecTuningUI();
extern void updateAecTuningUI(int percent, const char* msg);
extern void showHelpScreen();
extern void flushAecBuffer();

// Forward declarations
void testAEC();
void testHarmonic();
void tuneAEC();
void tuneResonance(bool returnToMainUI = true);
void calibrateBargeIn(bool returnToMainUI = true);
void tuneAllAudio();
void onSetupMode(bool enabled);
void performFactoryReset();
void updateVoiceList();

// Timer Variables
extern uint32_t g_pendingTimerSeconds;
extern bool g_cancelTimer;
unsigned long timerStartTime = 0;
uint32_t timerDurationMs = 0;
bool timerActive = false;
bool timerRinging = false;
unsigned long lastInterruptTime = 0;
unsigned long lastRingTime = 0;

bool alarmRinging = false;
unsigned long lastAlarmRingTime = 0;

// Chime Configuration (Prefers uncompressed 24kHz WAV, falls back to MP3)
const char* getChimeFilename() {
    static char chimePath[16] = "";
    if (chimePath[0] == '\0') {
        if (LittleFS.exists("/chime.wav")) {
            strlcpy(chimePath, "/chime.wav", sizeof(chimePath));
        } else {
            strlcpy(chimePath, "/chime.mp3", sizeof(chimePath));
        }
    }
    return chimePath;
}
#define CHIME_FILENAME getChimeFilename()

// Onboard RGB LED
Adafruit_NeoPixel pixels(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

unsigned long getEffectiveAecIgnore() {
    float v_norm = constrain((float)settings.volume / 21.0f, 0.0f, 1.0f);
    return (unsigned long)roundf(settings.aecIgnore * (0.8f + v_norm * 0.4f));
}

bool checkPlaybackBargeIn() {
    if (!settings.enableInterrupt) return false;
    bool triggered = speech.detectWakeWord(wakeThreshold);
    if (!triggered) return false;

    unsigned long elapsed = millis() - g_audioPlaybackStart;
    unsigned long eff_ignore = getEffectiveAecIgnore();
    if (elapsed > eff_ignore) {
        Serial.printf("\n>>> Playback interrupted by user! Elapsed: %lu ms, Ignore: %lu ms (base:%d ms), Eff Cutoff: %d (Vol: %d/21) <<<\n",
                      elapsed, eff_ignore, settings.aecIgnore, getEffectiveAecCutoff(), settings.volume);
        speaker.stop();
        speaker.playSpeechFromFile(CHIME_FILENAME);
        while(speaker.isRunning()) delay(30);
        flushAecBuffer();
        return true;
    } else {
        Serial.printf("[Barge-In] Interrupt ignored during initial ignore window (%lu ms <= %lu ms)\n",
                      elapsed, eff_ignore);
        return false;
    }
}

void onVolumeChange(int value) {
    settings.volume = value;
    if (settings.debugMode) Serial.printf("Volume: %d (Tone disabled)\n", settings.volume);
    speaker.setVolume(settings.volume);
    settings.save();
    Serial.printf("[Volume] Set to %d/21 -> Effective Barge-In: Atten=%d%%, Cutoff=%d, Ignore=%lums\n",
                  settings.volume, getEffectiveAecAttenuation(), getEffectiveAecCutoff(), getEffectiveAecIgnore());
}

void onBalanceChange(int value) {
    settings.inputBalance = value;
    setInputBalance(value);
    settings.save();
}

const char* getSpeechFilename(int toggle = -1) {
    bool isWav = (strcmp(settings.ttsFormat, "wav") == 0);
    if (toggle == 0) return isWav ? "/speech0.wav" : "/speech0.mp3";
    if (toggle == 1) return isWav ? "/speech1.wav" : "/speech1.mp3";
    return isWav ? "/speech.wav" : "/speech.mp3";
}

// Plays a chunk of TTS immediately and alternates files to avoid stuttering
bool playChunkedTTS(String text) {
    static int fileToggle = 0;
    
    const char* filename = getSpeechFilename(fileToggle);
    
    // 1. Download the next chunk in the background while previous plays
    bool downloadSuccess = llm.downloadTTS(text, network, filename, ttsVoice);
    
    // 2. Wait for previous chunk to finish playing, handle interrupts
    bool interrupted = false;

    if (settings.enableInterrupt) setInterruptMode(true);
    
    while (speaker.isRunning()) {
        if (settings.enableInterrupt) {
            if (checkPlaybackBargeIn()) {
                interrupted = true;
                break;
            }
            delay(20);
        } else {
            flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
            delay(50);
        }
    }
    if (settings.enableInterrupt) setInterruptMode(false);
    if (interrupted) return false;

    // 3. Play the newly downloaded chunk
    if (downloadSuccess) {
        speaker.playSpeechFromFile(filename);
        if (!isSpeaking) g_audioPlaybackStart = millis(); // Only reset ignore window on first chunk
        isSpeaking = true;
        fileToggle = 1 - fileToggle; // alternate between 0 and 1
    }
    return true;
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
        display.updateModelLabel(settings.llmModel);
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
                    bool tuneResonance_flag = cmd.tuneResonance;
                    bool showHelp = cmd.showHelp;
                    bool isTimerCmd = (g_pendingTimerSeconds > 0 || g_cancelTimer);
                    if (showBSOD || showGuruMeditation || showHelp || isTimerCmd) triggeredEasterEgg = true;

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

                    if (!enableTTSChunking && answer != "Interrupted by user") {
                        const char* speechFile = getSpeechFilename();
                        if (llm.downloadTTS(answer, network, speechFile, ttsVoice)) {
                            speaker.playSpeechFromFile(speechFile);
                            if (!isSpeaking) g_audioPlaybackStart = millis();
                            isSpeaking = true;
                            lv_timer_handler(); // Update UI once to show "Speaking"
                            
                            bool interrupted = false;
                            if (settings.enableInterrupt) setInterruptMode(true);

                            // Wait for playback to finish, or poll for interruptions
                            while (speaker.isRunning()) {
                                if (settings.enableInterrupt) {
                                    if (checkPlaybackBargeIn()) {
                                        interrupted = true;
                                        break;
                                    }
                                    delay(20); // Yield cleanly to audioTask
                                } else {
                                    flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
                                    delay(50);
                                }
                            }
                            
                            if (settings.enableInterrupt) setInterruptMode(false);
                            isSpeaking = false;
                            
                            if (interrupted) continue; // Skip the rest, loop back to "Listening..."
                        }
                        else {
                            display.showStatus("TTS Failed");
                            break;
                        }
                    }
                    else {
                        // Chunking mode
                        bool interrupted = false;
                        
                        if (answer != "Interrupted by user" && answer.length() > 0) {
                            String sentence = "";
                            for (size_t i = 0; i < answer.length(); i++) {
                                char c = answer[i];
                                sentence += c;
                                bool isPunctuation = (c == '.' || c == '?' || c == '!' || c == '\n');
                                bool isPause = (c == ',' || c == ';' || c == ':');
                                if (isPunctuation || (isPause && sentence.length() > 60)) {
                                    sentence.trim();
                                    bool hasWords = false;
                                    for (size_t j = 0; j < sentence.length(); j++) {
                                        if (isalnum(sentence[j])) { hasWords = true; break; }
                                    }
                                    if (hasWords) {
                                        if (!playChunkedTTS(sentence)) {
                                            interrupted = true;
                                            break;
                                        }
                                    }
                                    sentence = "";
                                }
                            }
                            if (!interrupted) {
                                sentence.trim();
                                bool hasWords = false;
                                for (size_t j = 0; j < sentence.length(); j++) {
                                    if (isalnum(sentence[j])) { hasWords = true; break; }
                                }
                                if (hasWords) {
                                    playChunkedTTS(sentence);
                                }
                            }
                        }
                        
                        // Wait for last chunk to finish gracefully
                        if (settings.enableInterrupt) setInterruptMode(true);
                        while (speaker.isRunning()) {
                            if (settings.enableInterrupt) {
                                if (checkPlaybackBargeIn()) {
                                    interrupted = true;
                                    break;
                                }
                                delay(20);
                            } else {
                                flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
                                delay(50);
                            }
                        }
                        if (settings.enableInterrupt) setInterruptMode(false);
                        isSpeaking = false;
                        if (interrupted || answer == "Interrupted by user") continue;
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

                        if (tuneResonance_flag) {
                            display.showStatus("Auto-Tuning Resonance...");
                            lv_timer_handler();
                            tuneResonance();
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
                        
                        if (isTimerCmd) {
                            break;       // End the conversation loop
                        }
                }
            }
            else {
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
        }
        else if (!isWebServerActive) {
            display.showStatus("Ready"); // Normal recovery
        }
        setLedColor(0, 0, 0); // LED Off
    } else {
        ttsVoice = voice;
        adminPrefs.putString("voice", ttsVoice);
        if (settings.debugMode) Serial.println("Voice changed to: " + ttsVoice);
        
        // Force a UI update so the dropdown menu closes cleanly before blocking
        lv_timer_handler(); 
        
        // Strip everything before and including the underscore for the spoken announcement
        String spokenVoice = ttsVoice;
        int underscorePos = spokenVoice.lastIndexOf('_');
        if (underscorePos != -1) {
            spokenVoice = spokenVoice.substring(underscorePos + 1);
        }
        
        // Speak the confirmation using the newly selected voice
        String textToSay = "Changing to " + spokenVoice;
        if (!enableTTSChunking) {
            const char* speechFile = getSpeechFilename();
            if (llm.downloadTTS(textToSay, network, speechFile, ttsVoice)) {
                speaker.playSpeechFromFile(speechFile);
                if (!isSpeaking) g_audioPlaybackStart = millis();
                isSpeaking = true;
                lv_timer_handler();
                
                if (settings.enableInterrupt) setInterruptMode(true);
                while(speaker.isRunning()) {
                    if (settings.enableInterrupt) {
                        if (checkPlaybackBargeIn()) {
                            break;
                        }
                        delay(20);
                    } else {
                        flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
                        delay(50);
                    }
                }
                if (settings.enableInterrupt) setInterruptMode(false);
                isSpeaking = false;
            }
        } else {
            playChunkedTTS(textToSay);
        }
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
    encodedString.reserve(str.length() * 3); // Pre-allocate to prevent heap fragmentation
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

    String html;
    html.reserve(16384);
    html += "<!DOCTYPE html><html><head><title>ESP32 AI Assistant</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:'Segoe UI',Tahoma,sans-serif;background:#f0f2f5;color:#333;margin:0;padding:15px;}";
    html += ".container{max-width:800px;margin:auto;background:#fff;padding:25px;border-radius:10px;box-shadow:0 4px 15px rgba(0,0,0,0.05);}";
    html += "h2{color:#2c3e50;border-bottom:2px solid #008CBA;padding-bottom:10px;margin-top:0;}";
    html += ".tabs{display:flex;border-bottom:2px solid #e0e0e0;margin-bottom:25px;flex-wrap:wrap;}";
    html += ".tab{padding:12px 20px;cursor:pointer;background:#f8f9fa;margin-right:5px;border-radius:6px 6px 0 0;font-weight:600;color:#6c757d;border:1px solid transparent;border-bottom:none;transition:background 0.2s;}";
    html += ".tab:hover{background:#e2e6ea;}";
    html += ".tab.active{background:#fff;border-color:#e0e0e0;margin-bottom:-2px;color:#008CBA;border-bottom:2px solid #fff;}";
    html += ".tab-content{display:none;animation:fade .4s;}";
    html += ".tab-content.active{display:block;}";
    html += "@keyframes fade{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:translateY(0)}}";
    html += "input[type=text],input[type=number],select,textarea{width:100%;padding:10px;margin:8px 0 20px 0;border:1px solid #ced4da;border-radius:5px;box-sizing:border-box;font-family:inherit;}";
    html += "input[type=range]{width:100%;margin:15px 0;}";
    html += ".btn{background-color:#008CBA;color:white;border:none;cursor:pointer;padding:12px;border-radius:5px;font-size:16px;width:100%;margin-top:15px;font-weight:bold;transition:background 0.2s;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
    html += ".btn:hover{background-color:#007bb5;}";
    html += ".btn-red{background-color:#dc3545;} .btn-red:hover{background-color:#c82333;}";
    html += ".btn-orange{background-color:#fd7e14;} .btn-orange:hover{background-color:#e86e04;}";
    html += ".card{background:#f8f9fa;padding:20px;border-radius:8px;margin-bottom:20px;border:1px solid #e9ecef;}";
    html += ".card h3{margin-top:0;color:#343a40;font-size:1.1em;border-bottom:1px solid #dee2e6;padding-bottom:8px;}";
    html += "label{font-weight:600;color:#495057;display:block;}";
    html += "small{color:#6c757d;display:block;margin-top:-16px;margin-bottom:15px;font-size:0.85em;line-height:1.4;}";
    html += ".checkbox-group{display:flex;align-items:center;margin-bottom:15px;background:#fff;padding:10px;border-radius:5px;border:1px solid #ced4da;}";
    html += ".checkbox-group input{width:auto;margin:0 15px 0 5px;transform:scale(1.3);cursor:pointer;}";
    html += ".checkbox-group label{margin:0;cursor:pointer;flex-grow:1;}";
    html += "ul.diag-list{list-style:none;padding:0;margin:0;}";
    html += "ul.diag-list li{background:#fff;border:1px solid #dee2e6;margin-bottom:8px;padding:12px 15px;border-radius:5px;display:flex;justify-content:space-between;align-items:center;box-shadow:0 1px 3px rgba(0,0,0,0.02);}";
    html += "ul.diag-list li span{font-weight:500;color:#495057;}";
    html += "ul.diag-list li a{color:#008CBA;text-decoration:none;font-weight:600;padding:4px 8px;border-radius:4px;background:#e9ecef;transition:background 0.2s;}";
    html += "ul.diag-list li a:hover{background:#dee2e6;}";
    html += ".modal{display:none;position:fixed;z-index:1000;left:0;top:0;width:100%;height:100%;background-color:rgba(0,0,0,0.5);}";
    html += ".modal-content{background-color:#fff;margin:15% auto;padding:25px;border-radius:8px;width:80%;max-width:400px;text-align:center;box-shadow:0 4px 15px rgba(0,0,0,0.2);}";
    html += ".modal-content h3{margin-top:0;}";
    html += ".close-btn{background-color:#008CBA;color:white;border:none;padding:10px 20px;border-radius:5px;cursor:pointer;margin-top:15px;font-size:16px;}";
    html += "</style>";
    html += "<script>";
    html += "function openTab(evt, tabName) {";
    html += "  var i, x, tablinks;";
    html += "  x = document.getElementsByClassName('tab-content');";
    html += "  for (i = 0; i < x.length; i++) { x[i].classList.remove('active'); }";
    html += "  tablinks = document.getElementsByClassName('tab');";
    html += "  for (i = 0; i < x.length; i++) { tablinks[i].classList.remove('active'); }";
    html += "  document.getElementById(tabName).classList.add('active');";
    html += "  evt.currentTarget.classList.add('active');";
    html += "  document.getElementById('saveBtnContainer').style.display = (tabName === 'Diagnostics') ? 'none' : 'block';";
    html += "}";
    html += "function toggleCustomModel(val) {";
    html += "  var el = document.getElementById('customModelDiv');";
    html += "  if (el) el.style.display = (val === '__custom__') ? 'block' : 'none';";
    html += "}";
    html += "function toggleCustomVoice(val) {";
    html += "  var el = document.getElementById('customVoiceDiv');";
    html += "  if (el) el.style.display = (val === '__custom__') ? 'block' : 'none';";
    html += "}";
    html += "document.addEventListener('DOMContentLoaded', function() {";
    html += "  document.getElementById('configForm').addEventListener('submit', function(e) {";
    html += "    e.preventDefault();";
    html += "    document.getElementById('saveModal').style.display='block';";
    html += "    document.getElementById('modalTitle').innerText='Saving...';";
    html += "    document.getElementById('modalTitle').style.color='#333';";
    html += "    document.getElementById('modalText').innerText='Please wait while settings are verified. This may take a few seconds.';";
    html += "    document.getElementById('modalClose').style.display='none';";
    html += "    var formData = new URLSearchParams(new FormData(this));";
    html += "    fetch('/save', { method: 'POST', body: formData })";
    html += "    .then(response => response.text())";
    html += "    .then(data => {";
    html += "      document.getElementById('modalClose').style.display='inline-block';";
    html += "      if (data === 'SUCCESS') {";
    html += "        document.getElementById('modalTitle').innerText='Saved & Verified!';";
    html += "        document.getElementById('modalTitle').style.color='#28a745';";
    html += "        document.getElementById('modalText').innerText='Connection successful and settings saved.';";
    html += "      } else {";
    html += "        document.getElementById('modalTitle').innerText='Saved (Verification Failed)';";
    html += "        document.getElementById('modalTitle').style.color='#dc3545';";
    html += "        document.getElementById('modalText').innerText='Settings saved, but API check failed: ' + data;";
    html += "      }";
    html += "    }).catch(err => {";
    html += "      document.getElementById('modalClose').style.display='inline-block';";
    html += "      document.getElementById('modalTitle').innerText='Error';";
    html += "      document.getElementById('modalTitle').style.color='#dc3545';";
    html += "      document.getElementById('modalText').innerText='Failed to communicate with device.';";
    html += "    });";
    html += "  });";
    html += "});";
    html += "</script>";
    html += "</head><body>";
    html += "<div id='saveModal' class='modal'><div class='modal-content'><h3 id='modalTitle'></h3><p id='modalText'></p><button class='close-btn' id='modalClose' onclick='document.getElementById(\"saveModal\").style.display=\"none\"'>OK</button></div></div>";
    html += "<div class='container'>";
    html += "<h2><img src='/logo.png' style='height:40px; vertical-align:middle; margin-right:15px;' onerror='this.style.display=\"none\"'>ESP32 AI Configuration</h2>";

    // Tabs
    html += "<div class='tabs'>";
    html += "<div class='tab active' onclick=\"openTab(event, 'General')\">General</div>";
    html += "<div class='tab' onclick=\"openTab(event, 'Audio')\">Audio & AEC</div>";
    html += "<div class='tab' onclick=\"openTab(event, 'System')\">System & Display</div>";
    html += "<div class='tab' onclick=\"openTab(event, 'Diagnostics')\">Diagnostics & Tools</div>";
    html += "</div>";

    // Open Form
    html += "<form id='configForm'>";

    // TAB: General
    html += "<div id='General' class='tab-content active'>";
    html += "<div class='card'>";
    html += "<label>API URL</label><input type='text' name='apiUrl' value='" + String(settings.apiUrl) + "'>";
    html += "<label>API Key</label><input type='text' name='apiKey' value='" + String(settings.apiKey) + "'>";

    // LLM Model Dropdown
    html += "<label>LLM Model</label>";
    html += "<select name='llmModel' id='llmModel' onchange='toggleCustomModel(this.value)'>";
    String mOpts = modelOptions;
    if (mOpts.length() == 0) {
        int sep = voiceOptions.indexOf("||");
        if (sep != -1) mOpts = voiceOptions.substring(sep + 2);
    }
    bool modelMatched = false;
    if (mOpts.length() > 0) {
        int mStart = 0;
        int mEnd = mOpts.indexOf('\n');
        while (mEnd != -1 || mStart < mOpts.length()) {
            String opt = (mEnd == -1) ? mOpts.substring(mStart) : mOpts.substring(mStart, mEnd);
            opt.trim();
            if (opt.length() > 0 && opt.indexOf("Error:") == -1) {
                String optId = opt;
                int op = opt.lastIndexOf('(');
                int cp = opt.lastIndexOf(')');
                if (op != -1 && cp != -1 && cp > op) {
                    optId = opt.substring(op + 1, cp);
                    optId.trim();
                }
                bool isSel = (!modelMatched && (optId == settings.llmModel || opt == settings.llmModel));
                if (isSel) modelMatched = true;
                html += "<option value='" + optId + "'" + (isSel ? " selected" : "") + ">" + opt + "</option>";
            }
            if (mEnd == -1) break;
            mStart = mEnd + 1;
            mEnd = mOpts.indexOf('\n', mStart);
        }
    }
    if (!modelMatched && strlen(settings.llmModel) > 0) {
        html += "<option value='" + String(settings.llmModel) + "' selected>" + String(settings.llmModel) + "</option>";
    }
    html += "<option value='__custom__'>Custom Model...</option>";
    html += "</select>";
    html += "<div id='customModelDiv' style='display:none;'><label>Custom Model Name/ID</label><input type='text' id='customModel' name='customModel' placeholder='e.g., gpt-4o-mini or mistral:latest'></div>";

    html += "<label>TTS Provider</label><select name='ttsProvider'>";
    html += "<option value='0'" + String(settings.ttsProvider == 0 ? " selected" : "") + ">OpenWebUI (Kokoro)</option>";
    html += "<option value='1'" + String(settings.ttsProvider == 1 ? " selected" : "") + ">Direct</option></select>";
    html += "<label>Direct TTS URL</label><input type='text' name='ttsUrl' value='" + String(settings.ttsUrl) + "' placeholder='e.g., http://host:port'>";
    html += "<small>Used when TTS Provider is 'Direct'. Must be full base URL.</small>";
    html += "<label>TTS Audio Format</label><select name='ttsFormat'>";
    html += "<option value='mp3'" + String(strcmp(settings.ttsFormat, "wav") != 0 ? " selected" : "") + ">MP3 (Compressed)</option>";
    html += "<option value='wav'" + String(strcmp(settings.ttsFormat, "wav") == 0 ? " selected" : "") + ">WAV (Raw PCM - Low CPU)</option>";
    html += "</select>";
    html += "<small>WAV bypasses MP3 decoding on ESP32 (0% decode CPU). Recommended for local Kokoro Direct TTS.</small>";

    // TTS Voice Dropdown
    html += "<label>TTS Voice</label>";
    html += "<select name='ttsVoice' id='ttsVoice' onchange='toggleCustomVoice(this.value)'>";
    String vOpts = voiceOptions;
    int vSep = vOpts.indexOf("||");
    if (vSep != -1) vOpts = vOpts.substring(0, vSep);

    const char* stdVoices[] = {
        "alloy", "echo", "fable", "onyx", "nova", "shimmer",
        "af_bella", "af_sarah", "af_nicole", "af_sky",
        "am_adam", "am_michael", "bf_emma", "bf_isabella", "bm_george", "bm_lewis"
    };

    bool voiceMatched = false;
    String addedVoices = "\n";
    if (vOpts.length() > 0 && vOpts != "alloy") {
        int vStart = 0;
        int vEnd = vOpts.indexOf('\n');
        while (vEnd != -1 || vStart < vOpts.length()) {
            String opt = (vEnd == -1) ? vOpts.substring(vStart) : vOpts.substring(vStart, vEnd);
            opt.trim();
            if (opt.length() > 0 && opt.indexOf("Error:") == -1) {
                if (addedVoices.indexOf("\n" + opt + "\n") == -1) {
                    addedVoices += opt + "\n";
                    bool isSel = (!voiceMatched && opt == ttsVoice);
                    if (isSel) voiceMatched = true;
                    html += "<option value='" + opt + "'" + (isSel ? " selected" : "") + ">" + opt + "</option>";
                }
            }
            if (vEnd == -1) break;
            vStart = vEnd + 1;
            vEnd = vOpts.indexOf('\n', vStart);
        }
    }
    for (size_t i = 0; i < sizeof(stdVoices) / sizeof(stdVoices[0]); i++) {
        String sv = stdVoices[i];
        if (addedVoices.indexOf("\n" + sv + "\n") == -1) {
            addedVoices += sv + "\n";
            bool isSel = (!voiceMatched && sv == ttsVoice);
            if (isSel) voiceMatched = true;
            html += "<option value='" + sv + "'" + (isSel ? " selected" : "") + ">" + sv + "</option>";
        }
    }
    if (!voiceMatched && ttsVoice.length() > 0) {
        html += "<option value='" + ttsVoice + "' selected>" + ttsVoice + "</option>";
    }
    html += "<option value='__custom__'>Custom Voice...</option>";
    html += "</select>";
    html += "<div id='customVoiceDiv' style='display:none;'><label>Custom Voice Name</label><input type='text' id='customVoice' name='customVoice' placeholder='e.g., af_bella or custom voice ID'></div>";

    html += "<label>TTS Speed (0.5 - 2.0)</label><input type='number' name='ttsSpeed' value='" + String(ttsSpeed) + "' step='0.05' min='0.5' max='2.0'>";
    html += "<label>System Prompt</label><textarea name='systemPrompt' rows='4'>" + String(settings.systemPrompt) + "</textarea>";
    html += "</div>";
    
    html += "<div class='card'><h3>AI Features</h3>";
    html += "<div class='checkbox-group'><input type='checkbox' id='memory' name='memory' value='1'" + String(settings.enableMemory ? " checked" : "") + "><label for='memory'>Enable Conversation Memory</label></div>";
    html += "<div class='checkbox-group'><input type='checkbox' id='calendar' name='calendar' value='1'" + String(settings.enableCalendar ? " checked" : "") + "><label for='calendar'>Enable Calendar & Schedule Access</label></div>";
    html += "<small style='margin-top:-5px;'>Queries OpenWebUI calendar when asked about schedule or upcoming events.</small>";
    html += "<div class='checkbox-group'><input type='checkbox' id='webSearch' name='webSearch' value='1'" + String(settings.enableWebSearch ? " checked" : "") + "><label for='webSearch'>Enable Live Web Search</label></div>";
    html += "<small style='margin-top:-5px;'>Performs live web searches via OpenWebUI for real-time information.</small>";
    html += "<label>Knowledge ID (RAG)</label><input type='text' name='knowledgeId' value='" + String(settings.knowledgeId) + "' placeholder='e.g., collection_id'>";
    html += "<small>OpenWebUI Collection/File ID to enable document context.</small>";
    html += "</div>";
    html += "</div>";

    // TAB: Audio & AEC
    html += "<div id='Audio' class='tab-content'>";
    html += "<div class='card'><h3>Microphone & Detection</h3>";
    html += "<label>Microphone Mode</label><select name='micMode'>";
    String micModes[] = {"Stereo (Beamforming)", "Left Channel Only", "Right Channel Only"};
    for (int i = 0; i < 3; i++) {
        html += "<option value='" + String(i) + "'" + (settings.micMode == i ? " selected" : "") + ">" + micModes[i] + "</option>";
    }
    html += "</select>";
    html += "<label>Wake Sensitivity (0.4 - 0.9)</label><input type='number' name='wakeThreshold' value='" + String(wakeThreshold) + "' step='0.05' min='0.4' max='0.9'>";
    html += "<label>Silence Threshold (300 - 2000)</label><input type='number' name='silenceThreshold' value='" + String(settings.silenceThreshold) + "' step='50' min='300' max='2000'>";
    html += "</div>";

    html += "<div class='card'><h3>Speaker & Resonance (HPF)</h3>";
    html += "<label>Speaker High-Pass Filter</label><select name='speakerHpf'>";
    int hpfOptions[] = {0, 160, 200, 240, 280, 320, 360, 400};
    String hpfLabels[] = {"Disabled (Full Range)", "160 Hz (Mild)", "200 Hz", "240 Hz", "280 Hz (Recommended)", "320 Hz", "360 Hz (Aggressive)", "400 Hz"};
    for (int i = 0; i < 8; i++) {
        html += "<option value='" + String(hpfOptions[i]) + "'" + (settings.speakerHpfCutoff == hpfOptions[i] ? " selected" : "") + ">" + hpfLabels[i] + "</option>";
    }
    if (settings.speakerHpfCutoff > 0) {
        bool found = false;
        for (int i = 0; i < 8; i++) if (settings.speakerHpfCutoff == hpfOptions[i]) found = true;
        if (!found) html += "<option value='" + String(settings.speakerHpfCutoff) + "' selected>Custom (" + String(settings.speakerHpfCutoff) + " Hz)</option>";
    }
    html += "</select>";
    html += "<small>Cuts inaudible sub-bass cone excursion to eliminate small-speaker buzz and distortion.</small>";
    html += "<div style='margin-top:12px;'><a href='/tune_resonance' class='btn' style='background:#00897b;display:inline-block;padding:8px 16px;color:#fff;text-decoration:none;border-radius:4px;'>Auto-Tune Speaker Resonance</a></div>";
    html += "</div>";

    html += "<div class='card'><h3>Acoustic Echo Cancellation (AEC)</h3>";
    html += "<div class='checkbox-group'><input type='checkbox' id='interrupt' name='interrupt' value='1'" + String(settings.enableInterrupt ? " checked" : "") + "><label for='interrupt'>Enable Voice Barge-in (Interrupt AI)</label></div>";
    html += "<small style='margin-top:-5px;'>Allows you to interrupt the AI by speaking over it.</small><br>";
    html += "<label>AEC Target Delay (Samples)</label><input type='number' name='aecDelay' value='" + String(settings.aecDelay) + "' step='10' min='0' max='3200'>";
    html += "<small>Echo alignment. Best tuned via /tune_aec command.</small>";
    html += "<label>AEC Attenuation (%) [Base at 50% Vol]</label><input type='number' name='aecAttenuation' value='" + String(settings.aecAttenuation) + "' min='0' max='100'>";
    html += "<small>Base attenuation at 50% vol (e.g. 55%). Automatically scales dynamically with volume slider.</small>";
    html += "<label>AEC Barge-in Cutoff [Base at 50% Vol]</label><input type='number' name='aecCutoff' value='" + String(settings.aecCutoff) + "' min='0' max='32767'>";
    html += "<small>Base cutoff threshold at 50% vol (e.g. 3000). Automatically scales dynamically with volume slider.</small>";
    html += "<label>AEC Interrupt Ignore (ms) [Base at 50% Vol]</label><input type='number' name='aecIgnore' value='" + String(settings.aecIgnore) + "' min='0' max='10000' step='100'>";
    html += "<small>Initial ignore window while Speex filter converges. Automatically scales dynamically with volume slider.</small>";
    html += "<div style='margin-top:14px;display:flex;gap:10px;flex-wrap:wrap;'>";
    html += "<a href='/tune_aec' class='btn' style='background:#1976d2;display:inline-block;padding:8px 16px;color:#fff;text-decoration:none;border-radius:4px;'>Auto-Tune AEC Delay</a>";
    html += "<a href='/tune_bargein' class='btn' style='background:#e65100;display:inline-block;padding:8px 16px;color:#fff;text-decoration:none;border-radius:4px;'>Auto-Calibrate Barge-In</a>";
    html += "<a href='/tune_all' class='btn' style='background:#2e7d32;display:inline-block;padding:8px 16px;color:#fff;text-decoration:none;border-radius:4px;'>Full Auto-Tune (All)</a>";
    html += "</div>";
    html += "</div>";
    html += "</div>";

    // TAB: System & Display
    html += "<div id='System' class='tab-content'>";
    html += "<div class='card'><h3>Display & Time</h3>";
    html += "<label>Timezone</label><select name='timezone'>";
    String tzs[] = {"UTC0", "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0", "MST7", "GMT0BST,M3.5.0/1,M10.5.0", "CET-1CEST,M3.5.0,M10.5.0/3", "JST-9", "CST-8", "AEST-10AEDT,M10.1.0,M4.1.0/3"};
    String names[] = {"UTC", "US Eastern", "US Central", "US Mountain", "US Pacific", "US Arizona", "London", "Paris/Berlin", "Tokyo", "Shanghai", "Sydney"};
    for (int i = 0; i < 11; i++) {
        html += "<option value='" + tzs[i] + "'" + (String(settings.timeZone) == tzs[i] ? " selected" : "") + ">" + names[i] + "</option>";
    }
    if (strlen(settings.timeZone) > 0 && html.indexOf("selected") == -1) {
         html += "<option value='" + String(settings.timeZone) + "' selected>Custom (" + String(settings.timeZone) + ")</option>";
    }
    html += "</select>";

    html += "<label>Clock Color</label><select name='clockColor'>";
    String colors[] = {"red", "green", "white"};
    String colorNames[] = {"Red", "Green", "White"};
    for (int i = 0; i < 3; i++) {
        html += "<option value='" + colors[i] + "'" + (String(settings.clockColor) == colors[i] ? " selected" : "") + ">" + colorNames[i] + "</option>";
    }
    html += "</select>";

    html += "<label>Screen Brightness (" + String(settings.brightness) + ")</label><input type='range' name='brightness' min='10' max='255' value='" + String(settings.brightness) + "' oninput='this.previousElementSibling.innerHTML=\"Screen Brightness (\" + this.value + \")\"'>";
    html += "<div class='checkbox-group' style='margin-top:15px;'><input type='checkbox' id='clockFormat12h' name='clockFormat12h' value='1'" + String(clockFormat12h ? " checked" : "") + "><label for='clockFormat12h'>Use 12-Hour Time Format</label></div>";
    html += "</div>";

    html += "<div class='card'><h3>Alarm Clock</h3>";
    html += "<div class='checkbox-group'><input type='checkbox' id='alarmEnabled' name='alarmEnabled' value='1'" + String(settings.alarmEnabled ? " checked" : "") + "><label for='alarmEnabled'>Enable Alarm</label></div>";
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", settings.alarmHour, settings.alarmMinute);
    html += "<label>Alarm Time</label><input type='time' name='alarmTime' value='" + String(timeBuf) + "'>";
    html += "</div>";

    html += "<div class='card'><h3>Weather (OpenWeatherMap)</h3>";
    html += "<label>API Key</label><input type='text' name='owKey' value='" + String(settings.openWeatherKey) + "' placeholder='Leave empty to disable'>";
    html += "<label>Location (City,CC)</label><input type='text' name='owLoc' value='" + String(settings.weatherLocation) + "'>";
    html += "</div>";

    html += "<div class='card'><h3>Advanced</h3>";
    html += "<div class='checkbox-group'><input type='checkbox' id='debugMode' name='debugMode' value='1'" + String(settings.debugMode ? " checked" : "") + "><label for='debugMode'>Enable Debug Logging (Serial 115200)</label></div>";
    html += "<div class='checkbox-group'><input type='checkbox' id='enableBME680' name='enableBME680' value='1'" + String(enableBME680 ? " checked" : "") + "><label for='enableBME680'>Enable BME680 Sensor (I2C)</label></div>";
    html += "<div class='checkbox-group'><input type='checkbox' id='ttsChunking' name='ttsChunking' value='1'" + String(enableTTSChunking ? " checked" : "") + "><label for='ttsChunking'>Enable TTS Sentence Chunking (Fast Audio Response)</label></div>";
    html += "<label>VOC Alarm Threshold (kOhms)</label><input type='number' name='vocAlarmThreshold' value='" + String(vocAlarmThreshold) + "' min='0' max='500'>";
    html += "<small>Triggers voice alarm if BME680 gas resistance drops below this value. Lower = worse air quality.</small>";
    html += "</div>";
    html += "</div>";

    html += "<div id='saveBtnContainer'><input type='submit' value='Save & Verify Configuration' class='btn'></div>";
    html += "</form>";

    // TAB: Diagnostics & Tools (Outside form)
    html += "<div id='Diagnostics' class='tab-content'>";
    
    size_t fsTotal = LittleFS.totalBytes();
    size_t fsUsed = LittleFS.usedBytes();
    size_t fsFree = (fsTotal > fsUsed) ? (fsTotal - fsUsed) : 0;
    int fsPercent = (fsTotal > 0) ? (int)((fsUsed * 100) / fsTotal) : 0;

    html += "<div class='card'><h3>Flash Storage (LittleFS)</h3>";
    html += "<p style='margin-bottom:8px;'><strong>Free Space:</strong> " + String(fsFree / 1024) + " KB (" + String((float)fsFree / (1024.0f * 1024.0f), 2) + " MB) / " + String(fsTotal / 1024) + " KB (" + String((float)fsTotal / (1024.0f * 1024.0f), 2) + " MB) &mdash; " + String(fsPercent) + "% Used</p>";
    html += "<div style='background:#e0e0e0;border-radius:4px;height:16px;overflow:hidden;margin-bottom:14px;'><div style='background:" + String(fsPercent > 85 ? "#d32f2f" : "#1976d2") + ";width:" + String(fsPercent) + "%;height:100%;'></div></div>";
    html += "<form action='/delete_wavs' method='POST'><input type='submit' value='Clean All Temporary / Diagnostic Audio Files' class='btn btn-red'></form>";
    html += "</div>";

    html += "<div class='card'><h3>Diagnostic Files</h3>";
    html += "<ul class='diag-list'>";
    
    if (LittleFS.exists("/aec_with.wav")) html += "<li><span>Audio with AEC</span> <a href='/download?file=aec_with.wav'>Download</a></li>";
    else html += "<li><span style='color:#999;'>Audio with AEC</span> <span style='color:#999;font-size:0.9em;'>Not found (Run /test_aec)</span></li>";
    
    if (LittleFS.exists("/aec_raw.wav")) html += "<li><span>Raw Mic Audio</span> <a href='/download?file=aec_raw.wav'>Download</a></li>";
    else html += "<li><span style='color:#999;'>Raw Mic Audio</span> <span style='color:#999;font-size:0.9em;'>Not found (Run /test_aec)</span></li>";

    if (LittleFS.exists("/mic_test.wav")) html += "<li><span>Mic Test Recording</span> <a href='/download?file=mic_test.wav'>Download</a></li>";
    else html += "<li><span style='color:#999;'>Mic Test Recording</span> <span style='color:#999;font-size:0.9em;'>Not found (Run /test_mic)</span></li>";
    
    if (LittleFS.exists("/harmonic_test.wav")) html += "<li><span>Harmonic Test</span> <a href='/download?file=harmonic_test.wav'>Download</a></li>";
    else html += "<li><span style='color:#999;'>Harmonic Test</span> <span style='color:#999;font-size:0.9em;'>Not found (Run /test_harmonic)</span></li>";
    
    if (LittleFS.exists("/aec_tune.log")) html += "<li><span>AEC Tuning Log</span> <div><a href='/tune_log'>Text</a> <a href='/tune_graph'>Graph</a> <a href='/download?file=aec_tune.log'>Download</a></div></li>";
    else html += "<li><span style='color:#999;'>AEC Tuning Log & Graph</span> <span style='color:#999;font-size:0.9em;'>Not found (Run /tune_aec)</span></li>";
    
    if (LittleFS.exists("/resonance_tune.log")) html += "<li><span>Resonance Tuning Log</span> <div><a href='/resonance_log'>View</a> <a href='/download?file=resonance_tune.log'>Download</a></div></li>";
    else html += "<li><span style='color:#999;'>Resonance Tuning Log</span> <span style='color:#999;font-size:0.9em;'>Not found (Run Auto-Tune Resonance)</span></li>";

    html += "</ul>";
    html += "</div>";

    html += "<div class='card'><h3>System Tools</h3>";
    html += "<a href='/update' class='btn' style='display:block;text-align:center;text-decoration:none;box-sizing:border-box;'>OTA Firmware/Filesystem Update</a>";
    html += "</div>";

    html += "</div>";

    html += "</div>";
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

    if (server.hasArg("ttsFormat")) {
        String fmt = server.arg("ttsFormat");
        fmt.trim();
        if (fmt == "wav") {
            strlcpy(settings.ttsFormat, "wav", sizeof(settings.ttsFormat));
        } else {
            strlcpy(settings.ttsFormat, "mp3", sizeof(settings.ttsFormat));
        }
        if (settings.debugMode) Serial.println("Web: TTS Format updated to: " + String(settings.ttsFormat));
    }

    // Model Selection
    if (server.hasArg("llmModel")) {
        String chosenModel = server.arg("llmModel");
        if (chosenModel == "__custom__" && server.hasArg("customModel") && server.arg("customModel").length() > 0) {
            chosenModel = server.arg("customModel");
        }
        chosenModel.trim();
        int openParen = chosenModel.lastIndexOf('(');
        int closeParen = chosenModel.lastIndexOf(')');
        if (openParen != -1 && closeParen != -1 && closeParen > openParen) {
            chosenModel = chosenModel.substring(openParen + 1, closeParen);
        }
        chosenModel.trim();
        if (chosenModel.length() > 0 && chosenModel != "__custom__") {
            strlcpy(settings.llmModel, chosenModel.c_str(), sizeof(settings.llmModel));
            if (settings.debugMode) Serial.println("Web: Model updated to: " + String(settings.llmModel));
        }
    }

    // Voice Selection
    if (server.hasArg("ttsVoice")) {
        String chosenVoice = server.arg("ttsVoice");
        if (chosenVoice == "__custom__" && server.hasArg("customVoice") && server.arg("customVoice").length() > 0) {
            chosenVoice = server.arg("customVoice");
        }
        chosenVoice.trim();
        if (chosenVoice.length() > 0 && chosenVoice != "__custom__") {
            ttsVoice = chosenVoice;
            adminPrefs.putString("voice", ttsVoice);
            if (settings.debugMode) Serial.println("Web: Voice updated to: " + ttsVoice);
        }
    }

    if (server.hasArg("wakeThreshold")) {
        float val = server.arg("wakeThreshold").toFloat();
        if (val >= 0.1 && val <= 1.0) {
            wakeThreshold = val;
            adminPrefs.putFloat("wake_thresh", wakeThreshold);
        }
    }
    
    if (server.hasArg("ttsSpeed")) {
        float val = server.arg("ttsSpeed").toFloat();
        if (val >= 0.5 && val <= 2.0) {
            ttsSpeed = val;
            adminPrefs.putFloat("tts_speed", ttsSpeed);
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
        if (val >= 0 && val <= 3200) {
            settings.aecDelay = val;
            setAecDelay(val);
        }
    }

    if (server.hasArg("aecAttenuation")) {
        int val = server.arg("aecAttenuation").toInt();
        if (val >= 0 && val <= 100) {
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

    if (server.hasArg("aecIgnore")) {
        int val = server.arg("aecIgnore").toInt();
        if (val >= 0 && val <= 10000) {
            settings.aecIgnore = val;
        }
    }

    if (server.hasArg("speakerHpf")) {
        int val = server.arg("speakerHpf").toInt();
        if (val >= 0 && val <= 1000) {
            settings.speakerHpfCutoff = val;
            speaker.setHpfCutoff(val);
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
    settings.enableMemory = server.hasArg("memory");
    settings.enableCalendar = server.hasArg("calendar");
    settings.enableWebSearch = server.hasArg("webSearch");
    settings.enableInterrupt = server.hasArg("interrupt");

    bool newEnableBME = server.hasArg("enableBME680");
    if (newEnableBME != enableBME680) {
        enableBME680 = newEnableBME;
        adminPrefs.putBool("en_bme", enableBME680);
        if (enableBME680) {
            if (bme.begin(0x77) || bme.begin(0x76)) {
                bme.setTemperatureOversampling(BME680_OS_8X);
                bme.setHumidityOversampling(BME680_OS_2X);
                bme.setPressureOversampling(BME680_OS_4X);
                bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
                bme.setGasHeater(320, 150);
                bmeReady = true;
            } else {
                bmeReady = false;
            }
        } else {
            bmeReady = false;
        }
    }

    bool newTTSChunking = server.hasArg("ttsChunking");
    if (newTTSChunking != enableTTSChunking) {
        enableTTSChunking = newTTSChunking;
        adminPrefs.putBool("tts_chunk", enableTTSChunking);
    }

    if (server.hasArg("vocAlarmThreshold")) {
        int val = server.arg("vocAlarmThreshold").toInt();
        vocAlarmThreshold = val;
        adminPrefs.putInt("voc_thresh", vocAlarmThreshold);
    }

    bool newClockFormat12h = server.hasArg("clockFormat12h");
    if (newClockFormat12h != clockFormat12h) {
        clockFormat12h = newClockFormat12h;
        adminPrefs.putBool("clk_12h", clockFormat12h);
    }

    settings.alarmEnabled = server.hasArg("alarmEnabled");
    if (server.hasArg("alarmTime")) {
        String at = server.arg("alarmTime");
        int colonIdx = at.indexOf(':');
        if (colonIdx != -1) {
            settings.alarmHour = at.substring(0, colonIdx).toInt();
            settings.alarmMinute = at.substring(colonIdx + 1).toInt();
        }
    }

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
    display.updateModelLabel(settings.llmModel);

    // Refresh weather immediately if configured
    if (newOwKey.length() > 0) getWeather();

    String models = llm.getModels(network);

    if (models.startsWith("Error")) {
        server.send(200, "text/plain", models);
    } else {
        modelOptions = models;
        updateVoiceList();
        display.updateModelLabel(settings.llmModel);
        server.send(200, "text/plain", "SUCCESS");
        if (settings.debugMode) Serial.println("Settings updated and verified via Web Interface");
    }
}

void onWiFiConfig(String ssid, String pass) {
    pendingSSID = ssid;
    pendingPass = pass;
    shouldConnectWiFi = true;
}

void updateVoiceList() {
    String newOptions = llm.getVoices(network);
    if (newOptions.startsWith("Error")) {
        if (settings.debugMode) Serial.println("Failed to fetch voices: " + newOptions);
        return;
    }

    if (newOptions.length() > 0 && newOptions != "No matching voices found") {
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
    bool isWavFormat = (strcmp(settings.ttsFormat, "wav") == 0);
    const char* ttsFile = isWavFormat ? "/aec_test_source.wav" : "/aec_test_source.mp3";

    // Check and log LittleFS storage health
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
    Serial.printf("\nLittleFS Storage: %u KB Free / %u KB Total (Used: %u KB)\n", 
                  (unsigned int)(freeBytes / 1024), 
                  (unsigned int)(totalBytes / 1024), 
                  (unsigned int)(usedBytes / 1024));

    // Free up space before downloading TTS and recording new WAVs
    if (LittleFS.exists("/aec_with.wav")) LittleFS.remove("/aec_with.wav");
    if (LittleFS.exists("/aec_raw.wav")) LittleFS.remove("/aec_raw.wav");
    if (LittleFS.exists("/aec_test_source.mp3")) LittleFS.remove("/aec_test_source.mp3");
    if (LittleFS.exists("/aec_test_source.wav")) LittleFS.remove("/aec_test_source.wav");

    // If free storage is tight (< 1 MB), clean up non-essential large test files
    usedBytes = LittleFS.usedBytes();
    freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
    if (freeBytes < 1024 * 1024) {
        Serial.println("LittleFS storage below 1MB. Cleaning up stale test audio files...");
        if (LittleFS.exists("/harmonic_test.wav")) LittleFS.remove("/harmonic_test.wav");
        if (LittleFS.exists("/sweep.wav")) LittleFS.remove("/sweep.wav");
        if (LittleFS.exists("/mic_test.wav")) LittleFS.remove("/mic_test.wav");
        if (LittleFS.exists("/tone_test.wav")) LittleFS.remove("/tone_test.wav");
        usedBytes = LittleFS.usedBytes();
        freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
        Serial.printf("Available space after cleanup: %u KB Free\n", (unsigned int)(freeBytes / 1024));
    }

    Serial.println("Downloading TTS for AEC Test...");
    display.showStatus("Downloading TTS...");
    lv_timer_handler();
    String testPhrase = "this is a test from the ai esp32 to see how speaker cancellation is functioning. Like and Subscribe to retro tech and electronics as well as classic wrench today. Beep Beep!";
    
    if (!network.isConnected()) {
        Serial.println("WiFi not connected. Cannot download TTS.");
        display.showStatus("Test Failed\nNo WiFi");
        delay(2000);
        display.showMainUI(ttsVoice, settings.volume, voiceOptions);
        return;
    }

    if (!llm.downloadTTS(testPhrase, network, ttsFile, ttsVoice)) {
        Serial.println("TTS Download failed. Aborting test.");
        display.showStatus("Test Failed\nTTS Error");
        delay(2000);
        display.showMainUI(ttsVoice, settings.volume, voiceOptions);
        return;
    }
    
    // --- Test 1: With AEC ---
    Serial.println("\n--- Test 1: With AEC (Cancellation Enabled) ---");
    Serial.println("Playing TTS & Recording (10s)...");
    display.showStatus("Test 1: AEC ON\nPlaying & Recording...");
    lv_timer_handler();
    setAecDebug(true); // Enable debug stats
    setInterruptMode(true);
    speaker.playSpeechFromFile(ttsFile);
    
    // Record with 0 threshold to ensure capture
    size_t wavSize1 = 0;
    uint8_t* wavData1 = speech.record(10000, &wavSize1, 0);
    
    speaker.stop();
    setInterruptMode(false);
    setAecDebug(false); // Disable debug stats
    
    if (wavData1 && wavSize1 > 0) {
        File f = LittleFS.open("/aec_with.wav", "w");
        if (f) {
            size_t written = f.write(wavData1, wavSize1);
            f.close();
            if (written == wavSize1) {
                Serial.printf("Saved /aec_with.wav (%u bytes)\n", (unsigned int)written);
            } else {
                Serial.printf("WARNING: LittleFS out of space! Only wrote %u / %u bytes to /aec_with.wav\n", 
                              (unsigned int)written, (unsigned int)wavSize1);
            }
        } else {
            Serial.println("ERROR: Failed to open /aec_with.wav for writing (LittleFS Full).");
        }

        // --- Analysis of AEC adaptation time ---
        if (wavSize1 > 44) {
            int16_t* samples = (int16_t*)(wavData1 + 44);
            int totalSamples = (wavSize1 - 44) / 2;
            int sampleRate = 16000;
            
            const int CHUNK_SAMPLES = 1600; // 100ms chunks
            const int QUIET_THRESHOLD = 300; // Average amplitude threshold for "quiet"
            const int CONFIRM_CHUNKS = 5; // Need 5 consecutive quiet chunks (500ms)
            
            int quiet_chunk_counter = 0;
            float quiet_time = -1.0f;

            for (int i = 0; i < totalSamples; i += CHUNK_SAMPLES) {
                long long chunk_sum = 0;
                int samples_in_chunk = (i + CHUNK_SAMPLES > totalSamples) ? (totalSamples - i) : CHUNK_SAMPLES;
                
                if (samples_in_chunk == 0) break;

                for (int j = 0; j < samples_in_chunk; j++) chunk_sum += abs(samples[i + j]);
                long avg_amp = chunk_sum / samples_in_chunk;

                if (avg_amp < QUIET_THRESHOLD) {
                    if (quiet_chunk_counter == 0) quiet_time = (float)i / sampleRate;
                    quiet_chunk_counter++;
                } else {
                    quiet_chunk_counter = 0;
                    quiet_time = -1.0f;
                }

                if (quiet_chunk_counter >= CONFIRM_CHUNKS) break;
            }
            
            if (quiet_time >= 0 && quiet_chunk_counter >= CONFIRM_CHUNKS) {
                Serial.printf("\n*** AEC Analysis: Audio quieted down after %.2f seconds. ***\n", quiet_time);
                settings.aecIgnore = (int)((quiet_time + 0.6f) * 1000.0f);
                if (settings.aecIgnore < 2000) settings.aecIgnore = 2000; // Minimum 2000ms floor
                
                // Calculate max residual amplitude after adaptation
                int startSample = (int)(quiet_time * sampleRate);
                long max_residual_amp = 0;
                for (int i = startSample; i < totalSamples; i++) {
                    long val = (abs(samples[i]) * (100 - settings.aecAttenuation)) / 100; // Apply attenuation to match detection logic
                    if (val > max_residual_amp) max_residual_amp = val;
                }
                
                int suggested_cutoff = (int)(max_residual_amp * 2.0f) + 2000; // Generous 2x + 2000 margin
                if (suggested_cutoff < 4500) suggested_cutoff = 4500; // Minimum 4500 floor to prevent self-interruption
                
                settings.aecCutoff = suggested_cutoff;
                settings.save();
                setAecCutoff(suggested_cutoff);
                Serial.printf("*** AEC Ignore Window automatically updated to %d ms. ***\n", settings.aecIgnore);
                Serial.printf("*** AEC Analysis: Max FILTERED amplitude (Attenuated Residual) is %ld. ***\n", max_residual_amp);
                Serial.printf("*** AEC Cutoff automatically updated to %d. ***\n", settings.aecCutoff);
            } else {
                Serial.println("\n*** AEC Analysis: Could not determine a stable quiet point. ***\n");
            }
        }
        free(wavData1); // Free after analysis
    } else {
        Serial.println("Recording 1 failed.");
    }

    delay(2000);

    // --- Test 2: Without AEC (Raw) ---
    Serial.println("\n--- Test 2: Without AEC (Raw Input) ---");
    display.showStatus("Test 2: AEC OFF\nPlaying & Recording...");
    lv_timer_handler();
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
            size_t written = f.write(wavData2, wavSize2);
            f.close();
            if (written == wavSize2) {
                Serial.printf("Saved /aec_raw.wav (%u bytes)\n", (unsigned int)written);
            } else {
                Serial.printf("WARNING: LittleFS out of space! Only wrote %u / %u bytes to /aec_raw.wav\n", 
                              (unsigned int)written, (unsigned int)wavSize2);
            }
        } else {
            Serial.println("ERROR: Failed to open /aec_raw.wav for writing (LittleFS Full).");
        }
        
        // --- Analysis of Raw Audio ---
        if (wavSize2 > 44) {
            long max_raw_amp = 0;
            int16_t* samples = (int16_t*)(wavData2 + 44);
            int sampleCount = (wavSize2 - 44) / 2;
            for(int i = 0; i < sampleCount; i++) {
                long val = abs(samples[i]);
                if (val > max_raw_amp) max_raw_amp = val;
            }
            Serial.printf("\n*** AEC Analysis: Max UNFILTERED amplitude is %ld. ***\n", max_raw_amp);
            
            if (max_raw_amp > 20000) {
                Serial.println("\n!!! WARNING: Speaker is too loud! Microphone is likely clipping. !!!");
                Serial.println("!!! AEC cannot mathematically cancel distorted audio. !!!");
                Serial.println("!!! Automatically lowering device volume to a safe level (12/21)... !!!");
                onVolumeChange(12); // Apply new volume and save to NVRAM
                display.showMainUI(ttsVoice, settings.volume, voiceOptions); // Update the slider on the screen
                Serial.println("!!! Please run /test_aec again at this new volume for optimal tuning. !!!\n");
            }
        }
        free(wavData2);
    } else {
        Serial.println("Recording 2 failed.");
    }

    // --- Playback Results with Timeouts & UI Refresh ---
    if (LittleFS.exists("/aec_with.wav")) {
        Serial.println("\n--- Playback 1/2: With AEC ---");
        display.showStatus("Playing Result (1/2)\nWith AEC...");
        lv_timer_handler();
        speaker.playSpeechFromFile("/aec_with.wav");
        unsigned long pbStart = millis();
        // Allow up to 12s timeout for a 10s audio clip
        while (speaker.isRunning() && (millis() - pbStart < 12000)) {
            delay(30);
        }
        speaker.stop();
    } else {
        Serial.println("/aec_with.wav not found or empty, skipping playback 1.");
    }
    
    delay(1000);
    
    if (LittleFS.exists("/aec_raw.wav")) {
        Serial.println("\n--- Playback 2/2: Without AEC ---");
        display.showStatus("Playing Result (2/2)\nWithout AEC...");
        lv_timer_handler();
        speaker.playSpeechFromFile("/aec_raw.wav");
        unsigned long pbStart = millis();
        // Allow up to 12s timeout for a 10s audio clip
        while (speaker.isRunning() && (millis() - pbStart < 12000)) {
            delay(30);
        }
        speaker.stop();
    } else {
        Serial.println("/aec_raw.wav not found or empty, skipping playback 2.");
    }
    
    Serial.println("\nAEC Test Complete.");
    display.showStatus("AEC Test Complete\nCheck Web UI");
    delay(3000);
    display.showMainUI(ttsVoice, settings.volume, voiceOptions);
}

void testHarmonic() {
    Serial.println("\n--- Harmonic Test ---");
    display.showStatus("Generating Sweep...");
    lv_timer_handler();

    const char* sweepFile = "/sweep.wav";
    
    // Generate the 40s sine sweep file
    File f = LittleFS.open(sweepFile, "w");
    if (f) {
        uint32_t sampleRate = 16000;
        uint32_t numSamples = 20 * sampleRate;
        uint32_t dataSize = numSamples * 2;

        // WAV Header
        uint8_t header[44] = {
            'R', 'I', 'F', 'F',
            0, 0, 0, 0, // size to be filled
            'W', 'A', 'V', 'E',
            'f', 'm', 't', ' ',
            16, 0, 0, 0, // fmt chunk size
            1, 0, // format PCM
            1, 0, // channels
            (uint8_t)(sampleRate & 0xFF), (uint8_t)((sampleRate >> 8) & 0xFF), 0, 0,
            (uint8_t)((sampleRate * 2) & 0xFF), (uint8_t)(((sampleRate * 2) >> 8) & 0xFF), 0, 0,
            2, 0, // block align
            16, 0, // bits per sample
            'd', 'a', 't', 'a',
            (uint8_t)(dataSize & 0xFF), (uint8_t)((dataSize >> 8) & 0xFF), (uint8_t)((dataSize >> 16) & 0xFF), (uint8_t)((dataSize >> 24) & 0xFF)
        };
        uint32_t overallSize = 36 + dataSize;
        header[4] = overallSize & 0xFF; header[5] = (overallSize >> 8) & 0xFF;
        header[6] = (overallSize >> 16) & 0xFF; header[7] = (overallSize >> 24) & 0xFF;
        f.write(header, 44);

        int16_t buffer[1000];
        float phase = 0;
        for (int s = 0; s < 2; s++) { // 2 phases of 10 seconds each (1 loop)
            bool up = (s % 2 == 0);
            float startF = up ? 20.0f : 15000.0f;
            float endF = up ? 15000.0f : 20.0f;
            for (int i = 0; i < 10 * sampleRate; i++) {
                float t = (float)i / sampleRate;
                float currentF = startF + (endF - startF) * (t / 10.0f);
                phase += 2.0f * M_PI * currentF / sampleRate;
                if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
                buffer[i % 1000] = (int16_t)(sin(phase) * 8000.0f); // Reduced amplitude to 25% to lower the volume
                if ((i + 1) % 1000 == 0) f.write((uint8_t*)buffer, 2000);
            }
        }
        f.close();
        Serial.println("Sweep generated.");
    } else {
        Serial.println("Failed to open /sweep.wav for writing.");
        display.showStatus("FS Error");
        return;
    }

    display.showStatus("Playing & Recording (20s)...");
    lv_timer_handler();
    Serial.println("Playing Sweep & Recording (20s)...");

    setAecBypass(true); // Record raw mic input to avoid Speex modifying the high frequencies
    speaker.playSpeechFromFile(sweepFile);
    size_t wavSize = 0;
    uint8_t* wavData = speech.record(20000, &wavSize, 0); // Hard lock for 20 seconds
    speaker.stop();
    setAecBypass(false);
    
    if (LittleFS.exists(sweepFile)) LittleFS.remove(sweepFile); // Erase the 1.2MB generator file to save space

    if (wavData && wavSize > 0) {
        display.showStatus("Saving...");
        lv_timer_handler();
        File out = LittleFS.open("/harmonic_test.wav", "w");
        if (out) { out.write(wavData, wavSize); out.close(); Serial.println("Saved /harmonic_test.wav"); }
        free(wavData);
    } else { Serial.println("Recording failed."); }

    display.showStatus("Test Complete\nCheck Downloads Page");
    Serial.println("Harmonic test complete. Pull harmonic_test.wav from the web downloads page.");
    delay(3000);
    display.showMainUI(ttsVoice, settings.volume, voiceOptions);
}

// Single-frequency DFT power calculation using the Goertzel algorithm
static float computeGoertzelPower(const int16_t* samples, size_t numSamples, float targetFreq, float sampleRate) {
    if (numSamples == 0 || sampleRate == 0 || targetFreq <= 0 || targetFreq >= sampleRate * 0.5f) return 0.0f;
    float k = (numSamples * targetFreq) / sampleRate;
    float omega = (2.0f * (float)M_PI * k) / (float)numSamples;
    float coeff = 2.0f * cosf(omega);
    float q0 = 0.0f, q1 = 0.0f, q2 = 0.0f;
    for (size_t i = 0; i < numSamples; i++) {
        q0 = coeff * q1 - q2 + (float)samples[i];
        q2 = q1;
        q1 = q0;
    }
    float p = (q1 * q1 + q2 * q2 - q1 * q2 * coeff) / ((float)numSamples * (float)numSamples);
    return (p > 0.0f) ? p : 0.0f;
}

void tuneResonance(bool returnToMainUI) {
    bool prevDebug = settings.debugMode;
    settings.debugMode = false;
    Serial.println("\n--- Starting Speaker Resonance Auto-Tuning ---");
    display.showStatus("Auto-Tuning Resonance...\nPlease remain silent");
    lv_timer_handler();

    File logFile = LittleFS.open("/resonance_tune.log", "w");
    auto logPrintln = [&](const String& msg) {
        if (logFile) { logFile.println(msg); logFile.flush(); }
        Serial.println(msg);
    };
    auto logPrintf = [&](const char* format, ...) {
        char buf[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        if (logFile) { logFile.print(buf); logFile.flush(); }
        Serial.print(buf);
    };

    logPrintln("\n==========================================");
    logPrintln("  Speaker Resonance & THD Calibration Log ");
    logPrintln("==========================================");

    // Temporarily bypass HPF during measurement to capture the speaker's raw acoustic profile
    speaker.setHpfCutoff(0);

    const int testFreqs[] = {120, 160, 200, 240, 280, 320, 360, 400};
    const int numFreqs = 8;
    float thdResults[numFreqs] = {0.0f};

    const uint32_t sampleRate = 16000;
    const float durationSec = 0.8f;
    const uint32_t numSamples = (uint32_t)(durationSec * sampleRate);
    const uint32_t dataSize = numSamples * 2;
    const char* toneFile = "/tone_test.wav";

    for (int idx = 0; idx < numFreqs; idx++) {
        int freq = testFreqs[idx];
        logPrintf("\nTesting %d Hz... ", freq);

        String uiMsg = "Testing Resonance:\n" + String(freq) + " Hz (" + String(idx + 1) + "/" + String(numFreqs) + ")";
        display.showStatus(uiMsg.c_str());
        lv_timer_handler();

        // 1. Generate pure sine tone
        if (LittleFS.exists(toneFile)) LittleFS.remove(toneFile);
        File f = LittleFS.open(toneFile, "w");
        if (!f) {
            logPrintln("Failed to create tone file.");
            break;
        }

        uint8_t header[44] = {
            'R', 'I', 'F', 'F',
            0, 0, 0, 0,
            'W', 'A', 'V', 'E',
            'f', 'm', 't', ' ',
            16, 0, 0, 0,
            1, 0,
            1, 0,
            (uint8_t)(sampleRate & 0xFF), (uint8_t)((sampleRate >> 8) & 0xFF), 0, 0,
            (uint8_t)((sampleRate * 2) & 0xFF), (uint8_t)(((sampleRate * 2) >> 8) & 0xFF), 0, 0,
            2, 0,
            16, 0,
            'd', 'a', 't', 'a',
            (uint8_t)(dataSize & 0xFF), (uint8_t)((dataSize >> 8) & 0xFF), (uint8_t)((dataSize >> 16) & 0xFF), (uint8_t)((dataSize >> 24) & 0xFF)
        };
        uint32_t overallSize = 36 + dataSize;
        header[4] = overallSize & 0xFF; header[5] = (overallSize >> 8) & 0xFF;
        header[6] = (overallSize >> 16) & 0xFF; header[7] = (overallSize >> 24) & 0xFF;
        f.write(header, 44);

        int16_t buffer[500];
        float phase = 0.0f;
        float phaseInc = 2.0f * (float)M_PI * (float)freq / (float)sampleRate;
        for (uint32_t s = 0; s < numSamples; s++) {
            phase += phaseInc;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            // Smooth attack and release envelope to prevent click transients
            float env = 1.0f;
            if (s < 800) env = (float)s / 800.0f;
            else if (s > numSamples - 800) env = (float)(numSamples - s) / 800.0f;
            buffer[s % 500] = (int16_t)(sinf(phase) * 10000.0f * env);
            if ((s + 1) % 500 == 0) f.write((uint8_t*)buffer, 1000);
        }
        f.close();

        // 2. Play tone and record with mic
        setAecBypass(true); // Raw mic signal
        speaker.playSpeechFromFile(toneFile);

        size_t wavSize = 0;
        uint8_t* wavData = speech.record(800, &wavSize, 0);
        speaker.stop();
        setAecBypass(false);

        // 3. Analyze Harmonic Distortion
        if (wavData && wavSize > 44) {
            int16_t* samples = (int16_t*)(wavData + 44);
            int totalRecSamples = (wavSize - 44) / 2;

            // Discard startup transient: analyze samples from index 3200 (0.2s) onwards
            int startIdx = 3200;
            int analyzeCount = totalRecSamples - startIdx;
            if (analyzeCount > 6400) analyzeCount = 6400; // 0.4s

            if (analyzeCount > 1000) {
                float p0 = computeGoertzelPower(samples + startIdx, analyzeCount, (float)freq, (float)sampleRate);
                float p2 = computeGoertzelPower(samples + startIdx, analyzeCount, (float)(2 * freq), (float)sampleRate);
                float p3 = computeGoertzelPower(samples + startIdx, analyzeCount, (float)(3 * freq), (float)sampleRate);
                float p4 = computeGoertzelPower(samples + startIdx, analyzeCount, (float)(4 * freq), (float)sampleRate);

                float fundAmp = sqrtf(p0);
                float harmAmp = sqrtf(p2 + p3 + p4);
                float thd = (fundAmp > 50.0f) ? (harmAmp / fundAmp) * 100.0f : 100.0f;
                thdResults[idx] = thd;

                const char* quality = (thd > 35.0f) ? "HEAVY OVERDRIVE / RATTLING" : (thd > 18.0f ? "MODERATE BUZZ" : "CLEAN");
                logPrintf("Fund: %.0f, Harmonics: %.0f -> THD: %.1f%% [%s]\n", fundAmp, harmAmp, thd, quality);
            }
            free(wavData);
        } else {
            logPrintln("Recording failed.");
            thdResults[idx] = 100.0f;
        }

        delay(250); // Pause between tones
    }

    if (LittleFS.exists(toneFile)) LittleFS.remove(toneFile);

    // 4. Determine Recommended High-Pass Cutoff
    // Scan from highest frequency downwards to find the highest frequency where the speaker rattled (THD > 18%)
    int recommendedCutoff = 0; // Default off if all clean
    for (int i = numFreqs - 1; i >= 0; i--) {
        if (thdResults[i] > 18.0f) {
            // Cutoff is set to the first clean frequency above the worst resonance
            if (i + 1 < numFreqs) {
                recommendedCutoff = testFreqs[i + 1];
            } else {
                recommendedCutoff = testFreqs[i];
            }
            break;
        }
    }

    // Typical miniature speaker safety floor: if resonance was detected below 200 Hz, set to 240 Hz
    if (recommendedCutoff > 0 && recommendedCutoff < 240) {
        recommendedCutoff = 240;
    }

    logPrintln("\n--- Calibration Results ---");
    logPrintf("Recommended Speaker High-Pass Cutoff: %d Hz\n", recommendedCutoff);
    if (logFile) logFile.close();
    settings.debugMode = prevDebug;

    // 5. Apply and save to NVS
    settings.speakerHpfCutoff = recommendedCutoff;
    settings.save();
    speaker.setHpfCutoff(recommendedCutoff);

    String doneMsg = "Tuning Complete!\nHPF: " + (recommendedCutoff > 0 ? (String(recommendedCutoff) + " Hz") : String("Disabled"));
    display.showStatus(doneMsg.c_str());
    lv_timer_handler();
    delay(2000);
    if (returnToMainUI) {
        display.showMainUI(ttsVoice, settings.volume, voiceOptions);
    }
}

void tuneAllAudio() {
    Serial.println("\n==========================================");
    Serial.println("  Starting Full Audio Pipeline Auto-Tune   ");
    Serial.println("  Step 1/3: Speaker Resonance & THD Cutoff ");
    Serial.println("  Step 2/3: Acoustic Echo Cancellation     ");
    Serial.println("  Step 3/3: Voice Barge-In Calibration     ");
    Serial.println("==========================================");

    // Step 1: Tune speaker resonance to eliminate non-linear rattling
    tuneResonance(false);

    delay(1000); // Brief pause before starting speech calibration

    // Step 2: Now that speaker is rattle-free, tune AEC delay
    tuneAEC();

    delay(1000);

    // Step 3: Calibrate barge-in cutoff and ignore window
    calibrateBargeIn();
}

void tuneAEC() {
    bool prevDebug = settings.debugMode;
    settings.debugMode = false; // Suppress verbose prints from other classes during tuning
    
    File logFile = LittleFS.open("/aec_tune.log", "w");
    auto logPrintln = [&](const String& msg) {
        if (logFile) { logFile.println(msg); logFile.flush(); }
    };
    auto logPrintf = [&](const char *format, ...) {
        char loc_buf[256];
        va_list arg;
        va_start(arg, format);
        vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
        va_end(arg);
        if (logFile) { logFile.print(loc_buf); logFile.flush(); }
    };

    Serial.println("\n--- Starting AEC Auto-Tuning ---");
    Serial.println("Tuning in progress. Detailed output is being saved to /aec_tune.log...");
    logPrintln("\n--- Starting AEC Auto-Tuning ---");
    showAecTuningUI(); // Load graphical UI
    
    logPrintln("Downloading TTS for AEC Tuning...");
    bool isWavFormat = (strcmp(settings.ttsFormat, "wav") == 0);
    const char* ttsFile = isWavFormat ? "/aec_tune.wav" : "/aec_tune.mp3";
    if (LittleFS.exists("/aec_tune.mp3")) LittleFS.remove("/aec_tune.mp3");
    if (LittleFS.exists("/aec_tune.wav")) LittleFS.remove("/aec_tune.wav");

    // Diagnostic phrase for AEC calibration (fits within 3.5s timing window)
    String testPhrase = "Testing AEC Please remain silent Pop Check One Two";
    
    if (!network.isConnected()) { 
        logPrintln("WiFi not connected."); 
        display.showStatus("Tuning Failed\nNo WiFi");
        if (logFile) logFile.close(); 
        settings.debugMode = prevDebug;
        return; 
    }
    if (!llm.downloadTTS(testPhrase, network, ttsFile, ttsVoice)) { 
        logPrintln("TTS Download failed."); 
        display.showStatus("Tuning Failed\nTTS Error");
        if (logFile) logFile.close(); 
        settings.debugMode = prevDebug;
        return; 
    }

    // Test range: 10ms to 70ms (160 to 1120 samples at 16kHz)
    int testDelays[] = {160, 320, 480, 640, 800, 960, 1120}; 
    long avgScores[7] = {0};
    int bestDelay = 640;
    long bestScore = 99999999; // Lower amplitude is better

    logPrintln("Please remain completely silent for about a minute...");
    
    int totalSteps = 9 * 3 + 1; // 7 coarse + 2 fine + 1 profiling pass
    int currentStep = 0;

    // Define the test sequence as a reusable lambda function
    auto runDelayTest = [&](int d) -> long {
        setAecDelay(d);
        
        long totalScore = 0;
        int validRuns = 0;
        String msStr = (d % 16 == 0) ? String(d / 16) : String(d / 16.0f, 1);
        logPrintf("\nTesting Delay: %d samples (~%s ms)\n", d, msStr.c_str());

        for (int run = 0; run < 3; run++) {
            logPrintf("  Run %d/3... ", run + 1);
            
            int percent = (currentStep * 100) / totalSteps;
            String uiMsg = "Testing Delay: " + msStr + "ms (" + String(run+1) + "/3)";
            updateAecTuningUI(percent, uiMsg.c_str());
            
            setInterruptMode(true);
            speaker.playSpeechFromFile(ttsFile);
            
            // Discard first 1.5 seconds of audio to let the AEC filter adapt to the room
            size_t discardSize = 0;
            uint8_t* discardData = speech.record(1500, &discardSize, 0);
            if (discardData) free(discardData);

            // Record 2.0 seconds for actual measurement
            size_t wavSize = 0;
            uint8_t* wavData = speech.record(2000, &wavSize, 0);
            
            // Allow remaining audio of the test phrase to finish cleanly before stopping
            unsigned long waitStart = millis();
            while (speaker.isRunning() && (millis() - waitStart < 2500)) {
                delay(20);
            }
            speaker.stop();
            setInterruptMode(false);
            
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
                logPrintf("Score: %ld\n", score);
            } else {
                logPrintln("Failed to record.");
            }

            currentStep++;
            
            percent = (currentStep * 100) / totalSteps;
            String scoreStr = (score != 99999999) ? String(score) : "Failed";
            uiMsg = "Testing Delay: " + msStr + "ms (" + String(run+1) + "/3)\nScore: " + scoreStr + "\nWaiting to settle...";
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

    logPrintln("\n--- Coarse Tuning Results ---");
    for (int i = 0; i < 7; i++) {
        String msStr = (testDelays[i] % 16 == 0) ? String(testDelays[i] / 16) : String(testDelays[i] / 16.0f, 1);
        logPrintf("Delay %d samples (~%s ms) -> Average Score: %ld\n", testDelays[i], msStr.c_str(), avgScores[i]);
        if (avgScores[i] < bestScore) {
            bestScore = avgScores[i];
            bestDelay = testDelays[i];
        }
    }

    // 2. Fine Tuning Pass (+/- 80 samples = +/- 5ms)
    int fineDelays[] = {bestDelay - 80, bestDelay + 80};
    if (fineDelays[0] < 0) fineDelays[0] = 0; // Prevent negative delays
    
    long fineScores[2] = {0};
    String bestMsStr = (bestDelay % 16 == 0) ? String(bestDelay / 16) : String(bestDelay / 16.0f, 1);
    logPrintf("\nBest Coarse Delay: %d samples (~%s ms). Starting Fine-Tuning (+/- 80 samples)...\n", bestDelay, bestMsStr.c_str());

    for (int i = 0; i < 2; i++) {
        fineScores[i] = runDelayTest(fineDelays[i]);
    }

    logPrintln("\n--- Fine Tuning Results ---");
    for (int i = 0; i < 2; i++) {
        String msStr = (fineDelays[i] % 16 == 0) ? String(fineDelays[i] / 16) : String(fineDelays[i] / 16.0f, 1);
        logPrintf("Delay %d samples (~%s ms) -> Average Score: %ld\n", fineDelays[i], msStr.c_str(), fineScores[i]);
        if (fineScores[i] < bestScore) {
            bestScore = fineScores[i];
            bestDelay = fineDelays[i];
        }
    }
    
    // Export results to JSON for the web graph
    JsonDocument doc;
    for(int i = 0; i < 7; i++) { 
        doc["coarseDelays"].add(testDelays[i]); 
        doc["coarseScores"].add(avgScores[i]); 
    }
    for(int i = 0; i < 2; i++) { 
        doc["fineDelays"].add(fineDelays[i]); 
        doc["fineScores"].add(fineScores[i]); 
    }
    File jsonFile = LittleFS.open("/aec_tune.json", "w");
    if (jsonFile) { serializeJson(doc, jsonFile); jsonFile.close(); }
    
    // 3. Attenuation & Cutoff Profiling Pass
    logPrintln("\n--- Final Acoustic Profiling ---");
    
    int percent = (currentStep * 100) / totalSteps;
    updateAecTuningUI(percent, "Profiling Room Acoustics...");
    
    setInterruptMode(true);
    speaker.playSpeechFromFile(ttsFile);
    
    size_t discardSize = 0;
    uint8_t* discardData = speech.record(1500, &discardSize, 0);
    if (discardData) free(discardData);

    // Record for 4.0 seconds to capture the loud dynamic parts (Pop, Check, etc.)
    size_t wavSize = 0;
    uint8_t* wavData = speech.record(4000, &wavSize, 0);
    
    // Allow remaining audio of the test phrase to finish cleanly before stopping
    unsigned long waitStart = millis();
    while (speaker.isRunning() && (millis() - waitStart < 2500)) {
        delay(20);
    }
    speaker.stop();
    setInterruptMode(false);
    
    long max_raw_residual = 0;
    if (wavData && wavSize > 44) {
        int16_t* samples = (int16_t*)(wavData + 44);
        int sampleCount = (wavSize - 44) / 2;
        for(int j=0; j<sampleCount; j++) {
            long val = abs(samples[j]); // UNATTENUATED
            if (val > max_raw_residual) max_raw_residual = val;
        }
        free(wavData);
        logPrintf("Max Unattenuated Residual: %ld\n", max_raw_residual);
    } else {
        logPrintln("Failed to record profiling pass.");
    }
    
    currentStep++;

    // Mathematically determine best attenuation percentage
    int bestAtten = 70; // High margin default
    if (max_raw_residual > 900) {
        int calcAtten = 100 - (90000 / max_raw_residual);
        if (calcAtten > bestAtten) bestAtten = calcAtten;
        if (bestAtten > 80) bestAtten = 80;
    }
    
    logPrintf("Calculated Optimal Attenuation: %d%%\n", bestAtten);
    
    long final_max_amp = (max_raw_residual * (100 - bestAtten)) / 100;
    logPrintf("Simulated Attenuated Max Amp: %ld\n", final_max_amp);
    
    int bestCutoff = (int)(final_max_amp * 2.0f) + 2000; // Generous 2x + 2000 margin
    if (bestCutoff < 4500) bestCutoff = 4500; // Hard minimum floor so AI never triggers itself

    Serial.println("\n----------------------------------");
    Serial.printf("Ultimate Best AEC Delay: %d samples\n", bestDelay);
    Serial.printf("Ultimate Best Attenuation: %d%%\n", bestAtten);
    Serial.printf("Ultimate Best Cutoff: %d\n", bestCutoff);
    Serial.println("Saved to NVRAM and applied!");

    logPrintln("----------------------------------");
    logPrintf("Ultimate Best AEC Delay: %d samples\n", bestDelay);
    logPrintf("Ultimate Best Attenuation: %d%%\n", bestAtten);
    logPrintf("Ultimate Best Cutoff: %d\n", bestCutoff);
    logPrintln("Saved to NVRAM and applied!");
    
    settings.aecDelay = bestDelay;
    settings.aecAttenuation = bestAtten;
    settings.aecCutoff = bestCutoff;
    settings.save();
    
    setAecDelay(bestDelay); // Lock it in
    setAecAttenuation(bestAtten);
    setAecCutoff(bestCutoff);
    
    if (logFile) logFile.close();
    settings.debugMode = prevDebug; // Restore debug mode
    
    // Display Final Results
    String resultMsg = "Tuning Complete!\nBest Delay: " + String(bestDelay) + " samples\nAtten: " + String(bestAtten) + "% | Cutoff: " + String(bestCutoff);
    updateAecTuningUI(100, resultMsg.c_str());
    delay(5000);
    delay(15000); // Wait 15 seconds so the user can read the results before it clears
    display.showMainUI(ttsVoice, settings.volume, voiceOptions); // Return to default UI
}

void calibrateBargeIn(bool returnToMainUI) {
    bool prevDebug = settings.debugMode;
    settings.debugMode = false;
    bool prevInterrupt = settings.enableInterrupt;
    settings.enableInterrupt = true; // Ensure barge-in evaluation is active in detectWakeWord

    Serial.println("\n==============================================");
    Serial.println("  Starting Voice Barge-In Auto-Calibration    ");
    Serial.println("==============================================");
    display.showStatus("Barge-In Auto-Cal\nPreparing TTS...");
    lv_timer_handler();

    bool isWavFormat = (strcmp(settings.ttsFormat, "wav") == 0);
    const char* ttsFile = isWavFormat ? "/bargein_test.wav" : "/bargein_test.mp3";

    // Clean up older temporary test files if free space is tight
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
    if (freeBytes < 200 * 1024) {
        if (LittleFS.exists("/aec_with.wav")) LittleFS.remove("/aec_with.wav");
        if (LittleFS.exists("/aec_raw.wav")) LittleFS.remove("/aec_raw.wav");
    }

    String testPhrase = "This is an automated acoustic calibration test to measure my echo cancellation and ensure I do not interrupt myself while speaking.";
    
    // Check if test audio exists, else download
    if (!LittleFS.exists(ttsFile)) {
        if (!network.isConnected()) {
            if (LittleFS.exists("/aec_test_source.mp3")) ttsFile = "/aec_test_source.mp3";
            else if (LittleFS.exists("/aec_tune.mp3")) ttsFile = "/aec_tune.mp3";
            else {
                Serial.println("Error: No WiFi and no local test audio file found. Cannot calibrate barge-in.");
                display.showStatus("Cal Failed\nNo Audio File");
                delay(2000);
                if (returnToMainUI) display.showMainUI(ttsVoice, settings.volume, voiceOptions);
                settings.debugMode = prevDebug;
                settings.enableInterrupt = prevInterrupt;
                return;
            }
        } else {
            Serial.println("Downloading TTS calibration audio...");
            display.showStatus("Downloading TTS...");
            lv_timer_handler();
            if (!llm.downloadTTS(testPhrase, network, ttsFile, ttsVoice)) {
                Serial.println("TTS Download failed. Checking fallback files...");
                if (LittleFS.exists("/aec_test_source.mp3")) ttsFile = "/aec_test_source.mp3";
                else if (LittleFS.exists("/aec_tune.mp3")) ttsFile = "/aec_tune.mp3";
                else {
                    Serial.println("Error: TTS download failed and no fallback audio found.");
                    display.showStatus("Cal Failed\nTTS Error");
                    delay(2000);
                    if (returnToMainUI) display.showMainUI(ttsVoice, settings.volume, voiceOptions);
                    settings.debugMode = prevDebug;
                    settings.enableInterrupt = prevInterrupt;
                    return;
                }
            }
        }
    }

    // -------------------------------------------------------------
    // PASS 1: Profiling (Measuring adaptation time and harmonic residue)
    // -------------------------------------------------------------
    Serial.println("\n--- Pass 1: Acoustic Profiling & Adaptation Measurement ---");
    Serial.println("Playing calibration speech and analyzing residual levels...");
    display.showStatus("Pass 1: Profiling\nPlease be quiet...");
    lv_timer_handler();

    // Set cutoff extremely high so detectWakeWord does NOT trigger early during profiling
    setAecCutoff(60000);
    setAecAttenuation(0); // Measure raw residue
    flushAecBuffer();

    setInterruptMode(true);
    speaker.playSpeechFromFile(ttsFile);
    unsigned long startTime = millis();
    g_audioPlaybackStart = startTime;

    int overall_max_burst = 0;
    int steady_max_burst = 0;
    int quiet_consecutive_chunks = 0;
    unsigned long converge_time_ms = 1200; // default fallback
    bool converged = false;

    while (speaker.isRunning()) {
        speech.detectWakeWord(wakeThreshold); // fast-path drains ringbuffer & calculates burst_max_level
        int32_t burst = getLastAecBurstLevel();
        unsigned long elapsed = millis() - startTime;

        if (burst > overall_max_burst) overall_max_burst = burst;

        // Detect adaptation convergence: when burst drops below 450 for >= 5 consecutive frames (~100ms)
        if (!converged && elapsed >= 400) {
            if (burst < 450) {
                quiet_consecutive_chunks++;
                if (quiet_consecutive_chunks >= 5) {
                    converged = true;
                    converge_time_ms = elapsed - 100;
                }
            } else {
                quiet_consecutive_chunks = 0;
            }
        }

        // Measure steady-state residual harmonics after filter has adapted (after max(converge_time, 1000ms))
        if (elapsed >= 1000) {
            if (burst > steady_max_burst) steady_max_burst = burst;
        }

        delay(20);
    }

    speaker.stop();
    setInterruptMode(false);
    flushAecBuffer();

    Serial.printf("\n[Pass 1 Results]\n");
    Serial.printf("  Filter Adaptation Time : %lu ms %s\n", converge_time_ms, converged ? "(measured)" : "(default)");
    Serial.printf("  Initial Peak Burst     : %d\n", overall_max_burst);
    Serial.printf("  Steady Harmonic Residue: %d\n", steady_max_burst);

    // Compute candidate parameters with realistic safety margins
    int candidateIgnore = (int)converge_time_ms + 200; // +200ms safety buffer
    if (candidateIgnore < 1000) candidateIgnore = 1000; // minimum floor 1000ms
    if (candidateIgnore > 2000) candidateIgnore = 2000;

    int candidateAtten = 55; // 55% attenuation during voice playback
    if (steady_max_burst > 2500) candidateAtten = 60;

    int attenuated_peak = (steady_max_burst * (100 - candidateAtten)) / 100;
    int candidateCutoff = (int)(attenuated_peak * 1.8f) + 800;
    if (candidateCutoff < 2800) candidateCutoff = 2800;
    if (candidateCutoff > 4000) candidateCutoff = 4000;

    Serial.printf("  Candidate Ignore : %d ms\n", candidateIgnore);
    Serial.printf("  Candidate Atten  : %d%%\n", candidateAtten);
    Serial.printf("  Candidate Cutoff : %d\n", candidateCutoff);

    settings.aecIgnore = candidateIgnore;
    settings.aecAttenuation = candidateAtten;
    settings.aecCutoff = candidateCutoff;
    setAecAttenuation(candidateAtten);
    setAecCutoff(candidateCutoff);

    // -------------------------------------------------------------
    // PASS 2: Closed-Loop Live Verification ("Talking to itself and adjusting")
    // -------------------------------------------------------------
    Serial.println("\n--- Pass 2: Active Self-Trigger Closed-Loop Verification ---");
    Serial.println("Testing whether device triggers its own interrupt threshold...");

    bool passSuccess = false;
    for (int pass = 1; pass <= 4; pass++) {
        Serial.printf("\n[Verification Run %d/4] Testing with Ignore: %d ms, Atten: %d%%, Cutoff: %d...\n",
                      pass, settings.aecIgnore, settings.aecAttenuation, settings.aecCutoff);
        display.showStatus(("Verify (" + String(pass) + "/4)\nTesting Audio...").c_str());
        lv_timer_handler();
        delay(600);

        flushAecBuffer();
        setInterruptMode(true);
        speaker.playSpeechFromFile(ttsFile);
        unsigned long testStart = millis();
        g_audioPlaybackStart = testStart;

        bool selfTriggered = false;
        unsigned long selfTriggerTime = 0;
        int32_t triggerLevel = 0;
        int32_t triggerEval = 0;
        int32_t max_eval_seen = 0;

        while (speaker.isRunning()) {
            bool triggered = speech.detectWakeWord(wakeThreshold);
            unsigned long elapsed = millis() - g_audioPlaybackStart;
            int32_t cur_eval = getLastAecEvalLevel();
            if (cur_eval > max_eval_seen) max_eval_seen = cur_eval;

            if (triggered && (elapsed > getEffectiveAecIgnore())) {
                selfTriggered = true;
                selfTriggerTime = elapsed;
                triggerLevel = getLastAecBurstLevel();
                triggerEval = cur_eval;
                speaker.stop();
                break;
            }
            delay(20);
        }

        speaker.stop();
        setInterruptMode(false);
        flushAecBuffer();

        if (selfTriggered) {
            Serial.printf(">>> SELF-TRIGGER DETECTED at %lu ms! (Raw: %d, Atten: %d, Cutoff: %d, Ignore: %lu ms) <<<\n",
                          selfTriggerTime, triggerLevel, triggerEval, settings.aecCutoff, getEffectiveAecIgnore());

            // Dynamically adjust parameters:
            if (selfTriggerTime < (getEffectiveAecIgnore() + 300)) {
                settings.aecIgnore += 200;
                Serial.printf(">>> Adjusting: Increased aecIgnore to %d ms\n", settings.aecIgnore);
            }
            int boost = (triggerEval > settings.aecCutoff) ? (triggerEval - settings.aecCutoff + 500) : 500;
            settings.aecCutoff += boost;
            if (settings.aecAttenuation < 65) settings.aecAttenuation += 5;
            setAecCutoff(settings.aecCutoff);
            setAecAttenuation(settings.aecAttenuation);
            Serial.printf(">>> Adjusting: New Cutoff = %d, New Atten = %d%%. Re-running test...\n",
                          settings.aecCutoff, settings.aecAttenuation);
        } else {
            // Apply balanced safety margin above peak level seen during test
            int safe_cutoff = (max_eval_seen * 2) + 600;
            if (safe_cutoff < 2800) safe_cutoff = 2800;
            if (safe_cutoff > 4000) safe_cutoff = 4000;
            if (settings.aecCutoff < safe_cutoff) {
                settings.aecCutoff = safe_cutoff;
                setAecCutoff(settings.aecCutoff);
                Serial.printf(">>> Adding extra safety headroom: Cutoff locked at %d (Max eval seen: %d) <<<\n",
                              settings.aecCutoff, max_eval_seen);
            }
            Serial.println(">>> PASSED: Full playback completed with 0 false self-interrupts! <<<");
            passSuccess = true;
            break;
        }
    }

    if (!passSuccess) {
        Serial.println("\nWarning: Could not completely eliminate false interrupts in 4 passes.");
        Serial.println("Applying robust parameters.");
        if (settings.aecIgnore < 1200) settings.aecIgnore = 1200;
        if (settings.aecCutoff < 3200) settings.aecCutoff = 3200;
        if (settings.aecAttenuation < 60) settings.aecAttenuation = 60;
        setAecCutoff(settings.aecCutoff);
        setAecAttenuation(settings.aecAttenuation);
    }

    // Save proven settings to NVRAM
    settings.save();

    Serial.println("\n==============================================");
    Serial.println("  Barge-In Auto-Calibration Complete!         ");
    Serial.printf ("  Final AEC Ignore     : %d ms\n", settings.aecIgnore);
    Serial.printf ("  Final AEC Attenuation: %d%%\n", settings.aecAttenuation);
    Serial.printf ("  Final Barge-In Cutoff: %d\n", settings.aecCutoff);
    Serial.println("  Saved permanently to NVRAM.");
    Serial.println("==============================================\n");

    display.showStatus(("Calibrated!\nCutoff: " + String(settings.aecCutoff) + "\nIgn: " + String(settings.aecIgnore) + "ms").c_str());
    lv_timer_handler();
    delay(3000);

    if (returnToMainUI) {
        display.showMainUI(ttsVoice, settings.volume, voiceOptions);
    }

    settings.debugMode = prevDebug;
    settings.enableInterrupt = prevInterrupt;
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    
    // Handle backspace/delete
    if (c == '\b' || c == 0x7F) {
      if (inputBuffer.length() > 0) {
        inputBuffer.remove(inputBuffer.length() - 1);
        Serial.print("\b \b"); // Visually erase character from terminal
      }
      continue;
    }
    
    Serial.print(c); // Local echo

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
          Serial.println("/test_mic       - Record 5s clip to test mic");
          Serial.println("/mic_mode <0|1|2> - Mic Channel: 1=Left, 2=Right, 0=Auto");
          Serial.println("/mic_shift <n>  - I2S Bit Shift gain (8-16, default: 12)");
          Serial.println("/wake_thresh <n>- Set wake word threshold (e.g. 0.40)");
          Serial.println("/reset_pk       - Reset peak stats session tracking");
          Serial.println("/test_hal       - Run on-device wake word verification test");
          Serial.println("/test_aec       - Run AEC diagnostics");
          Serial.println("/test_harmonic  - Run 20s sweep and record to check mic harmonics");
          Serial.println("/tune_resonance - Auto-tune speaker high-pass filter for resonance/THD");
          Serial.println("/tune_aec       - Auto-tune AEC delay alignment");
          Serial.println("/tune_bargein   - Auto-calibrate voice barge-in (self-trigger test)");
          Serial.println("/tune_all       - Full calibration: Resonance, AEC delay, and Barge-In");
          Serial.println("/fs             - Show LittleFS storage space");
          Serial.println("/delete_wavs    - Delete all diagnostic WAVs to free flash space");
          Serial.println("/debug_aec      - Toggle AEC debug stats");
          Serial.println("/aec_delay <n>  - Set AEC delay samples");
          Serial.println("/aec_gain <n>   - Set AEC gain multiplier");
          Serial.println("/aec_invert     - Toggle AEC phase inversion");
          Serial.println("<text>          - Send prompt to AI");
          Serial.println("/factory_reset  - Erase all settings and reboot");
          Serial.println("/reboot         - Restart the device");
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
          Serial.printf("AEC Atten:  %d%%\n", settings.aecAttenuation);
          Serial.printf("AEC Cutoff: %d\n", settings.aecCutoff);
          Serial.printf("Debug Mode: %s\n", settings.debugMode ? "ON" : "OFF");
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
          if (d >= 0) {
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
        } else if (inputBuffer == "/test_hal") {
          speech.runSelfTest();
        } else if (inputBuffer.startsWith("/mic_mode ")) {
          int m = inputBuffer.substring(10).toInt();
          if (m >= 0 && m <= 2) {
              settings.micMode = m;
              settings.save();
              Serial.printf("Microphone Mode set to %d (%s)\n", m, m == 1 ? "Left Channel Only" : (m == 2 ? "Right Channel Only" : "Auto Stronger Channel"));
          } else {
              Serial.println("Usage: /mic_mode <0=Auto, 1=Left, 2=Right>");
          }
        } else if (inputBuffer.startsWith("/mic_shift ")) {
          int s = inputBuffer.substring(11).toInt();
          if (s >= 8 && s <= 16) {
              speech.setBitShift(s);
              Serial.printf("I2S bit shift set to %d\n", s);
          } else {
              Serial.println("Usage: /mic_shift <8..16> (11=+30dB, 12=+24dB, 13=+18dB, 14=+12dB)");
          }
        } else if (inputBuffer.startsWith("/wake_thresh ")) {
          float t = inputBuffer.substring(13).toFloat();
          if (t >= 0.05f && t <= 1.0f) {
              wakeThreshold = t;
              adminPrefs.putFloat("wake_thresh", wakeThreshold);
              Serial.printf("Wake word threshold set to %.2f\n", wakeThreshold);
          } else {
              Serial.println("Usage: /wake_thresh <0.05 - 1.00>");
          }
        } else if (inputBuffer == "/reset_pk" || inputBuffer == "/reset_stats") {
          speech.resetStats();
          Serial.println("microWakeWord peak statistics reset.");
        } else if (inputBuffer == "/test_aec") {
          testAEC();
        } else if (inputBuffer == "/test_harmonic") {
          testHarmonic();
        } else if (inputBuffer == "/tune_resonance") {
          tuneResonance();
        } else if (inputBuffer == "/tune_aec") {
          tuneAEC();
        } else if (inputBuffer == "/tune_bargein" || inputBuffer == "/test_bargein") {
          calibrateBargeIn();
        } else if (inputBuffer == "/tune_all" || inputBuffer == "/calibrate_audio") {
          tuneAllAudio();
        } else if (inputBuffer == "/fs" || inputBuffer == "/space") {
          size_t total = LittleFS.totalBytes();
          size_t used = LittleFS.usedBytes();
          size_t freeSpace = (total > used) ? (total - used) : 0;
          Serial.println("\n--- LittleFS Storage ---");
          Serial.printf("Total: %u KB (%.2f MB)\n", (unsigned int)(total / 1024), (float)total / (1024.0f * 1024.0f));
          Serial.printf("Used:  %u KB (%.2f MB)\n", (unsigned int)(used / 1024), (float)used / (1024.0f * 1024.0f));
          Serial.printf("Free:  %u KB (%.2f MB)\n", (unsigned int)(freeSpace / 1024), (float)freeSpace / (1024.0f * 1024.0f));
          Serial.println("\nFiles on LittleFS:");
          File root = LittleFS.open("/");
          if (root && root.isDirectory()) {
              File f = root.openNextFile();
              int count = 0;
              while (f) {
                  Serial.printf("  %-25s %8u bytes (%6.1f KB)\n", f.name(), (unsigned int)f.size(), (float)f.size() / 1024.0f);
                  count++;
                  f = root.openNextFile();
              }
              if (count == 0) Serial.println("  (No files found)");
          }
          Serial.println("------------------------\n");
        } else if (inputBuffer == "/delete_wavs" || inputBuffer == "/clean") {
          const char* filesToDelete[] = {
              "/aec_with.wav", "/aec_raw.wav", "/mic_test.wav", "/harmonic_test.wav",
              "/sweep.wav", "/tone_test.wav", "/aec_tune.wav", "/aec_tune.mp3",
              "/aec_test_source.wav", "/aec_test_source.mp3",
              "/speech.wav", "/speech0.wav", "/speech1.wav",
              "/speech.mp3", "/speech0.mp3", "/speech1.mp3",
              "/aec_tune.log", "/aec_tune.json", "/resonance_tune.log"
          };
          int deleted = 0;
          for (size_t i = 0; i < sizeof(filesToDelete) / sizeof(filesToDelete[0]); i++) {
              if (LittleFS.exists(filesToDelete[i])) {
                  LittleFS.remove(filesToDelete[i]);
                  deleted++;
              }
          }
          size_t total = LittleFS.totalBytes();
          size_t used = LittleFS.usedBytes();
          size_t freeSpace = (total > used) ? (total - used) : 0;
          Serial.printf("Cleaned %d temporary audio/diagnostic files. Free space: %u KB (%.2f MB)\n", 
                        deleted, (unsigned int)(freeSpace / 1024), (float)freeSpace / (1024.0f * 1024.0f));
        } else if (inputBuffer == "/format_fs") {
          Serial.println("Formatting LittleFS filesystem... Please wait...");
          LittleFS.format();
          Serial.println("Format complete. Rebooting...");
          delay(1000);
          ESP.restart();
        } else if (inputBuffer == "/reboot") {
          Serial.println("Rebooting now...");
          delay(1000);
          ESP.restart();
        } else if (inputBuffer.startsWith("/say ")) {
          String textToSay = inputBuffer.substring(5);
          textToSay.trim();
          if (textToSay.length() > 0) {
            if (settings.debugMode) Serial.println("Direct TTS: " + textToSay);
            speaker.stop();
            isSpeaking = false;
            display.showMainUI(ttsVoice, settings.volume, voiceOptions);
            display.showStatus("Direct TTS...");
            lv_timer_handler(); // Force the screen to update before blocking
            
            if (!enableTTSChunking) {
                const char* speechFile = getSpeechFilename();
                if (llm.downloadTTS(textToSay, network, speechFile, ttsVoice)) {
                    speaker.playSpeechFromFile(speechFile);
                    if (!isSpeaking) g_audioPlaybackStart = millis();
                    isSpeaking = true;
                    lv_timer_handler();
                    if (settings.enableInterrupt) setInterruptMode(true);
                    while(speaker.isRunning()) {
                        if (settings.enableInterrupt) {
                            if (checkPlaybackBargeIn()) {
                                break;
                            }
                            delay(20);
                        } else {
                            flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
                            delay(50);
                        }
                    }
                    if (settings.enableInterrupt) setInterruptMode(false);
                    isSpeaking = false;
                    display.showStatus("Ready");
                } else {
                    display.showResponse("TTS Failed");
                }
            } else {
                bool interrupted = false;
                String sentence = "";
                for (size_t i = 0; i < textToSay.length(); i++) {
                    char c = textToSay[i];
                    sentence += c;
                    bool isPunctuation = (c == '.' || c == '?' || c == '!' || c == '\n');
                    bool isPause = (c == ',' || c == ';' || c == ':');
                    if (isPunctuation || (isPause && sentence.length() > 60)) {
                        sentence.trim();
                        bool hasWords = false;
                        for (size_t j = 0; j < sentence.length(); j++) {
                            if (isalnum(sentence[j])) { hasWords = true; break; }
                        }
                        if (hasWords) {
                            if (!playChunkedTTS(sentence)) {
                                interrupted = true;
                                break;
                            }
                        }
                        sentence = "";
                    }
                }
                if (!interrupted) {
                    sentence.trim();
                    bool hasWords = false;
                    for (size_t j = 0; j < sentence.length(); j++) {
                        if (isalnum(sentence[j])) { hasWords = true; break; }
                    }
                    if (hasWords) {
                        playChunkedTTS(sentence);
                    }
                }
                
                // Block and wait for chunked audio to finish to prevent instant UI overwrites
                if (settings.enableInterrupt) setInterruptMode(true);
                while(speaker.isRunning()) {
                    if (settings.enableInterrupt) {
                        if (checkPlaybackBargeIn()) {
                            break;
                        }
                        delay(20);
                    } else {
                        flushAecBuffer();
                        delay(50);
                    }
                }
                if (settings.enableInterrupt) setInterruptMode(false);
                isSpeaking = false;
                display.showStatus("Ready");
            }
          }
        } else if (inputBuffer == "/factory_reset") {
          performFactoryReset();
          Serial.println("Rebooting now...");
          delay(1000);
          ESP.restart();
        } else if (inputBuffer == "/test_mic") {
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
          
          if (!enableTTSChunking && answer != "Interrupted by user") {
              // Download TTS to file, then play
              const char* speechFile = getSpeechFilename();
              if (llm.downloadTTS(answer, network, speechFile, ttsVoice)) {
                  speaker.playSpeechFromFile(speechFile);
                  if (!isSpeaking) g_audioPlaybackStart = millis();
                  isSpeaking = true;
                  lv_timer_handler();
                  if (settings.enableInterrupt) setInterruptMode(true);
                  while(speaker.isRunning()) {
                      if (settings.enableInterrupt) {
                          if (checkPlaybackBargeIn()) {
                              break;
                          }
                          delay(20);
                      } else {
                          flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
                          delay(50);
                      }
                  }
                  if (settings.enableInterrupt) setInterruptMode(false);
                  isSpeaking = false;
              } else {
                  display.showResponse("TTS Failed");
              }
          } else {
              bool interrupted = false;
              if (answer != "Interrupted by user" && answer.length() > 0) {
                  String sentence = "";
                  for (size_t i = 0; i < answer.length(); i++) {
                      char c = answer[i];
                      sentence += c;
                      bool isPunctuation = (c == '.' || c == '?' || c == '!' || c == '\n');
                      bool isPause = (c == ',' || c == ';' || c == ':');
                      if (isPunctuation || (isPause && sentence.length() > 60)) {
                          sentence.trim();
                          bool hasWords = false;
                          for (size_t j = 0; j < sentence.length(); j++) {
                              if (isalnum(sentence[j])) { hasWords = true; break; }
                          }
                          if (hasWords) {
                              if (!playChunkedTTS(sentence)) {
                                  interrupted = true;
                                  break;
                              }
                          }
                          sentence = "";
                      }
                  }
                  if (!interrupted) {
                      sentence.trim();
                      bool hasWords = false;
                      for (size_t j = 0; j < sentence.length(); j++) {
                          if (isalnum(sentence[j])) { hasWords = true; break; }
                      }
                      if (hasWords) {
                          playChunkedTTS(sentence);
                      }
                  }
              }
              // Wait for chunked TTS to finish playing
              if (settings.enableInterrupt) setInterruptMode(true);
              while(speaker.isRunning()) {
                  if (settings.enableInterrupt) {
                      if (checkPlaybackBargeIn()) {
                          break;
                      }
                      delay(20);
                  } else {
                      flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
                      delay(50);
                  }
              }
              if (settings.enableInterrupt) setInterruptMode(false);
              isSpeaking = false;
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

void performFactoryReset() {
    Serial.println("\n--- FACTORY RESET ---");
    Serial.println("Resetting all settings to factory defaults...");
    
    // Reset SettingsManager fields to default by manually setting them
    // This mirrors the defaults set in the SettingsManager constructor
    strlcpy(settings.wifiSSID, "YOUR_WIFI_SSID", sizeof(settings.wifiSSID));
    strlcpy(settings.wifiPass, "YOUR_WIFI_PASSWORD", sizeof(settings.wifiPass));
    strlcpy(settings.apiUrl, "http://your-api-endpoint/api/chat/completions", sizeof(settings.apiUrl));
    strlcpy(settings.apiKey, "your_api_key_here", sizeof(settings.apiKey));
    strlcpy(settings.llmModel, "llama3.2:3b", sizeof(settings.llmModel));
    settings.volume = 21;
    settings.brightness = 255;
    settings.calibration = {0, 0, 0, 0, false};
    strlcpy(settings.timeZone, "UTC0", sizeof(settings.timeZone));
    strlcpy(settings.clockColor, "red", sizeof(settings.clockColor));
    settings.silenceThreshold = 800;
    strlcpy(settings.openWeatherKey, "", sizeof(settings.openWeatherKey));
    strlcpy(settings.weatherLocation, "New York,US", sizeof(settings.weatherLocation));
    settings.micMode = 1;
    settings.debugMode = false;
    settings.inputBalance = 0;
    strlcpy(settings.systemPrompt, "You are a helpful AI assistant running on an ESP32-S3.", sizeof(settings.systemPrompt));
    settings.ttsProvider = 0;
    strlcpy(settings.ttsUrl, "", sizeof(settings.ttsUrl));
    settings.enableWebSearch = false;
    settings.enableCalendar = true;
    settings.enableMemory = false;
    settings.enableInterrupt = true;
    settings.aecDelay = 640;
    settings.aecAttenuation = 55;
    settings.aecCutoff = 3000;
    settings.aecIgnore = 1000;
    settings.alarmEnabled = false;
    settings.alarmHour = 7;
    settings.alarmMinute = 0;
    settings.speakerHpfCutoff = 0;
    speaker.setHpfCutoff(0);
    strlcpy(settings.knowledgeId, "", sizeof(settings.knowledgeId));
    settings.save();

    // Reset admin preferences (password, voice, wake threshold)
    adminPrefs.clear();
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
      performFactoryReset();
      Serial.println("All settings have been reset to factory defaults.");
      // Wait for button release to avoid accidental double-triggering
      while(digitalRead(0) == LOW) delay(10);
  }

  // Apply loaded settings
  network.setCredentials(settings.wifiSSID, settings.wifiPass);
  // Migration: Fix API URL for OpenWebUI compatibility
  if (String(settings.apiUrl).endsWith("/v1/chat/completions") && String(settings.apiUrl).indexOf("api.openai.com") == -1) {
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
  display.showStatus("Powered by microWakeWord");
  display.showStatus("UI by LVGL");

  // Admin Password Check
  adminPassword = adminPrefs.getString("pass", "");
  ttsVoice = adminPrefs.getString("voice", "alloy");
  wakeThreshold = adminPrefs.getFloat("wake_thresh", 0.5);
  if (wakeThreshold > 0.65f) {
      wakeThreshold = 0.5f;
      adminPrefs.putFloat("wake_thresh", 0.5f);
  }
  // Auto-heal stale or out-of-bounds AEC NVRAM values from previous builds
  if (settings.aecIgnore < 800 || settings.aecIgnore > 2500) {
      settings.aecIgnore = 1000;
      settings.save();
  }
  if (settings.aecCutoff < 2600 || settings.aecCutoff > 4500) {
      settings.aecCutoff = 3000;
      settings.save();
  }
  if (settings.aecAttenuation < 45 || settings.aecAttenuation > 70) {
      settings.aecAttenuation = 55;
      settings.save();
  }
  if (!settings.enableInterrupt) {
      settings.enableInterrupt = true;
      settings.save();
  }
  ttsSpeed = adminPrefs.getFloat("tts_speed", 0.8);
  setInputBalance(settings.inputBalance);
  vocAlarmThreshold = adminPrefs.getInt("voc_thresh", 25);
  clockFormat12h = adminPrefs.getBool("clk_12h", false);
  
  enableTTSChunking = adminPrefs.getBool("tts_chunk", false);
  enableBME680 = adminPrefs.getBool("en_bme", false);
  
  // Initialize I2C on pins 8 (SDA) and 3 (SCL)
  Wire.begin(8, 3);
  Serial.println("[I2C] Scanning bus on SDA=GPIO8, SCL=GPIO3...");
  byte count = 0;
  for (byte i = 8; i < 120; i++) {
      Wire.beginTransmission(i);
      if (Wire.endTransmission() == 0) {
          Serial.printf("[I2C] Found device at address 0x%02X\n", i);
          count++;
      }
  }
  if (count == 0) {
      Serial.println("[I2C] No I2C devices found! Check wiring, 3.3V power, and CS pin.");
  }

  if (enableBME680) {
      Serial.println("[BME680] Initializing sensor...");
      if (bme.begin(0x77)) {
          Serial.println("[BME680] Connected successfully at address 0x77!");
          bme.setTemperatureOversampling(BME680_OS_8X);
          bme.setHumidityOversampling(BME680_OS_2X);
          bme.setPressureOversampling(BME680_OS_4X);
          bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
          bme.setGasHeater(320, 150);
          bmeReady = true;
      } else if (bme.begin(0x76)) {
          Serial.println("[BME680] Connected successfully at address 0x76!");
          bme.setTemperatureOversampling(BME680_OS_8X);
          bme.setHumidityOversampling(BME680_OS_2X);
          bme.setPressureOversampling(BME680_OS_4X);
          bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
          bme.setGasHeater(320, 150);
          bmeReady = true;
      } else {
          Serial.println("[BME680] ERROR: Sensor failed to respond at 0x77 or 0x76!");
          bmeReady = false;
      }
  } else {
      Serial.println("[BME680] Sensor disabled in web settings.");
  }
  
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

  // Serve the static logo image directly from LittleFS
  server.serveStatic("/logo.png", LittleFS, "/logo.png");

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

  server.on("/downloads", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      // The downloads page is now fully integrated into the main page's "Diagnostics" tab.
      // We leave this route active as a redirect to gracefully handle legacy links.
      server.sendHeader("Location", "/");
      server.send(303);
  });

  server.on("/tune_resonance", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      server.sendHeader("Location", "/#audio");
      server.send(303);
      g_runResonanceTuneUI = true;
  });

  server.on("/tune_aec", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      server.sendHeader("Location", "/#audio");
      server.send(303);
      g_runAecTuneUI = true;
  });

  server.on("/tune_bargein", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      server.sendHeader("Location", "/#audio");
      server.send(303);
      g_runBargeInTuneUI = true;
  });

  server.on("/tune_all", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      server.sendHeader("Location", "/#audio");
      server.send(303);
      g_runTuneAllUI = true;
  });

  server.on("/delete_wavs", HTTP_POST, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      const char* filesToDelete[] = {
          "/aec_with.wav", "/aec_raw.wav", "/mic_test.wav", "/harmonic_test.wav",
          "/sweep.wav", "/tone_test.wav", "/aec_tune.wav", "/aec_tune.mp3",
          "/aec_test_source.wav", "/aec_test_source.mp3",
          "/speech.wav", "/speech0.wav", "/speech1.wav",
          "/speech.mp3", "/speech0.mp3", "/speech1.mp3",
          "/aec_tune.log", "/aec_tune.json", "/resonance_tune.log"
      };
      for (size_t i = 0; i < sizeof(filesToDelete) / sizeof(filesToDelete[0]); i++) {
          if (LittleFS.exists(filesToDelete[i])) LittleFS.remove(filesToDelete[i]);
      }
      server.sendHeader("Location", "/#Diagnostics");
      server.send(303);
  });

  server.on("/download", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      if (server.hasArg("file")) {
          String filename = server.arg("file");
          if (!filename.startsWith("/")) filename = "/" + filename;
          // Restrict downloads specifically to known safe files
          if (filename == "/aec_with.wav" || filename == "/aec_raw.wav" || filename == "/mic_test.wav" || filename == "/harmonic_test.wav" || filename == "/aec_tune.log" || filename == "/resonance_tune.log") {
              if (LittleFS.exists(filename)) {
                  File f = LittleFS.open(filename, "r");
                  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename.substring(1) + "\"");
                  if (filename.endsWith(".log")) {
                      server.streamFile(f, "text/plain");
                  } else {
                      server.streamFile(f, "audio/wav");
                  }
                  f.close();
                  return;
              }
          }
      }
      server.send(404, "text/plain", "File not found or access denied.");
  });

  server.on("/resonance_log", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      if (LittleFS.exists("/resonance_tune.log")) {
          File f = LittleFS.open("/resonance_tune.log", "r");
          server.streamFile(f, "text/plain");
          f.close();
      } else {
          server.send(404, "text/plain", "Resonance tuning log not found. Please run the Auto-Tune Speaker Resonance first.");
      }
  });

  server.on("/tune_log", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      if (LittleFS.exists("/aec_tune.log")) {
          File f = LittleFS.open("/aec_tune.log", "r");
          server.streamFile(f, "text/plain");
          f.close();
      } else {
          server.send(404, "text/plain", "Tuning log not found. Please run the AEC Auto-Tune first.");
      }
  });

  server.on("/tune_data", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      if (LittleFS.exists("/aec_tune.json")) {
          File f = LittleFS.open("/aec_tune.json", "r");
          server.streamFile(f, "application/json");
          f.close();
      } else {
          server.send(404, "application/json", "{}");
      }
  });

  server.on("/tune_graph", HTTP_GET, []() {
      if (!server.authenticate("admin", adminPassword.c_str())) return server.requestAuthentication();
      String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
      html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
      html += "<style>body{font-family:sans-serif;padding:20px;} canvas{max-width:100%;background:#f9f9f9;border-radius:8px;padding:10px;}</style></head><body>";
      html += "<h2>AEC Delay Tuning Graph</h2>";
      html += "<canvas id='myChart'></canvas>";
      html += "<br><br><a href='/' style='color:#008CBA;text-decoration:none;'>&larr; Back to Configuration</a>";
      html += "<script>";
      html += "fetch('/tune_data').then(r=>r.json()).then(data=>{";
      html += "  if(!data.coarseDelays) { alert('No tuning data found'); return; }";
      html += "  let pts = [];";
      html += "  for(let i=0; i<data.coarseDelays.length; i++) pts.push({x: data.coarseDelays[i], y: data.coarseScores[i]});";
      html += "  for(let i=0; i<data.fineDelays.length; i++) pts.push({x: data.fineDelays[i], y: data.fineScores[i]});";
      html += "  pts.sort((a,b) => a.x - b.x);";
      html += "  let labels = pts.map(p => p.x + ' (' + (p.x/16) + 'ms)');";
      html += "  let scores = pts.map(p => p.y);";
      html += "  new Chart(document.getElementById('myChart'), {";
      html += "    type: 'line',";
      html += "    data: { labels: labels, datasets: [{ label: 'Amplitude Score (Lower is Better)', data: scores, borderColor: '#008CBA', backgroundColor: '#008CBA', tension: 0.3, fill: false, pointRadius: 5, pointHoverRadius: 8 }] },";
      html += "    options: { responsive: true, scales: { y: { beginAtZero: true, title: { display: true, text: 'Average Amplitude Score' } }, x: { title: { display: true, text: 'AEC Target Delay (Samples)' } } } }";
      html += "  });";
      html += "}).catch(e=>alert('Error loading graph data'));";
      html += "</script></body></html>";
      server.send(200, "text/html", html);
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
  Serial.printf("[Barge-in] Status: %s | Base Cutoff: %d | Base Atten: %d%% | Base Ignore: %dms (Volume: %d/21 -> Eff Cutoff: %d, Eff Atten: %d%%, Eff Ignore: %lums)\n",
                settings.enableInterrupt ? "ENABLED" : "DISABLED",
                settings.aecCutoff, settings.aecAttenuation, settings.aecIgnore,
                settings.volume, getEffectiveAecCutoff(), getEffectiveAecAttenuation(), getEffectiveAecIgnore());
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
    
    // Update VU Meter (Logic and throttling safely handled inside DisplayManager)
    display.updateAudioVUMeter(0, 0);
  }

  if (g_runAecTuneUI) {
      g_runAecTuneUI = false;
      tuneAEC();
  }
  
  if (g_runAecTestUI) {
      g_runAecTestUI = false;
      testAEC();
  }
  
  if (g_runHarmonicTestUI) {
      g_runHarmonicTestUI = false;
      testHarmonic();
  }

  if (g_runResonanceTuneUI) {
      g_runResonanceTuneUI = false;
      tuneResonance();
  }

  if (g_runTuneAllUI) {
      g_runTuneAllUI = false;
      tuneAllAudio();
  }

  if (g_runBargeInTuneUI) {
      g_runBargeInTuneUI = false;
      calibrateBargeIn();
  }

  // Hourly Weather Update
  if (!isSpeaking && strlen(settings.openWeatherKey) > 0 && network.isConnected()) {
      if (millis() - lastWeatherUpdate > 3600000) { // 1 hour
          getWeather();
          lastWeatherUpdate = millis();
      }
  }

  static unsigned long lastBmeRead = 0;
  if (!isSpeaking && bmeReady && (millis() - lastBmeRead > 60000 || (lastBmeRead == 0 && millis() > 3000))) {
      lastBmeRead = millis();
      if (bme.performReading()) {
          g_currentVocKOhms = bme.gas_resistance / 1000.0;
          Serial.printf("[BME680] Gas: %.1f kOhms | Temp: %.1f C | Hum: %.1f%% | Pres: %.1f hPa\n", 
                        g_currentVocKOhms, bme.temperature, bme.humidity, bme.pressure / 100.0);
          
          // Voice Alarm Logic
          if (g_currentVocKOhms > 0 && g_currentVocKOhms < vocAlarmThreshold) {
              if (millis() - lastVocAlarmTime > 900000 || lastVocAlarmTime == 0) { // 15 minute cooldown between alarms
                  if (!isSpeaking && !isProcessing && !isWebServerActive && !g_isSubMenuActive) {
                      lastVocAlarmTime = millis(); // Only trigger cooldown if alarm actually plays!
                      if (settings.debugMode) Serial.println("VOC Alarm Triggered!");
                      String alarmText = "Warning. Indoor air quality has dropped below the configured threshold.";
                      const char* vocFile = (strcmp(settings.ttsFormat, "wav") == 0) ? "/voc_alarm.wav" : "/voc_alarm.mp3";
                      if (!LittleFS.exists(vocFile)) {
                          llm.downloadTTS(alarmText, network, vocFile, ttsVoice);
                      }
                      speaker.playSpeechFromFile(vocFile);
                      if (!isSpeaking) g_audioPlaybackStart = millis();
                      isSpeaking = true;
                      if (settings.enableInterrupt) setInterruptMode(true);
                      while(speaker.isRunning()) {
                          if (settings.enableInterrupt) {
                              if (checkPlaybackBargeIn()) {
                                  if (settings.debugMode) Serial.println("VOC Alarm interrupted!");
                                  break;
                              }
                          } else {
                              flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
                          }
                          delay(20);
                      }
                      if (settings.enableInterrupt) setInterruptMode(false);
                      isSpeaking = false;
                  }
              }
          }
      } else {
          Serial.println("[BME680] performReading() failed!");
      }
  }
  
  // Timer State Machine
  if (g_pendingTimerSeconds > 0) {
      timerStartTime = millis();
      timerDurationMs = g_pendingTimerSeconds * 1000;
      timerActive = true;
      timerRinging = false;
      if (settings.debugMode) Serial.printf("Timer set for %u seconds\n", g_pendingTimerSeconds);
      g_pendingTimerSeconds = 0;
  }
  
  if (g_cancelTimer) {
      timerActive = false;
      timerRinging = false;
      g_cancelTimer = false;
      if (settings.debugMode) Serial.println("Timer cancelled");
  }
  
  if (timerActive && (millis() - timerStartTime >= timerDurationMs)) {
      timerActive = false;
      timerRinging = true;
      lastRingTime = 0; // Trigger immediately
      if (settings.debugMode) Serial.println("Timer Expired!");
  }
  
  if (timerRinging && !isSpeaking && !isProcessing && !isWebServerActive && !g_isSubMenuActive) {
      if (millis() - lastRingTime > 5000) { // Repeat every 5 seconds
          lastRingTime = millis();
          
          const char* timerFile = (strcmp(settings.ttsFormat, "wav") == 0) ? "/timer.wav" : "/timer.mp3";
          if (!LittleFS.exists(timerFile)) {
              // Suppress status updates so the clock screen is not interrupted
              llm.downloadTTS("The timer has reached zero.", network, timerFile, ttsVoice);
          }
          
          speaker.playSpeechFromFile(timerFile);
          if (!isSpeaking) g_audioPlaybackStart = millis();
          isSpeaking = true;
          
          if (settings.enableInterrupt) setInterruptMode(true);
          bool interrupted = false;
          
          while(speaker.isRunning()) {
              if (settings.enableInterrupt) {
                  if (checkPlaybackBargeIn()) {
                      if (settings.debugMode) Serial.println("Timer playback interrupted!");
                      interrupted = true;
                      timerRinging = false; // Cancel timer via interrupt
                      lastInterruptTime = millis();
                      break;
                  }
                  delay(20);
              } else {
                  flushAecBuffer(); // Prevent AEC ringbuffer from overflowing
                  delay(50);
              }
          }
          
          if (settings.enableInterrupt) setInterruptMode(false);
          isSpeaking = false;
      }
  }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Alarm Clock State Machine
    static int lastAlarmDay = -1;
    if (settings.alarmEnabled && timeinfo.tm_hour == settings.alarmHour && timeinfo.tm_min == settings.alarmMinute) {
        if (lastAlarmDay != timeinfo.tm_yday && timeinfo.tm_year > 100) { // Time synced & not already triggered today
            alarmRinging = true;
            lastAlarmDay = timeinfo.tm_yday;
            if (settings.debugMode) Serial.println("Alarm Triggered!");
        }
    }

    if (alarmRinging && !isSpeaking && !isProcessing && !isWebServerActive && !g_isSubMenuActive) {
        if (millis() - lastAlarmRingTime > 5000) { // Repeat every 5 seconds
            lastAlarmRingTime = millis();
            
            const char* alarmFile = (strcmp(settings.ttsFormat, "wav") == 0) ? "/alarm.wav" : "/alarm.mp3";
            if (!LittleFS.exists(alarmFile)) {
                llm.downloadTTS("Alarm! It is time to wake up.", network, alarmFile, ttsVoice);
            }
            
            speaker.playSpeechFromFile(alarmFile);
            if (!isSpeaking) g_audioPlaybackStart = millis();
            isSpeaking = true;
            
            if (settings.enableInterrupt) setInterruptMode(true);
            
            while(speaker.isRunning()) {
                if (settings.enableInterrupt) {
                    if (checkPlaybackBargeIn()) {
                        alarmRinging = false; // Turn off ringing when user interrupts
                        break;
                    }
                } else {
                    flushAecBuffer();
                }
                delay(20);
            }
            if (settings.enableInterrupt) setInterruptMode(false);
            isSpeaking = false;
        }
    }

  // Check for Wake Word if not already speaking, in web config mode, or in a sub-menu
  if (!isSpeaking && !isWebServerActive && !g_isSubMenuActive) {
      // Allow any loud word (barge-in) to cancel the timer during the silence between rings
      bool tempInterrupt = false;
      if (timerRinging && settings.enableInterrupt) {
          setInterruptMode(true);
          tempInterrupt = true;
        } else if (alarmRinging && settings.enableInterrupt) {
            setInterruptMode(true);
            tempInterrupt = true;
      }

      if (speech.detectWakeWord(wakeThreshold)) {
          if (settings.debugMode) Serial.println("Wake Word Detected!");
          speaker.playSpeechFromFile(CHIME_FILENAME);
          
          while(speaker.isRunning()) delay(30);
          
          if (timerRinging) {
              timerRinging = false; // Cancel timer if they use the wake word during silence
              display.showStatus("Ready"); // Just return to standby
                    flushAecBuffer();
            } else if (alarmRinging) {
                alarmRinging = false; // Cancel alarm ringing
                display.showStatus("Ready");
                flushAecBuffer();
          } else {
              onVoiceChange("TALK_ACTION");
          }
      }
      
      if (tempInterrupt) {
          setInterruptMode(false);
      }
  }
  else if (!isSpeaking) {
      // If we are in a sub-menu or web config, we don't want to run the neural network,
      // but we MUST drain the AEC ringbuffer so it doesn't max out, overflow, and waste cycles!
      flushAecBuffer();
  }

  // Keep loop responsive
  delay(5);

  // Check if speaking finished
  if (isSpeaking && !speaker.isRunning()) {
      isSpeaking = false;
      if (!g_isSubMenuActive && !isWebServerActive) {
          String waitMsg = "Ready\nRSSI: " + String(network.getSignalStrength()) + " dBm";
          display.showStatus(waitMsg.c_str());
      }
  }
  handleSerialCommands();
  if (isWebServerActive) {
      server.handleClient();
  }
}