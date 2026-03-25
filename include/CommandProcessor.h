/*
  CommandProcessor.h - ESP32 AI Module
  Handles local voice command interception.
*/
#pragma once
#include <Arduino.h>

struct CommandResult {
    bool handled = false;
    String response = "";
    bool runAecTest = false;
    bool systemReboot = false;
    bool showBSOD = false;
    bool showGuruMeditation = false;
    bool calibrateTouch = false;
    bool enterConfigMode = false;
    bool tuneAEC = false;
    bool showHelp = false;
};

class CommandProcessor {
public:
    static CommandResult processCommand(const String& text);
};