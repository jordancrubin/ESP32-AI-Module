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

CommandResult CommandProcessor::processCommand(const String& text) {
    CommandResult result = {false, "", false, false, false, false};
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
    } else if (lowerText.indexOf("test aec") != -1 || lowerText.indexOf("test a e c") != -1 || lowerText.indexOf("test echo cancellation") != -1 ||
               lowerText.indexOf("test the aec") != -1 || lowerText.indexOf("test the a e c") != -1 || lowerText.indexOf("test the echo cancellation") != -1) {
        result.response = "We will be conducting an A E C test, ensure there is no noise in the room. We will play back the results both with and without A E C.";
        result.handled = true;
        result.runAecTest = true;
    } else if (lowerText.indexOf("system reboot") != -1 || lowerText.indexOf("reboot the system") != -1 || lowerText.indexOf("reboot system") != -1) {
        result.response = "Rebooting the system now.";
        result.handled = true;
        result.systemReboot = true;
    } else if (lowerText.indexOf("show me your blue screen of death") != -1 || lowerText.indexOf("blue screen of death") != -1) {
        result.response = "Ok.";
        result.handled = true;
        result.showBSOD = true;
    } else if (lowerText.indexOf("show me your guru meditation") != -1 || lowerText.indexOf("guru meditation") != -1) {
        result.response = "Ok.";
        result.handled = true;
        result.showGuruMeditation = true;
    } else if (lowerText.indexOf("what are your commands") != -1 || lowerText.indexOf("list your commands") != -1 || lowerText == "help" || lowerText == "help." || lowerText == "help!") {
        result.response = "Here are the local commands I understand. You can tell me to raise, lower, or set the volume to a number between zero and ten. You can say, 'test A E C', to test the acoustic echo cancellation. You can also say, 'system reboot', to restart the device.";
        result.handled = true;
    }

    return result;
}