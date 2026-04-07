/*
  CommandProcessor.cpp - ESP32 AI Module
  Handles local voice command interception.
*/
#include "CommandProcessor.h"
#include "SettingsManager.h"
#include "DisplayManager.h"

extern SettingsManager settings;
extern DisplayManager display;
extern String ttsVoice;
extern String voiceOptions;
extern void onVolumeChange(int value);

uint32_t g_pendingTimerSeconds = 0;
bool g_cancelTimer = false;

static int wordToInt(String word) {
    if (word == "one" || word == "a" || word == "an") return 1;
    if (word == "two") return 2;
    if (word == "three") return 3;
    if (word == "four") return 4;
    if (word == "five") return 5;
    if (word == "six") return 6;
    if (word == "seven") return 7;
    if (word == "eight") return 8;
    if (word == "nine") return 9;
    if (word == "ten") return 10;
    if (word == "eleven") return 11;
    if (word == "twelve") return 12;
    if (word == "thirteen") return 13;
    if (word == "fourteen") return 14;
    if (word == "fifteen") return 15;
    if (word == "sixteen") return 16;
    if (word == "seventeen") return 17;
    if (word == "eighteen") return 18;
    if (word == "nineteen") return 19;
    if (word == "twenty") return 20;
    if (word == "thirty") return 30;
    if (word == "forty") return 40;
    if (word == "fifty") return 50;
    if (word == "sixty") return 60;
    return word.toInt(); // fallback to numeric parsing
}

CommandResult CommandProcessor::processCommand(const String& text) {
    CommandResult result; // Values default safely to false/"" automatically
    String lowerText = text;
    lowerText.toLowerCase();

    // Local Command: Volume Control (0-10 Spoken Scale -> 0-21 Internal Scale)
    if (lowerText.indexOf("lower the volume") != -1 || lowerText.indexOf("decrease the volume") != -1 || lowerText.indexOf("volume down") != -1) {
        int currentScale = (settings.volume * 10 + 10) / 21; // Convert 0-21 to 0-10
        if (currentScale > 0) currentScale--;
        settings.volume = (currentScale * 21) / 10;
        onVolumeChange(settings.volume);
        display.showMainUI(ttsVoice, settings.volume, voiceOptions); // Update slider UI
        result.response = "The volume has been decreased to " + String(currentScale) + ".";
        result.handled = true;
    } else if (lowerText.indexOf("raise the volume") != -1 || lowerText.indexOf("increase the volume") != -1 || lowerText.indexOf("volume up") != -1) {
        int currentScale = (settings.volume * 10 + 10) / 21;
        if (currentScale < 10) currentScale++;
        settings.volume = (currentScale * 21) / 10;
        onVolumeChange(settings.volume);
        display.showMainUI(ttsVoice, settings.volume, voiceOptions); // Update slider UI
        result.response = "The volume has been increased to " + String(currentScale) + ".";
        result.handled = true;
    } else if (lowerText.indexOf("set volume to") != -1 || lowerText.indexOf("set the volume to") != -1) {
        int idx = lowerText.indexOf("volume to");
        String after = lowerText.substring(idx + 9);
        int targetVol = -1;
        
        if (after.indexOf("10") != -1 || after.indexOf("ten") != -1) targetVol = 10;
        else if (after.indexOf("9") != -1 || after.indexOf("nine") != -1) targetVol = 9;
        else if (after.indexOf("8") != -1 || after.indexOf("eight") != -1) targetVol = 8;
        else if (after.indexOf("7") != -1 || after.indexOf("seven") != -1) targetVol = 7;
        else if (after.indexOf("6") != -1 || after.indexOf("six") != -1) targetVol = 6;
        else if (after.indexOf("5") != -1 || after.indexOf("five") != -1) targetVol = 5;
        else if (after.indexOf("4") != -1 || after.indexOf("four") != -1) targetVol = 4;
        else if (after.indexOf("3") != -1 || after.indexOf("three") != -1) targetVol = 3;
        else if (after.indexOf("2") != -1 || after.indexOf("two") != -1 || after.indexOf("too") != -1) targetVol = 2; // Whisper sometimes spells "to too"
        else if (after.indexOf("1") != -1 || after.indexOf("one") != -1) targetVol = 1;
        else if (after.indexOf("0") != -1 || after.indexOf("zero") != -1) targetVol = 0;
        
        if (targetVol != -1) {
            settings.volume = (targetVol * 21) / 10;
            onVolumeChange(settings.volume);
            display.showMainUI(ttsVoice, settings.volume, voiceOptions); // Update slider UI
            result.response = "The volume has been set to " + String(targetVol) + ".";
        } else {
            result.response = "The volume is out of range. It must be between zero and ten.";
        }
        result.handled = true;
    } else if (lowerText.indexOf("lower the brightness") != -1 || lowerText.indexOf("decrease the brightness") != -1 || lowerText.indexOf("brightness down") != -1) {
        int currentScale = ((settings.brightness - 10) * 10 + 122) / 245; // Convert 10-255 to 0-10
        if (currentScale > 0) currentScale--;
        settings.brightness = 10 + (currentScale * 245) / 10;
        display.setBacklight(settings.brightness);
        settings.save();
        result.response = "The brightness has been decreased to " + String(currentScale) + ".";
        result.handled = true;
    } else if (lowerText.indexOf("raise the brightness") != -1 || lowerText.indexOf("increase the brightness") != -1 || lowerText.indexOf("brightness up") != -1) {
        int currentScale = ((settings.brightness - 10) * 10 + 122) / 245;
        if (currentScale < 10) currentScale++;
        settings.brightness = 10 + (currentScale * 245) / 10;
        display.setBacklight(settings.brightness);
        settings.save();
        result.response = "The brightness has been increased to " + String(currentScale) + ".";
        result.handled = true;
    } else if (lowerText.indexOf("set brightness to") != -1 || lowerText.indexOf("set the brightness to") != -1) {
        int idx = lowerText.indexOf("brightness to");
        String after = lowerText.substring(idx + 13);
        int targetBri = -1;
        
        if (after.indexOf("10") != -1 || after.indexOf("ten") != -1) targetBri = 10;
        else if (after.indexOf("9") != -1 || after.indexOf("nine") != -1) targetBri = 9;
        else if (after.indexOf("8") != -1 || after.indexOf("eight") != -1) targetBri = 8;
        else if (after.indexOf("7") != -1 || after.indexOf("seven") != -1) targetBri = 7;
        else if (after.indexOf("6") != -1 || after.indexOf("six") != -1) targetBri = 6;
        else if (after.indexOf("5") != -1 || after.indexOf("five") != -1) targetBri = 5;
        else if (after.indexOf("4") != -1 || after.indexOf("four") != -1) targetBri = 4;
        else if (after.indexOf("3") != -1 || after.indexOf("three") != -1) targetBri = 3;
        else if (after.indexOf("2") != -1 || after.indexOf("two") != -1 || after.indexOf("too") != -1) targetBri = 2; // Whisper sometimes spells "to too"
        else if (after.indexOf("1") != -1 || after.indexOf("one") != -1) targetBri = 1;
        else if (after.indexOf("0") != -1 || after.indexOf("zero") != -1) targetBri = 0;
        
        if (targetBri != -1) {
            settings.brightness = 10 + (targetBri * 245) / 10;
            display.setBacklight(settings.brightness);
            settings.save();
            result.response = "The brightness has been set to " + String(targetBri) + ".";
        } else {
            result.response = "The brightness is out of range. It must be between zero and ten.";
        }
        result.handled = true;
    } else if (lowerText.indexOf("set timer for") != -1 || lowerText.indexOf("set a timer for") != -1 || lowerText.indexOf("set the timer for") != -1 || 
               lowerText.indexOf("start timer for") != -1 || lowerText.indexOf("start a timer for") != -1 || lowerText.indexOf("start the timer for") != -1) {
        String cleanText = lowerText;
        cleanText.replace(".", " ");
        cleanText.replace(",", " ");
        cleanText.replace("!", " ");
        cleanText.replace("?", " ");
        
        uint32_t totalSecs = 0;
        int lastNum = 0;
        
        int start = 0;
        int end = cleanText.indexOf(' ');
        while (end != -1 || start < cleanText.length()) {
            String word = (end == -1) ? cleanText.substring(start) : cleanText.substring(start, end);
            word.trim();
            if (word.length() > 0) {
                if (word == "hour" || word == "hours") {
                    totalSecs += lastNum * 3600;
                    lastNum = 0;
                } else if (word == "minute" || word == "minutes" || word == "min" || word == "mins") {
                    totalSecs += lastNum * 60;
                    lastNum = 0;
                } else if (word == "second" || word == "seconds" || word == "sec" || word == "secs") {
                    totalSecs += lastNum;
                    lastNum = 0;
                } else if (word == "and") {
                    // Ignore filler word
                } else {
                    int val = wordToInt(word);
                    if (val > 0 || word == "0") {
                        if (lastNum >= 20 && val < 10) lastNum += val; // Handles "twenty five" -> 25
                        else lastNum = val;
                    }
                }
            }
            if (end == -1) break;
            start = end + 1;
            end = cleanText.indexOf(' ', start);
        }

        if (totalSecs > 0) {
            g_pendingTimerSeconds = totalSecs;
            
            String niceTime = "";
            int h = totalSecs / 3600;
            int m = (totalSecs % 3600) / 60;
            int s = totalSecs % 60;
            if (h > 0) niceTime += String(h) + (h > 1 ? " hours " : " hour ");
            if (m > 0) niceTime += String(m) + (m > 1 ? " minutes " : " minute ");
            if (s > 0) niceTime += String(s) + (s > 1 ? " seconds" : " second");
            niceTime.trim();
            
            result.response = "Timer set for " + niceTime + ".";
            result.handled = true;
        } else {
            result.response = "I couldn't understand the timer duration.";
            result.handled = true;
        }
    } else if (lowerText.indexOf("cancel timer") != -1 || lowerText.indexOf("stop timer") != -1 || lowerText.indexOf("cancel the timer") != -1 || lowerText.indexOf("stop the timer") != -1) {
        g_cancelTimer = true;
        result.response = "The timer has been cancelled.";
        result.handled = true;
    } else if (lowerText.indexOf("clock color to") != -1 || lowerText.indexOf("clock colour to") != -1) {
        String newColor = "";
        if (lowerText.indexOf("red") != -1) newColor = "red";
        else if (lowerText.indexOf("green") != -1) newColor = "green";
        else if (lowerText.indexOf("white") != -1) newColor = "white";
        
        if (newColor != "") {
            strlcpy(settings.clockColor, newColor.c_str(), sizeof(settings.clockColor));
            settings.save();
            result.response = "The clock color has been set to " + newColor + ".";
        } else {
            result.response = "I can only set the clock color to red, green, or white.";
        }
        result.handled = true;
    } else if (lowerText.indexOf("test aec") != -1 || lowerText.indexOf("test a e c") != -1 || lowerText.indexOf("test echo cancellation") != -1 ||
               lowerText.indexOf("test the aec") != -1 || lowerText.indexOf("test the a e c") != -1 || lowerText.indexOf("test the echo cancellation") != -1) {
        result.response = "We will be conducting an A E C test, ensure there is no noise in the room. We will play back the results both with and without A E C.";
        result.handled = true;
        result.runAecTest = true;
    } else if (lowerText.indexOf("tune aec") != -1 || lowerText.indexOf("tune a e c") != -1 || 
               lowerText.indexOf("auto tune") != -1 || lowerText.indexOf("auto-tune") != -1) {
        result.response = "Starting automatic A E C calibration. Please remain completely silent for the next fifteen seconds.";
        result.handled = true;
        result.tuneAEC = true;
    } else if (lowerText.indexOf("system reboot") != -1 || lowerText.indexOf("reboot the system") != -1 || lowerText.indexOf("reboot system") != -1) {
        result.response = "Rebooting the system now.";
        result.handled = true;
        result.systemReboot = true;
    } else if (lowerText.indexOf("show me your blue screen of death") != -1 || lowerText.indexOf("show me a blue screen of death") != -1 || lowerText.indexOf("blue screen of death") != -1) {
        result.response = "Ok.";
        result.handled = true;
        result.showBSOD = true;
    } else if (lowerText.indexOf("show me your guru meditation") != -1 || lowerText.indexOf("show me a guru meditation") != -1 || lowerText.indexOf("guru meditation") != -1) {
        result.response = "Ok.";
        result.handled = true;
        result.showGuruMeditation = true;
    } else if (lowerText.indexOf("calibrate the touchscreen") != -1 || lowerText.indexOf("recalibrate the touchscreen") != -1 || lowerText.indexOf("calibrate touchscreen") != -1) {
        result.response = "Calibrating the touchscreen now. Please tap the red dots on the screen.";
        result.handled = true;
        result.calibrateTouch = true;
    } else if (lowerText.indexOf("go into configuration mode") != -1 || lowerText.indexOf("configuration mode") != -1) {
        result.response = "Entering configuration mode, you must manually exit.";
        result.handled = true;
        result.enterConfigMode = true;
    } else if (lowerText.indexOf("what are your commands") != -1 || lowerText.indexOf("list your commands") != -1 || lowerText == "help" || lowerText == "help." || lowerText == "help!") {
        result.response = "Here are the local commands I understand. You can tell me to raise, lower, or set the volume or brightness to a number between zero and ten. You can ask me to change the clock color to red, green, or white. You can ask me to calibrate the touchscreen, or go into configuration mode. You can say, 'test A E C', to test the acoustic echo cancellation. You can also say, 'system reboot', to restart the device.";
        result.handled = true;
        result.showHelp = true;
    }

    return result;
}